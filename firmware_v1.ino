/**
 * firmware_v1.ino  --  Projeto Motiva | CP2 | Firmware 1.0
 * No IoT de vegetacao: sessao de 5 leituras a cada 48 s, media e LED azul.
 * Apos 3 sessoes consulta version.json no GitHub e, se houver versao nova, faz OTA.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>

// ===== BLOCO 1: configuracao (versao, rede, repositorio, pinos) =====
const char* FW_VERSION = "1.0";

const char* WIFI_SSID  = "Wokwi-GUEST";
const char* WIFI_PASS  = "";
const int   WIFI_CANAL = 6;

// Manifesto no repositorio publico do grupo (GitHub raw)
const char* MANIFEST_URL =
  "https://raw.githubusercontent.com/Luisin07/motiva-ota-cp2/main/version.json";

// 0 = normal | 1 = sem Wi-Fi | 2 = manifesto inacessivel | 3 = .bin inacessivel | 4 = erro na gravacao OTA
#define MODO_TESTE_ERRO 0

const int PIN_LED_R = 25;
const int PIN_LED_G = 26;
const int PIN_LED_B = 27;

const int           N_LEITURAS           = 5;
const unsigned long INTERVALO_LEITURA_MS = 2000;
const unsigned long INTERVALO_SESSAO_MS  = 48000;
const int           SESSOES_ANTES_OTA    = 3;
const int           ALTURA_MIN_CM        = 10;
const int           ALTURA_MAX_CM        = 20;

// ===== BLOCO 2: estado global da sessao =====
int           leituras[N_LEITURAS];
int           indiceLeitura       = 0;
int           numeroSessao        = 0;
int           sessoesConcluidas   = 0;
bool          sessaoEmAndamento   = false;
bool          otaPendente         = false;
bool          otaResolvida        = false;
unsigned long inicioSessaoMs      = 0;
unsigned long proximaLeituraMs    = 0;

// ===== BLOCO 3: LED RGB =====
void setLed(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? HIGH : LOW);
  digitalWrite(PIN_LED_G, g ? HIGH : LOW);
  digitalWrite(PIN_LED_B, b ? HIGH : LOW);
}

void ledVersao1()      { setLed(false, false, true); }
void ledAtualizando()  { setLed(true,  false, true); }

// ===== BLOCO 4: banner e informacoes do boot =====
void imprimirBanner() {
  const esp_partition_t* part = esp_ota_get_running_partition();
  Serial.println();
  Serial.println("========================================");
  Serial.println(" MONITORAMENTO DE VEGETACAO - FW 1.0");
  Serial.println("========================================");
  Serial.printf("Versao instalada : %s\n", FW_VERSION);
  Serial.printf("Particao em uso  : %s (0x%06X)\n", part->label, part->address);
  Serial.printf("Build            : %s %s\n", __DATE__, __TIME__);
  Serial.printf("OTA sera verificada apos %d sessoes.\n", SESSOES_ANTES_OTA);
  Serial.println("LED AZUL = Firmware 1.0 em execucao");
  Serial.println("========================================");
}

// ===== BLOCO 5: conexao Wi-Fi (com timeout, sem travar o firmware) =====
bool conectarWiFi(unsigned long timeoutMs = 10000) {
  if (WiFi.status() == WL_CONNECTED) return true;

  const char* ssid = (MODO_TESTE_ERRO == 1) ? "REDE-INEXISTENTE" : WIFI_SSID;
  Serial.printf("[WIFI] Conectando a '%s'...", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, WIFI_PASS, WIFI_CANAL);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < timeoutMs) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI][ERRO] Sem conexao Wi-Fi. O monitoramento continua offline.");
    WiFi.disconnect();
    return false;
  }
  Serial.printf("[WIFI] Conectado. IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

// ===== BLOCO 6: comparacao de versoes (formato MAJOR.MINOR) =====
int compararVersoes(const String& a, const String& b) {
  int aMaj = 0, aMin = 0, bMaj = 0, bMin = 0;
  sscanf(a.c_str(), "%d.%d", &aMaj, &aMin);
  sscanf(b.c_str(), "%d.%d", &bMaj, &bMin);
  if (aMaj != bMaj) return (aMaj > bMaj) ? 1 : -1;
  if (aMin != bMin) return (aMin > bMin) ? 1 : -1;
  return 0;
}

// ===== BLOCO 7: consulta do manifesto version.json =====
bool consultarManifesto(String& versaoRemota, String& urlFirmware, String& md5) {
  String url = MANIFEST_URL;
  if (MODO_TESTE_ERRO == 2) url.replace("version.json", "arquivo-que-nao-existe.json");

  Serial.printf("[OTA] Consultando manifesto: %s\n", url.c_str());

  WiFiClientSecure client;
  client.setInsecure();  // simplificacao de laboratorio: em campo validar o certificado do servidor
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  if (!http.begin(client, url)) {
    Serial.println("[OTA][ERRO] Nao foi possivel iniciar a requisicao HTTP do manifesto.");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA][ERRO] Manifesto inacessivel (HTTP %d: %s).\n",
                  code, http.errorToString(code).c_str());
    http.end();
    return false;
  }

  String corpo = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, corpo);
  if (err || !doc["version"].is<const char*>() || !doc["url"].is<const char*>()) {
    Serial.printf("[OTA][ERRO] Manifesto invalido (%s).\n", err ? err.c_str() : "campos ausentes");
    return false;
  }

  versaoRemota = doc["version"].as<String>();
  urlFirmware  = doc["url"].as<String>();
  md5          = doc["md5"] | "";
  if (MODO_TESTE_ERRO == 3) urlFirmware.replace("firmware_v2.bin", "firmware_inexistente.bin");
  if (MODO_TESTE_ERRO == 4) urlFirmware = url;  // baixa o JSON como se fosse .bin: Update rejeita

  Serial.printf("[OTA] Versao disponivel: %s | URL: %s\n", versaoRemota.c_str(), urlFirmware.c_str());
  return true;
}

// ===== BLOCO 8: download do .bin e gravacao OTA na particao livre =====
bool baixarEGravarFirmware(const String& urlFirmware, const String& md5) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(30000);

  Serial.println("[OTA] Baixando firmware...");
  if (!http.begin(client, urlFirmware)) {
    Serial.println("[OTA][ERRO] Nao foi possivel iniciar o download do firmware.");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA][ERRO] Firmware nao pode ser baixado (HTTP %d: %s).\n",
                  code, http.errorToString(code).c_str());
    http.end();
    return false;
  }

  int tamanho = http.getSize();
  if (tamanho <= 0) {
    Serial.println("[OTA][ERRO] Tamanho do firmware desconhecido ou vazio.");
    http.end();
    return false;
  }
  Serial.printf("[OTA] Tamanho do arquivo: %d bytes\n", tamanho);

  const esp_partition_t* destino = esp_ota_get_next_update_partition(NULL);
  Serial.printf("[OTA] Gravando na particao: %s (0x%06X)\n", destino->label, destino->address);

  if (!Update.begin(tamanho)) {
    Serial.printf("[OTA][ERRO] Update.begin falhou: %s\n", Update.errorString());
    http.end();
    return false;
  }
  // MD5 do manifesto: Update.end() recusa o arquivo se ele chegar corrompido
  if (md5.length() == 32 && Update.setMD5(md5.c_str()))
    Serial.printf("[OTA] Integridade sera validada por MD5: %s\n", md5.c_str());

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[1024];
  size_t  escrito = 0;
  int     ultimoPct = -1;
  unsigned long ultimoDado = millis();

  while (escrito < (size_t)tamanho && millis() - ultimoDado < 20000) {
    size_t disponivel = stream->available();
    if (disponivel == 0) { delay(1); continue; }

    size_t lidos = stream->readBytes(buffer, min(disponivel, sizeof(buffer)));
    if (Update.write(buffer, lidos) != lidos) {
      Serial.printf("\n[OTA][ERRO] Falha na gravacao: %s\n", Update.errorString());
      Update.abort();
      http.end();
      return false;
    }
    escrito += lidos;
    ultimoDado = millis();

    int pct = (escrito * 100) / tamanho;
    if (pct / 10 != ultimoPct / 10) {
      Serial.printf("[OTA] Progresso: %3d%% (%u/%d bytes)\n", pct, escrito, tamanho);
      ultimoPct = pct;
    }
  }
  http.end();

  if (escrito != (size_t)tamanho) {
    Serial.printf("[OTA][ERRO] Download incompleto (%u de %d bytes).\n", escrito, tamanho);
    Update.abort();
    return false;
  }

  if (!Update.end(true) || !Update.isFinished()) {
    Serial.printf("[OTA][ERRO] O processo de atualizacao retornou erro: %s\n", Update.errorString());
    return false;
  }

  Serial.println("[OTA] Firmware gravado e validado com sucesso.");
  return true;
}

// ===== BLOCO 9: registro em NVS (v2 le isso apos o reboot para rastreabilidade) =====
void registrarAtualizacao(const String& versaoNova) {
  Preferences prefs;
  prefs.begin("ota", false);
  prefs.putString("versao_ant", FW_VERSION);
  prefs.putString("versao_nova", versaoNova);
  prefs.putString("manifesto", MANIFEST_URL);
  prefs.putUInt("sessoes_v1", sessoesConcluidas);
  prefs.end();
}

// ===== BLOCO 10: fluxo completo de verificacao OTA =====
void verificarAtualizacao() {
  Serial.println();
  Serial.println("----------------------------------------");
  Serial.println("[OTA] Verificando atualizacao remota...");
  Serial.println("----------------------------------------");
  otaPendente = false;

  if (!conectarWiFi()) {
    Serial.println("[OTA] Nova tentativa apos a proxima sessao.");
    return;
  }

  String versaoRemota, urlFirmware, md5;
  if (!consultarManifesto(versaoRemota, urlFirmware, md5)) {
    Serial.println("[OTA] Nova tentativa apos a proxima sessao.");
    return;
  }

  Serial.printf("[OTA] Instalada: %s | Disponivel: %s\n", FW_VERSION, versaoRemota.c_str());
  if (compararVersoes(versaoRemota, FW_VERSION) <= 0) {
    Serial.println("[OTA] A versao instalada ja e a mais recente. Nada a fazer.");
    otaResolvida = true;
    return;
  }

  Serial.println("[OTA] Atualizacao disponivel! Iniciando OTA...");
  ledAtualizando();

  if (!baixarEGravarFirmware(urlFirmware, md5)) {
    ledVersao1();
    Serial.println("[OTA] Atualizacao cancelada. Firmware 1.0 segue em execucao.");
    Serial.println("[OTA] Nova tentativa apos a proxima sessao.");
    return;
  }

  registrarAtualizacao(versaoRemota);
  Serial.println("[OTA] Reiniciando o ESP32 em 3 segundos...");
  Serial.flush();
  delay(3000);
  ESP.restart();
}

// ===== BLOCO 11: sessao de medicao (vetor, leituras e media) =====
int gerarLeitura() {
  return random(ALTURA_MIN_CM, ALTURA_MAX_CM + 1);
}

float calcularMedia(const int* valores, int n) {
  long soma = 0;
  for (int i = 0; i < n; i++) soma += valores[i];
  return (float)soma / n;
}

void iniciarSessao(unsigned long agora) {
  unsigned long anterior = inicioSessaoMs;
  numeroSessao++;
  inicioSessaoMs    = agora;
  proximaLeituraMs  = agora;
  indiceLeitura     = 0;
  sessaoEmAndamento = true;

  Serial.println();
  Serial.println("========================================");
  Serial.println("MONITORAMENTO DE VEGETACAO - FW 1.0");
  Serial.println("========================================");
  Serial.printf("Sessao #%d  (t = %.1f s desde o boot)\n", numeroSessao, millis() / 1000.0);
  if (numeroSessao > 1)
    Serial.printf("Intervalo desde o inicio da sessao anterior: %.1f s\n", (millis() - anterior) / 1000.0);
}

void realizarLeitura() {
  leituras[indiceLeitura] = gerarLeitura();
  Serial.printf("Leitura %d: %d cm\n", indiceLeitura + 1, leituras[indiceLeitura]);
  indiceLeitura++;
  proximaLeituraMs += INTERVALO_LEITURA_MS;
}

void finalizarSessao() {
  sessaoEmAndamento = false;
  sessoesConcluidas++;

  Serial.printf("Media da sessao: %.1f cm\n", calcularMedia(leituras, N_LEITURAS));
  Serial.println("Proxima sessao em 48 segundos.");

  if (sessoesConcluidas >= SESSOES_ANTES_OTA && !otaResolvida) otaPendente = true;
}

// ===== BLOCO 12: setup e loop (agendamento por millis, sem delays longos) =====
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  ledVersao1();
  randomSeed(esp_random());

  imprimirBanner();
  conectarWiFi();

  iniciarSessao(millis());
}

void loop() {
  unsigned long agora = millis();

  if (sessaoEmAndamento) {
    if (agora >= proximaLeituraMs) {
      realizarLeitura();
      if (indiceLeitura >= N_LEITURAS) finalizarSessao();
    }
  } else if (otaPendente) {
    verificarAtualizacao();
  }

  // proxima sessao conta a partir do INICIO da anterior (48 s), nao do fim da 5a leitura
  if (!sessaoEmAndamento && agora - inicioSessaoMs >= INTERVALO_SESSAO_MS) {
    iniciarSessao(inicioSessaoMs + INTERVALO_SESSAO_MS);
  }
}
