# Projeto Motiva – CP2 | Atualização Remota de Firmware (OTA)

Nó IoT de monitoramento de altura da vegetação com **ESP32 simulado no Wokwi**, que roda o **Firmware 1.0**, consulta um manifesto (`version.json`) neste repositório, baixa o `firmware_v2.bin` por HTTPS, grava via OTA, reinicia e passa a rodar o **Firmware 2.0**.

## Integrantes

| Nome | RM |
|---|---|
| Luis Otavio Santini | 563556 |
| Vitor Barbosa Paiva | 565303 |
| Arthur Traldi Felix | 563477 |
| Lucas Andrade de Souza | 564066 |

## Links

- **Projeto Wokwi:** https://wokwi.com/projects/475817234852771841
- **Repositório OTA:** `https://github.com/Luisin07/motiva-ota-cp2`
- **Manifesto:** `https://raw.githubusercontent.com/Luisin07/motiva-ota-cp2/main/version.json`

## Estrutura do repositório

```
motiva-ota-cp2/
├── version.json        # manifesto: versão disponível, URL do .bin, MD5 e tamanho
├── firmware_v2.bin     # binário do FW 2.0 (baixado pelo ESP32 durante o OTA)
├── firmware_v1.ino     # código-fonte FW 1.0 (roda no Wokwi e faz o OTA)
├── firmware_v2.ino     # código-fonte FW 2.0 (compilado -> firmware_v2.bin)
├── firmware_v1_wokwi.bin # FW 1.0 compilado (imagem completa) p/ upload no Wokwi
├── wokwi/
│   ├── sketch.ino      # = firmware_v1.ino
│   ├── diagram.json    # ESP32 DevKit C v4 + LED RGB (catodo comum) + 3 resistores 220 Ω
│   └── libraries.txt   # ArduinoJson
└── README.md
```

## Arquitetura

```
 ESP32 (Wokwi)            Wokwi-GUEST / Internet            GitHub (raw)
 ┌──────────────┐   HTTPS GET version.json    ┌──────────────────────┐
 │ Firmware 1.0 │ ──────────────────────────▶ │ version.json         │
 │  app0        │ ◀── {version, url, md5} ─── │                      │
 │              │   HTTPS GET firmware_v2.bin │ firmware_v2.bin      │
 │  Update ────▶│ ◀────────────────────────── │                      │
 │  grava app1  │                             └──────────────────────┘
 └──────┬───────┘
        │ ESP.restart()
 ┌──────▼───────┐
 │ Firmware 2.0 │  boot pela partição app1
 └──────────────┘
```

Fluxo: **consultar versão → comparar → baixar .bin → validar (MD5) → gravar na partição OTA livre → reiniciar**.

- A tabela de partições padrão do ESP32 tem duas partições de aplicação (`app0`/`app1`). O FW 1.0 roda em `app0` e grava o FW 2.0 em `app1`; ao final, `Update.end()` marca `app1` como partição de boot. Se qualquer etapa falhar, `app0` continua intacta e o FW 1.0 segue rodando.
- Antes de reiniciar, o FW 1.0 grava em NVS (memória não-volátil, que o OTA não apaga) a versão anterior e a URL do manifesto. O FW 2.0 lê isso no boot e imprime **"atualizado de 1.0 para 2.0"** e a partição em uso — é a prova de que ele veio pelo OTA.

## Firmware 1.0

- Sessão a cada **48 s contados do início da sessão anterior** (agendamento por `millis()`, sem `delay` longo): leituras em 0, 2, 4, 6 e 8 s; próxima sessão em 48 s (não 56 s). O Serial imprime o intervalo medido entre sessões.
- 5 leituras pseudoaleatórias entre 10 e 20 cm guardadas em vetor, cada uma exibida no Serial, e a média.
- **LED azul** = FW 1.0.
- Depois de **3 sessões** completas: conecta ao Wi-Fi, consulta o manifesto, compara versões (`MAJOR.MINOR`) e, se houver versão nova, faz o OTA. Durante o download o LED fica **magenta**.

## Firmware 2.0 (evolução funcional)

Mantém tudo do 1.0 e acrescenta:

- **Ordenação** de uma cópia do vetor (insertion sort implementado no código) e exibição da ordem original e crescente.
- **Mediana** = 3º elemento do vetor ordenado.
- **Histerese** sobre a mediana: `>= 16 cm` → ALERTA; `<= 14 cm` → NORMAL; entre 14 e 16 → mantém o estado anterior.
- **LED verde** = NORMAL, **LED vermelho** = ALERTA.
- **Roteiro de testes:** as 5 primeiras sessões usam valores fixos para comprovar os testes 6, 7 e 8 de forma reproduzível; a partir da 6ª sessão os valores voltam a ser aleatórios (`ROTEIRO_TESTES` no código).

