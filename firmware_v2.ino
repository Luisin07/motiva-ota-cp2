/**
 * firmware_v2.ino  --  Projeto Motiva | CP2 | Firmware 2.0 (entregue via OTA)
 * Mantem o FW 1.0 (5 leituras / 2 s, sessao a cada 48 s, media) e acrescenta
 * ordenacao, mediana, histerese NORMAL/ALERTA e LED verde/vermelho.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>

// ===== BLOCO 1: configuracao (versao, rede, pinos, limites) =====
const char* FW_VERSION = "2.0";

const char* WIFI_SSID  = "Wokwi-GUEST";
const char* WIFI_PASS  = "";
const int   WIFI_CANAL = 6;

const int PIN_LED_R = 25;
const int PIN_LED_G = 26;
const int PIN_LED_B = 27;

const int           N_LEITURAS           = 5;
const unsigned long INTERVALO_LEITURA_MS = 2000;
const unsigned long INTERVALO_SESSAO_MS  = 48000;
const int           ALTURA_MIN_CM        = 10;
const int           ALTURA_MAX_CM        = 20;

const float LIMITE_ALERTA_CM = 16.0;
const float LIMITE_NORMAL_CM = 14.0;

// ===== BLOCO 2: roteiro de testes da histerese (sessoes 1..5 deterministicas, depois aleatorio) =====
#define ROTEIRO_TESTES true
const int N_SESSOES_ROTEIRO = 5;
const int ROTEIRO[N_SESSOES_ROTEIRO][N_LEITURAS] = {
  {13, 12, 14, 15, 11},  // mediana 13 -> NORMAL        (teste 8)
  {18, 16, 17, 19, 15},  // mediana 17 -> ALERTA        (teste 6)
  {15, 14, 16, 15, 13},  // mediana 15 -> mantem ALERTA (teste 7)
  {12, 14, 13, 15, 11},  // mediana 13 -> NORMAL        (teste 8)
  {16, 14, 15, 13, 17},  // mediana 15 -> mantem NORMAL (teste 7)
};

// ===== BLOCO 3: estado global =====
enum EstadoVegetacao { NORMAL, ALERTA };

int             leituras[N_LEITURAS];
int             ordenadas[N_LEITURAS];
int             indiceLeitura     = 0;
int             numeroSessao      = 0;
bool            sessaoEmAndamento = false;
bool            checarVersao      = false;
EstadoVegetacao estado            = NORMAL;
unsigned long   inicioSessaoMs    = 0;
unsigned long   proximaLeituraMs  = 0;
String          manifestoUrl      = "";

// ===== BLOCO 4: LED RGB (FW 2.0: verde = NORMAL, vermelho = ALERTA) =====
void setLed(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? HIGH : LOW);
  digitalWrite(PIN_LED_G, g ? HIGH : LOW);
  digitalWrite(PIN_LED_B, b ? HIGH : LOW);
}

void atualizarLed() {
  if (estado == ALERTA) setLed(true, false, false);
  else                  setLed(false, true, false);
}

const char* nomeEstado(EstadoVegetacao e) {
  return (e == ALERTA) ? "ALERTA" : "NORMAL";
}

// ===== BLOCO 5: banner e comprovacao da atualizacao OTA (dados gravados pelo FW 1.0 em NVS) =====
void imprimirBanner() {
  const esp_partition_t* part = esp_ota_get_running_partition();
  Serial.println();
  Serial.println("########################################");
  Serial.println(" MONITORAMENTO DE VEGETACAO - FW 2.0");
  Serial.println("########################################");
  Serial.printf("Versao instalada : %s\n", FW_VERSION);
  Serial.printf("Particao em uso  : %s (0x%06X)\n", part->label, part->address);
  Serial.printf("Build            : %s %s\n", __DATE__, __TIME__);

  Preferences prefs;
  prefs.begin("ota", true);
  String anterior = prefs.getString("versao_ant", "");
  manifestoUrl    = prefs.getString("manifesto", "");
  uint32_t sessV1 = prefs.getUInt("sessoes_v1", 0);
  prefs.end();

  if (anterior.length() > 0) {
    Serial.printf("OTA              : atualizado de %s para %s (apos %u sessoes no FW %s)\n",
                  anterior.c_str(), FW_VERSION, sessV1, anterior.c_str());
    Serial.printf("Origem           : %s\n", manifestoUrl.c_str());
  } else {
    Serial.println("OTA              : sem registro de atualizacao (gravado direto?)");
  }

  Serial.println("Novidades        : ordenacao, mediana, histerese NORMAL/ALERTA");
  Serial.printf("Histerese        : ALERTA se mediana >= %.0f | NORMAL se mediana <= %.0f | entre: mantem\n",
                LIMITE_ALERTA_CM, LIMITE_NORMAL_CM);
  Serial.println("LED              : VERDE = NORMAL | VERMELHO = ALERTA");
  if (ROTEIRO_TESTES)
    Serial.printf("Roteiro de testes: sessoes 1 a %d com valores fixos p/ comprovar a histerese\n",
                  N_SESSOES_ROTEIRO);
  Serial.println("########################################");
}

// ===== BLOCO 6: Wi-Fi e checagem de versao (comprova o cenario "ja e a mais recente") =====
bool conectarWiFi(unsigned long timeoutMs = 10000) {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.printf("[WIFI] Conectando a '%s'...", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CANAL);
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

int compararVersoes(const String& a, const String& b) {
  int aMaj = 0, aMin = 0, bMaj = 0, bMin = 0;
  sscanf(a.c_str(), "%d.%d", &aMaj, &aMin);
  sscanf(b.c_str(), "%d.%d", &bMaj, &bMin);
  if (aMaj != bMaj) return (aMaj > bMaj) ? 1 : -1;
  if (aMin != bMin) return (aMin > bMin) ? 1 : -1;
  return 0;
}

void verificarVersao() {
  Serial.println();
  Serial.println("[OTA] Verificando se ha versao mais nova...");
  if (manifestoUrl.length() == 0) {
    Serial.println("[OTA] URL do manifesto nao registrada. Verificacao ignorada.");
    return;
  }
  if (!conectarWiFi()) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  if (!http.begin(client, manifestoUrl)) {
    Serial.println("[OTA][ERRO] Nao foi possivel iniciar a requisicao do manifesto.");
    return;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA][ERRO] Manifesto inacessivel (HTTP %d: %s).\n", code, http.errorToString(code).c_str());
    http.end();
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err || !doc["version"].is<const char*>()) {
    Serial.println("[OTA][ERRO] Manifesto invalido.");
    return;
  }

  String remota = doc["version"].as<String>();
  Serial.printf("[OTA] Instalada: %s | Disponivel: %s\n", FW_VERSION, remota.c_str());
  if (compararVersoes(remota, FW_VERSION) <= 0)
    Serial.println("[OTA] A versao instalada ja e a mais recente. Nenhuma atualizacao necessaria.");
  else
    Serial.println("[OTA] Existe versao mais nova publicada (fora do escopo deste CP).");
}

// ===== BLOCO 7: processamento de dados (media, ordenacao propria, mediana) =====
float calcularMedia(const int* valores, int n) {
  long soma = 0;
  for (int i = 0; i < n; i++) soma += valores[i];
  return (float)soma / n;
}

// insertion sort sobre uma COPIA: o vetor original fica preservado para exibicao
void ordenarCopia(const int* origem, int* destino, int n) {
  for (int i = 0; i < n; i++) destino[i] = origem[i];
  for (int i = 1; i < n; i++) {
    int chave = destino[i];
    int j = i - 1;
    while (j >= 0 && destino[j] > chave) {
      destino[j + 1] = destino[j];
      j--;
    }
    destino[j + 1] = chave;
  }
}

int calcularMediana(const int* ordenado, int n) {
  return ordenado[n / 2];
}

void imprimirVetor(const char* rotulo, const int* v, int n) {
  Serial.print(rotulo);
  for (int i = 0; i < n; i++) Serial.printf("%d ", v[i]);
  Serial.println();
}

// ===== BLOCO 8: histerese baseada na mediana =====
void aplicarHisterese(int mediana) {
  EstadoVegetacao anterior = estado;

  if (mediana >= LIMITE_ALERTA_CM)      estado = ALERTA;
  else if (mediana <= LIMITE_NORMAL_CM) estado = NORMAL;
  // faixa morta (14,16): mantem o estado para nao oscilar perto do limite

  const char* regra;
  if (mediana >= LIMITE_ALERTA_CM)      regra = "mediana >= 16 cm -> entrar em ALERTA";
  else if (mediana <= LIMITE_NORMAL_CM) regra = "mediana <= 14 cm -> entrar/retornar a NORMAL";
  else                                  regra = "14 < mediana < 16 cm -> manter estado anterior";

  Serial.printf("Histerese: %s\n", regra);
  if (estado != anterior)
    Serial.printf("Estado: %s -> %s  (MUDOU)\n", nomeEstado(anterior), nomeEstado(estado));
  else
    Serial.printf("Estado: %s (mantido)\n", nomeEstado(estado));

  atualizarLed();
  Serial.printf("LED: %s\n", estado == ALERTA ? "VERMELHO" : "VERDE");
}

// ===== BLOCO 9: sessao de medicao =====
bool sessaoRoteirizada() {
  return ROTEIRO_TESTES && numeroSessao <= N_SESSOES_ROTEIRO;
}

int gerarLeitura() {
  if (sessaoRoteirizada()) return ROTEIRO[numeroSessao - 1][indiceLeitura];
  return random(ALTURA_MIN_CM, ALTURA_MAX_CM + 1);
}

void iniciarSessao(unsigned long agendado) {
  unsigned long anterior = inicioSessaoMs;
  numeroSessao++;
  inicioSessaoMs    = agendado;
  proximaLeituraMs  = agendado;
  indiceLeitura     = 0;
  sessaoEmAndamento = true;

  Serial.println();
  Serial.println("========================================");
  Serial.println("MONITORAMENTO DE VEGETACAO - FW 2.0");
  Serial.println("========================================");
  Serial.printf("Sessao #%d  (t = %.1f s desde o boot)%s\n", numeroSessao, millis() / 1000.0,
                sessaoRoteirizada() ? "  [ROTEIRO DE TESTE]" : "");
  if (numeroSessao > 1)
    Serial.printf("Intervalo desde o inicio da sessao anterior: %.1f s\n", (millis() - anterior) / 1000.0);
  Serial.printf("Estado atual: %s\n", nomeEstado(estado));
}

void realizarLeitura() {
  leituras[indiceLeitura] = gerarLeitura();
  Serial.printf("Leitura %d: %d cm\n", indiceLeitura + 1, leituras[indiceLeitura]);
  indiceLeitura++;
  proximaLeituraMs += INTERVALO_LEITURA_MS;
}

void finalizarSessao() {
  sessaoEmAndamento = false;

  ordenarCopia(leituras, ordenadas, N_LEITURAS);
  int mediana = calcularMediana(ordenadas, N_LEITURAS);

  Serial.println("----------------------------------------");
  imprimirVetor("Ordem original : ", leituras,  N_LEITURAS);
  imprimirVetor("Ordem crescente: ", ordenadas, N_LEITURAS);
  Serial.printf("Media da sessao  : %.1f cm\n", calcularMedia(leituras, N_LEITURAS));
  Serial.printf("Mediana da sessao: %d cm\n", mediana);
  aplicarHisterese(mediana);
  Serial.println("Proxima sessao em 48 segundos.");

  if (numeroSessao == 1) checarVersao = true;
}

// ===== BLOCO 10: setup e loop (agendamento por millis) =====
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  atualizarLed();
  randomSeed(esp_random());

  imprimirBanner();
  iniciarSessao(millis());
}

void loop() {
  unsigned long agora = millis();

  if (sessaoEmAndamento) {
    if (agora >= proximaLeituraMs) {
      realizarLeitura();
      if (indiceLeitura >= N_LEITURAS) finalizarSessao();
    }
  } else if (checarVersao) {
    checarVersao = false;
    verificarVersao();
  }

  // proxima sessao conta a partir do INICIO da anterior (48 s), nao do fim da 5a leitura
  if (!sessaoEmAndamento && agora - inicioSessaoMs >= INTERVALO_SESSAO_MS) {
    iniciarSessao(inicioSessaoMs + INTERVALO_SESSAO_MS);
  }
}