| Sessão | Leituras | Ordenado | Mediana | Resultado esperado |
|---|---|---|---|---|
| 1 | 13 12 14 15 11 | 11 12 13 14 15 | 13 | NORMAL (verde) – teste 8 |
| 2 | 18 16 17 19 15 | 15 16 17 18 19 | 17 | ALERTA (vermelho) – teste 6 |
| 3 | 15 14 16 15 13 | 13 14 15 15 16 | 15 | mantém ALERTA – teste 7 |
| 4 | 12 14 13 15 11 | 11 12 13 14 15 | 13 | volta a NORMAL – teste 8 |
| 5 | 16 14 15 13 17 | 13 14 15 16 17 | 15 | mantém NORMAL – teste 7 |

- Após a 1ª sessão consulta o manifesto de novo e mostra **"A versão instalada já é a mais recente"**.

### Média x mediana x histerese

- **Média:** soma ÷ 5. Sensível a leitura absurda (um pico puxa o valor).
- **Mediana:** valor central depois de ordenar. Ignora um valor extremo, por isso é mais robusta para decidir o estado.
- **Histerese:** dois limites (14 e 16) com uma faixa morta entre eles. Evita que o estado fique alternando NORMAL/ALERTA quando a vegetação está perto do limite.

### Por que OTA em campo?

Os nós ficam espalhados ao longo da via, em locais de acesso difícil. Com OTA dá para corrigir bugs, ajustar limites e publicar funções novas sem mandar técnico até cada equipamento, reduzindo custo, tempo e risco. O esquema de duas partições garante que uma atualização com falha não deixa o equipamento sem firmware.

## Tratamento de erros

Todas as falhas aparecem no Serial com prefixo `[ERRO]` e o FW 1.0 continua medindo; nova tentativa após a próxima sessão.

| Situação | Mensagem | Como reproduzir (`MODO_TESTE_ERRO` no firmware_v1.ino) |
|---|---|---|
| Sem Wi-Fi | `[WIFI][ERRO] Sem conexao Wi-Fi...` | `1` (usa SSID inexistente) |
| Manifesto inacessível | `[OTA][ERRO] Manifesto inacessivel (HTTP 404...)` | `2` |
| Versão já é a mais recente | `[OTA] A versao instalada ja e a mais recente` | natural no FW 2.0 |
| .bin não pode ser baixado | `[OTA][ERRO] Firmware nao pode ser baixado (HTTP 404...)` | `3` |
| Erro na gravação/atualização | `[OTA][ERRO] Falha na gravacao: Wrong Magic Byte` | `4` (tenta gravar o JSON como firmware) |

Também tratados: manifesto com JSON inválido, download incompleto (timeout) e MD5 divergente.

## Como executar

1. Abrir o projeto Wokwi (link acima) e clicar em **Play**.
2. Aguardar 3 sessões (~1 min 45 s) com LED azul.
3. O ESP32 consulta o `version.json`, detecta a 2.0, baixa o `.bin` (progresso em %) e reinicia.
4. Após o reboot: banner **FW 2.0**, partição `app1`, "atualizado de 1.0 para 2.0" e LED verde/vermelho conforme a histerese.

> Para rodar de novo a partir do FW 1.0, basta parar e dar Play outra vez no Wokwi: a simulação reinicia com a flash limpa.

### Alternativa: fila de compilação do Wokwi cheia

Se o Wokwi mostrar "Build Servers Busy", use o FW 1.0 já compilado deste repositório (`firmware_v1_wokwi.bin`, imagem completa: bootloader + tabela de partições com `app0`/`app1` + aplicação, gerada do mesmo `firmware_v1.ino`):

1. No `diagram.json`, na placa: `"attrs": { "firmwareOffset": "0" }`.
2. No editor, **F1 → Upload Firmware and Start Simulation...** e escolha `firmware_v1_wokwi.bin`.
3. O boot deve mostrar `Particao em uso : app0`. Foi assim que a demonstração do relatório foi registrada.

### Como o firmware_v2.bin foi gerado

Arduino ESP32 core 3.2.1, placa *ESP32 Dev Module*, Flash Mode **DIO**, partição padrão (`default.csv`), biblioteca ArduinoJson 7.4.2:

```
arduino-cli compile -b esp32:esp32:esp32:FlashMode=dio --export-binaries firmware_v2
```

O arquivo publicado é o binário da aplicação (`firmware_v2.ino.bin`), não o `merged.bin`.

## Bibliotecas

- **WiFi / HTTPClient / WiFiClientSecure** (core ESP32): conexão e requisições HTTPS. `setInsecure()` é uma simplificação de laboratório; em produção o certificado do servidor deve ser validado.
- **Update** (core ESP32): grava o binário na partição OTA livre e troca a partição de boot.
- **Preferences** (core ESP32): NVS para registrar a atualização.
- **ArduinoJson**: leitura do `version.json`.
