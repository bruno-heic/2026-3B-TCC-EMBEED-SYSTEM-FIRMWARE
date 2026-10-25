#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <TinyGPSPlus.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ====== CONFIGURE AQUI ======
const char *SUPABASE_URL = "https://rnteixjmrascgaztgpcf.supabase.co";
const char *SUPABASE_KEY = "sb_publishable_a3v-QJFyyaiRCSPftbXTtw_bo1wHb3l";   // anon/publishable (nunca a service_role)
// ============================

// UUIDs do pareamento BLE (o app usa os mesmos)
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CONFIG_UUID  "beb5483e-36e1-4688-b7f5-ea07361b26a8"  // app -> coleira: "SERIAL\nREDE\nSENHA"
#define STATUS_UUID  "beb5483e-36e1-4688-b7f5-ea07361b26a9"  // coleira -> app: andamento
#define ID_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26aa"  // coleira -> app: id_produto (somente leitura)

const int RES_OK        = 0;
const int RES_ERRO_REDE = 1;
const int RES_RECUSADA  = 2;

Preferences prefs;

// GPS NEO-6M: TX do GPS -> GPIO 4; RX do GPS -> GPIO 3
HardwareSerial GPS(1);
TinyGPSPlus gps;
const int GPS_RX_PIN = 4;
const int GPS_TX_PIN = 3;
const unsigned long INTERVALO_GPS_MS = 30000;
unsigned long ultimaLocalizacaoEnviada = 0;

// Monitoramento do GPS
bool gpsMonitorAtivo = true;         // comando "gps" liga/desliga
bool gpsRaw = false;                 // comando "gpsraw" mostra o NMEA bruto
const unsigned long INTERVALO_MONITOR_GPS_MS = 3000;
unsigned long ultimoMonitorGPS = 0;
unsigned long ultimoByteGPS = 0;     // millis() do ultimo byte recebido do modulo
uint32_t bytesGPS = 0;

// Alerta de saida da area segura
const int LEITURAS_FORA_PARA_ALERTA = 2;                  // leituras seguidas fora da area
const unsigned long INTERVALO_REENVIO_SAIDA_MS = 300000;  // 5 min entre alertas
int leiturasForaSeguidas = 0;
unsigned long ultimoAlertaSaida = 0;                      // 0 = nenhum enviado nesta saida

// ---------- Sensor de temperatura DS18B20 ----------
// DATA no GPIO 1 (pull-up interno ou resistor de 4,7k entre DATA e 3V3)
const int TEMP_PIN = 1;
const unsigned long INTERVALO_LEITURA_TEMP_MS = 5000;    // le o sensor a cada 5 s
const unsigned long INTERVALO_ENVIO_TEMP_MS   = 60000;   // envia ao Supabase a cada 1 min
const unsigned long CONVERSAO_TEMP_MS = 800;             // tempo de conversao do sensor (12 bits)
OneWire oneWireTemp(TEMP_PIN);
DallasTemperature sensorTemp(&oneWireTemp);
unsigned long ultimaLeituraTemp = 0;
unsigned long ultimoEnvioTemp = 0;
unsigned long inicioConversaoTemp = 0;
bool convertendoTemp = false;
float ultimaTemperatura = NAN;

// Monitoramento da temperatura
bool tempMonitorAtivo = true;        // comando "tempmon" liga/desliga
float tempMin = NAN, tempMax = NAN;
uint32_t leiturasTempOk = 0;
uint32_t leiturasTempFalha = 0;
uint16_t leiturasTempIguais = 0;     // leituras seguidas sem mudar

struct ConfigColeira {
  bool configurada = false;
  int idProduto = 0;
  String nomeProduto;
  String tipoProduto;
  String numSerie;
  String chave;
  String wifiSsid;
  String wifiSenha;
};
ConfigColeira cfg;

// ---------- Sensor de som ----------
// Confirme o GPIO conforme o modulo e a placa ESP32-C3 utilizados.
const int MIC_PIN = 0;
const unsigned long JANELA_SOM_MS = 20;
const int JANELAS_CONSECUTIVAS = 3;
const float FATOR_RUIDO = 2.0f;
const unsigned long DURACAO_SOM_MS = 500;
const unsigned long SILENCIO_REARME_MS = 1000;
const unsigned long INTERVALO_REENVIO_ALERTA_MS = 30000;

int centroSom = 2048;
int limiteSom = 700;
int contagemSom = 0;
unsigned long inicioEpisodioSom = 0;
unsigned long inicioSilencioSom = 0;
unsigned long ultimaTentativaAlerta = 0;
bool episodioSomAtivo = false;
bool alertaSomEnviado = false;
bool alertaSomPendente = false;
bool sensorSomCalibrado = false;

bool reconhecida = false;       // o banco já aceitou esta coleira?
bool recusada = false;          // o banco recusou (serial/chave errados)?
unsigned long ultimaTentativa = 0;

// Pareamento BLE
bool bleAtivo = false;
unsigned long bleEncerrarEm = 0;    // 0 = não agendado
BLECharacteristic *pStatus = nullptr;
volatile bool novaConfigWifi = false;
volatile bool dispositivoConectado = false;
String bleSerial, bleSsid, bleSenha;

// ---------- Leitura pelo Serial Monitor ----------
String lerLinha(const String &pergunta, const String &padrao = "", bool ocultar = false) {
  Serial.print(pergunta);
  if (padrao.length()) Serial.print(" [" + padrao + "]");
  Serial.print(": ");

  while (!Serial.available()) delay(10);
  String s = Serial.readStringUntil('\n');
  s.trim();
  if (s.length() == 0) s = padrao;   // Enter vazio = usa o valor sugerido
  Serial.println(ocultar ? String("********") : s);
  return s;
}

String lerObrigatorio(const String &pergunta, const String &padrao = "") {
  while (true) {
    String s = lerLinha(pergunta, padrao);
    if (s.length()) return s;
    Serial.println("Campo obrigatorio.");
  }
}

int lerInteiroPositivo(const String &pergunta) {
  while (true) {
    String s = lerLinha(pergunta);
    long v = s.toInt();
    if (v > 0) return (int)v;
    Serial.println("Valor invalido. Digite um numero inteiro maior que zero.");
  }
}

// Serial: 12 caracteres, letras maiúsculas e números
bool serialValido(const String &s) {
  if (s.length() != 12) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
  }
  return true;
}

String lerSerial() {
  while (true) {
    String s = lerLinha("Numero de serie (12 caracteres, o mesmo do banco)");
    s.toUpperCase();
    if (serialValido(s)) return s;
    Serial.println("Formato invalido. Use 12 letras/numeros, ex.: PTF2BKGZMRC1");
  }
}

// Chave: 32 caracteres hexadecimais (0-9 e a-f)
bool chaveValida(const String &s) {
  if (s.length() != 32) return false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

String lerChave() {
  while (true) {
    String s = lerLinha("Chave secreta (32 caracteres, gerada pelo banco)", "", true);
    s.toLowerCase();
    if (chaveValida(s)) return s;
    Serial.println("Chave invalida. Sao 32 caracteres (0-9 e a-f).");
  }
}

// ---------- Memória permanente ----------
void carregarConfig() {
  prefs.begin("coleira", false);
  cfg.configurada = prefs.getBool("ok", false);
  cfg.idProduto   = prefs.getInt("idProduto", 0);
  cfg.nomeProduto = prefs.getString("nome", "");
  cfg.tipoProduto = prefs.getString("tipo", "");
  cfg.numSerie    = prefs.getString("serie", "");
  cfg.chave       = prefs.getString("chave", "");
  cfg.wifiSsid    = prefs.getString("ssid", "");
  cfg.wifiSenha   = prefs.getString("wpass", "");
  prefs.end();
}

// Salva ID, serial, chave, nome e tipo (identidade de fábrica)
void salvarIdentidade() {
  prefs.begin("coleira", false);
  prefs.putBool("ok", true);
  prefs.putInt("idProduto", cfg.idProduto);
  prefs.putString("nome", cfg.nomeProduto);
  prefs.putString("tipo", cfg.tipoProduto);
  prefs.putString("serie", cfg.numSerie);
  prefs.putString("chave", cfg.chave);
  prefs.end();
}

// Salva só a rede Wi-Fi (vem do app, via BLE)
void salvarWifi() {
  prefs.begin("coleira", false);
  prefs.putString("ssid", cfg.wifiSsid);
  prefs.putString("wpass", cfg.wifiSenha);
  prefs.end();
}

// Apaga só a rede Wi-Fi, mantendo a identidade
void apagarWifi() {
  prefs.begin("coleira", false);
  prefs.remove("ssid");
  prefs.remove("wpass");
  prefs.end();
  cfg.wifiSsid = "";
  cfg.wifiSenha = "";
}

void mostrarConfig() {
  Serial.println("  ID do produto : " + String(cfg.idProduto));
  Serial.println("  Nome          : " + cfg.nomeProduto);
  Serial.println("  Tipo          : " + cfg.tipoProduto);
  Serial.println("  Numero serie  : " + cfg.numSerie);
  Serial.println("  Chave secreta : " + cfg.chave.substring(0, 4) + "****************************");
  Serial.println("  Wi-Fi         : " +
                 (cfg.wifiSsid.length() ? cfg.wifiSsid + " (senha oculta)"
                                        : String("(nenhuma - configure pelo app)")));
}

// ---------- Assistente de setup (fábrica: só identidade, sem Wi-Fi) ----------
void executarSetup() {
  while (true) {
    Serial.println("\n--- SETUP DA COLEIRA (fabrica) ---");
    cfg.idProduto   = lerInteiroPositivo("ID do produto (o id_produto devolvido pelo banco)");
    cfg.numSerie    = lerSerial();
    cfg.chave       = lerChave();
    cfg.nomeProduto = lerObrigatorio("Nome do produto", "Coleira Inteligente");
    cfg.tipoProduto = lerObrigatorio("Tipo do produto", "coleira");

    Serial.println("\nConfira os dados:");
    mostrarConfig();

    String ok = lerLinha("Salvar? (s/n)", "s");
    ok.toLowerCase();
    if (ok == "s") {
      cfg.configurada = true;
      salvarIdentidade();
      Serial.println("Identidade salva na memoria da coleira.");
      return;
    }
    Serial.println("Ok, vamos refazer.");
  }
}

// ---------- Wi-Fi ----------
String motivoWifi() {
  switch (WiFi.status()) {
    case WL_IDLE_STATUS:     return "tentando conectar";
    case WL_NO_SSID_AVAIL:   return "rede nao encontrada (nome errado ou rede de 5 GHz)";
    case WL_CONNECT_FAILED:  return "senha incorreta ou falha de autenticacao";
    case WL_CONNECTION_LOST: return "conexao perdida";
    case WL_DISCONNECTED:    return "desconectado";
    default:                 return "status " + String((int)WiFi.status());
  }
}

String nomeMotivo(uint8_t r) {
  switch (r) {
    case 2:   return "autenticacao expirou";
    case 8:   return "desconectado por comando";
    case 15:  return "tempo esgotado no handshake (senha incorreta ou sinal fraco)";
    case 200: return "perdeu o sinal do roteador";
    case 201: return "rede nao encontrada";
    case 202: return "falha de autenticacao (senha incorreta ou seguranca incompativel)";
    case 203: return "roteador recusou a associacao";
    case 204: return "tempo esgotado no handshake (senha incorreta ou sinal fraco)";
    case 205: return "falha ao conectar";
    default:  return "motivo desconhecido";
  }
}

void aoEventoWifi(arduino_event_id_t evento, arduino_event_info_t info) {
  static unsigned long ultimoAviso = 0;

  if (evento == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.println("\n[wifi] CONECTADO a '" + WiFi.SSID() + "' | IP " +
                   WiFi.localIP().toString() + " | sinal " + String(WiFi.RSSI()) + " dBm");
    return;
  }

  if (evento != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;

  uint8_t r = info.wifi_sta_disconnected.reason;
  if (r == 8) return;   // desconexão pedida pelo próprio código

  if (millis() - ultimoAviso > 3000) {
    ultimoAviso = millis();
    Serial.println("\n[wifi] " + nomeMotivo(r) + " (codigo " + String(r) + ")");
  }
}

int listarRedes() {
  Serial.println("Procurando redes...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  delay(200);

  int n = WiFi.scanNetworks();
  if (n <= 0) {
    Serial.println("Nenhuma rede encontrada.");
    return 0;
  }
  for (int i = 0; i < n; i++) {
    Serial.printf("  %2d) %-32s canal %2d  sinal %d dBm  %s\n",
                  i + 1, WiFi.SSID(i).c_str(), (int)WiFi.channel(i), (int)WiFi.RSSI(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "aberta" : "protegida");
  }
  return n;
}

// Só para teste pelo Serial Monitor (o fluxo normal usa o app/BLE)
void escolherWifi(String &ssid, String &senha) {
  int n = listarRedes();
  String esc = lerLinha("Numero da rede na lista (Enter para digitar o nome)");
  int idx = esc.toInt();

  if (n > 0 && idx >= 1 && idx <= n) {
    ssid = WiFi.SSID(idx - 1);
    Serial.println("Rede escolhida: " + ssid);
  } else {
    ssid = lerObrigatorio("Nome da rede Wi-Fi");
  }
  WiFi.scanDelete();

  senha = lerLinha("Senha do Wi-Fi (Enter se for rede aberta)", "", true);
}

bool conectarWifi(unsigned long timeoutMs = 20000) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // sem economia de energia: conexão mais estável
  WiFi.setAutoReconnect(true);
  WiFi.disconnect();
  delay(300);
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiSenha.c_str());

  Serial.print("Conectando ao Wi-Fi '" + cfg.wifiSsid + "'");
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < timeoutMs) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi conectado. IP: " + WiFi.localIP().toString());
    return true;
  }
  Serial.println("Nao foi possivel conectar ao Wi-Fi: " + motivoWifi());
  return false;
}

void mostrarStatus() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi   : CONECTADO");
    Serial.println("  Rede  : " + WiFi.SSID());
    Serial.println("  IP    : " + WiFi.localIP().toString());
    Serial.println("  Sinal : " + String(WiFi.RSSI()) + " dBm");
  } else {
    Serial.println("Wi-Fi   : NAO CONECTADO (" + motivoWifi() + ")");
  }
  Serial.println("Banco   : " + String(reconhecida ? "coleira reconhecida"
                                       : recusada ? "recusada"
                                                  : "ainda nao reconhecida"));
  Serial.println("Pareamento BLE: " + String(bleAtivo ? "ATIVO" : "desligado"));
  if (isnan(ultimaTemperatura)) {
    Serial.println("Temperatura: sem leitura valida ainda");
  } else {
    Serial.printf("Temperatura: %.2f C (min %.2f, max %.2f | ok %lu, falhas %lu)\n",
                  ultimaTemperatura, tempMin, tempMax,
                  (unsigned long)leiturasTempOk, (unsigned long)leiturasTempFalha);
  }
}

// ---------- Banco ----------
int extrairIdProduto(const String &json) {
  int i = json.indexOf("\"id_produto\"");
  if (i < 0) return -1;
  i = json.indexOf(':', i);
  if (i < 0) return -1;
  return json.substring(i + 1).toInt();
}

int reconhecerNoBanco(int &codigo, String &resposta) {
  resposta = "";

  if (WiFi.status() != WL_CONNECTED) {
    codigo = -100;
    Serial.println("Sem Wi-Fi no momento da consulta.");
    return RES_ERRO_REDE;
  }

  String host = String(SUPABASE_URL);
  host.replace("https://", "");
  IPAddress ip;
  if (!WiFi.hostByName(host.c_str(), ip)) {
    codigo = -101;
    Serial.println("DNS falhou: nao encontrei '" + host + "'. O Wi-Fi esta sem internet?");
    return RES_ERRO_REDE;
  }

  WiFiClientSecure client;
  client.setInsecure();   // simplifica no TCC; em produção valide o certificado

  HTTPClient http;
  http.setTimeout(8000);
  http.begin(client, String(SUPABASE_URL) + "/rest/v1/rpc/reconhecer_coleira");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  if (String(SUPABASE_KEY).startsWith("eyJ")) {
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  }

  String json = "{\"p_num_serie\":\"" + cfg.numSerie + "\",\"p_chave\":\"" + cfg.chave + "\"}";

  codigo = http.POST(json);
  resposta = http.getString();
  http.end();

  if (codigo < 0) {
    Serial.println("Supabase -> " + String(codigo) + " | sinal " + String(WiFi.RSSI()) + " dBm");
  } else {
    Serial.println("Supabase -> " + String(codigo));
  }

  if (codigo >= 200 && codigo < 300) return RES_OK;
  if (codigo == 400) return RES_RECUSADA;
  return RES_ERRO_REDE;
}

void tentarReconhecer() {
  Serial.println("Consultando o banco...");
  int codigo = 0;
  String resposta;
  int r = reconhecerNoBanco(codigo, resposta);

  if (r == RES_OK) {
    reconhecida = true;
    recusada = false;
    Serial.println("COLEIRA RECONHECIDA pelo banco.");
    Serial.println("Resposta: " + resposta);

    int idBanco = extrairIdProduto(resposta);
    if (idBanco != cfg.idProduto) {
      Serial.println("ATENCAO: o ID salvo na coleira (" + String(cfg.idProduto) +
                     ") difere do banco (" + String(idBanco) + "). Refaca o 'setup'.");
    }
  } else if (r == RES_RECUSADA) {
    recusada = true;
    Serial.println("RECUSADA: serial ou chave nao conferem com o banco. Refaca o 'setup'.");
    Serial.println("Resposta: " + resposta);
  } else {
    Serial.println("Sem resposta valida (codigo " + String(codigo) + "). Nova tentativa em 30 s.");
    if (codigo == 401 || codigo == 403) Serial.println("Dica: confira a URL e a chave anon.");
    if (codigo == 404) Serial.println("Dica: a funcao reconhecer_coleira nao existe. Rode o SQL.");
  }
}

// ---------- GPS e envio seguro ao Supabase ----------

// Le o valor de uma chave dentro de um objeto JSON simples: "chave": valor
bool valorJson(const String &obj, const char *chave, String &saida) {
  String k = "\"" + String(chave) + "\"";
  int i = obj.indexOf(k);
  if (i < 0) return false;
  i = obj.indexOf(':', i);
  if (i < 0) return false;
  i++;
  while (i < (int)obj.length() && obj[i] == ' ') i++;
  int f = i;
  while (f < (int)obj.length() && obj[f] != ',' && obj[f] != '}') f++;
  saida = obj.substring(i, f);
  saida.trim();
  return true;
}

// Procura, no array "areas" da resposta, uma area com dentro_area=false
bool encontrarAreaFora(const String &resp, int &idArea, String &dist) {
  int ini = resp.indexOf("\"areas\"");
  if (ini < 0) return false;
  int pos = resp.indexOf('[', ini);
  if (pos < 0) return false;
  int fimArr = resp.indexOf(']', pos);
  if (fimArr < 0) return false;

  int p = resp.indexOf('{', pos);
  while (p >= 0 && p < fimArr) {
    int q = resp.indexOf('}', p);
    if (q < 0) break;
    String obj = resp.substring(p, q + 1);

    String dentro, id;
    if (valorJson(obj, "dentro_area", dentro) && dentro == "false" &&
        valorJson(obj, "id_area", id)) {
      idArea = id.toInt();
      if (!valorJson(obj, "distancia_metros", dist)) dist = "0";
      return true;
    }
    p = resp.indexOf('{', q);
  }
  return false;
}

bool enviarAlertaSaidaArea(int idArea, const String &dist) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[AREA] Sem Wi-Fi; alerta de saida nao enviado.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Prototipo/TCC: validar certificado em producao.

  HTTPClient http;
  http.setTimeout(12000);
  String endpoint = String(SUPABASE_URL) + "/rest/v1/rpc/registrar_alerta_saida_area";
  if (!http.begin(client, endpoint)) {
    Serial.println("[AREA] Falha ao iniciar HTTP.");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  if (String(SUPABASE_KEY).startsWith("eyJ")) {
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  }

  String corpo = "{\"p_num_serie\":\"" + cfg.numSerie +
                 "\",\"p_chave\":\"" + cfg.chave +
                 "\",\"p_id_area\":" + String(idArea) +
                 ",\"p_distancia_metros\":" + dist + "}";

  Serial.println("[AREA] Enviando alerta de saida da area " + String(idArea) +
                 " (distancia " + dist + " m)...");
  int codigo = http.POST(corpo);
  String resposta = http.getString();
  http.end();

  Serial.println("[AREA] Supabase HTTP: " + String(codigo));
  Serial.println("[AREA] Resposta: " + resposta);

  if (codigo == 404) Serial.println("[AREA] A RPC registrar_alerta_saida_area ainda nao existe no banco.");
  if (codigo == 401 || codigo == 403) Serial.println("[AREA] Confira as permissoes EXECUTE da RPC.");

  return codigo >= 200 && codigo < 300;
}

// Chamada com a resposta de registrar_localizacao_coleira
void tratarSaidaArea(const String &resposta) {
  int idArea = 0;
  String dist;

  if (!encontrarAreaFora(resposta, idArea, dist)) {
    if (leiturasForaSeguidas > 0) Serial.println("[AREA] Coleira voltou para dentro da area segura.");
    leiturasForaSeguidas = 0;
    ultimoAlertaSaida = 0;   // na proxima saida, alerta de novo
    return;
  }

  leiturasForaSeguidas++;
  Serial.printf("[AREA] FORA da area %d (%s m) | leitura fora %d/%d\n",
                idArea, dist.c_str(), leiturasForaSeguidas, LEITURAS_FORA_PARA_ALERTA);

  if (leiturasForaSeguidas < LEITURAS_FORA_PARA_ALERTA) return;

  if (ultimoAlertaSaida != 0 && millis() - ultimoAlertaSaida < INTERVALO_REENVIO_SAIDA_MS) {
    Serial.println("[AREA] Alerta ja enviado recentemente; aguardando para reenviar.");
    return;
  }

  if (enviarAlertaSaidaArea(idArea, dist)) {
    ultimoAlertaSaida = millis();
    Serial.println("[AREA] Alerta de saida enviado.");
  }
}

bool enviarLocalizacao(double latitude, double longitude) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[GPS] Sem Wi-Fi; localizacao nao enviada.");
    return false;
  }

  if (!reconhecida || recusada) {
    Serial.println("[GPS] Coleira ainda nao reconhecida; localizacao nao enviada.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Para o prototipo/TCC; em producao valide o certificado TLS.

  HTTPClient http;
  http.setTimeout(12000);
  String endpoint = String(SUPABASE_URL) + "/rest/v1/rpc/registrar_localizacao_coleira";
  if (!http.begin(client, endpoint)) {
    Serial.println("[GPS] Nao foi possivel iniciar requisicao HTTP.");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  // A chave publishable sb_publishable_... nao e um JWT; nao a use como Bearer.
  if (String(SUPABASE_KEY).startsWith("eyJ")) {
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  }

  String corpo = "{\"p_num_serie\":\"" + cfg.numSerie +
                 "\",\"p_chave\":\"" + cfg.chave +
                 "\",\"p_latitude\":" + String(latitude, 7) +
                 ",\"p_longitude\":" + String(longitude, 7) + "}";

  Serial.println("[GPS] Enviando latitude=" + String(latitude, 7) +
                 " longitude=" + String(longitude, 7));
  int codigo = http.POST(corpo);
  String resposta = http.getString();
  http.end();

  Serial.println("[GPS] Supabase HTTP: " + String(codigo));
  if (codigo >= 200 && codigo < 300) {
    Serial.println("[GPS] Localizacao registrada. Resposta: " + resposta);
    tratarSaidaArea(resposta);
    return true;
  }

  Serial.println("[GPS] Falha ao registrar localizacao: " + resposta);
  if (codigo == 404) Serial.println("[GPS] Confira se a RPC registrar_localizacao_coleira foi criada.");
  if (codigo == 401 || codigo == 403) Serial.println("[GPS] Confira a chave e as permissoes EXECUTE da RPC.");
  return false;
}

// Unico ponto que le a UART do GPS (conta bytes, mostra NMEA bruto e alimenta o TinyGPS)
void alimentarGPS() {
  while (GPS.available() > 0) {
    char c = GPS.read();
    bytesGPS++;
    ultimoByteGPS = millis();
    if (gpsRaw) Serial.write(c);
    gps.encode(c);
  }
}

void monitorarGPS() {
  if (!gpsMonitorAtivo) return;
  if (millis() - ultimoMonitorGPS < INTERVALO_MONITOR_GPS_MS) return;
  ultimoMonitorGPS = millis();

  bool semDados = (bytesGPS == 0) || (millis() - ultimoByteGPS > 3000);
  uint32_t chars = gps.charsProcessed();
  uint32_t comFix = gps.sentencesWithFix();
  uint32_t falhas = gps.failedChecksum();

  Serial.println("------ [GPS] monitor ------");
  Serial.printf("[GPS] bytes recebidos: %lu | frases com fix: %lu | checksum falhou: %lu\n",
                (unsigned long)bytesGPS, (unsigned long)comFix, (unsigned long)falhas);

  if (semDados) {
    Serial.println("[GPS] STATUS: SEM DADOS do modulo.");
    Serial.println("[GPS] Confira: TX do GPS -> GPIO " + String(GPS_RX_PIN) +
                   ", RX do GPS -> GPIO " + String(GPS_TX_PIN) +
                   ", alimentacao (VCC/GND) e baud 9600.");
    return;
  }

  if (chars > 100 && falhas > comFix + 5 && comFix == 0) {
    Serial.println("[GPS] STATUS: dados chegando, mas corrompidos (baud errado ou fio solto).");
  }

  int sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
  Serial.printf("[GPS] satelites: %d | HDOP: %s\n", sats,
                gps.hdop.isValid() ? String(gps.hdop.hdop(), 1).c_str() : "?");

  if (gps.location.isValid()) {
    unsigned long idade = gps.location.age();
    Serial.println("[GPS] STATUS: FIX OK" + String(idade > 5000 ? " (mas coordenada ANTIGA)" : ""));
    Serial.println("[GPS] lat=" + String(gps.location.lat(), 7) +
                   " lng=" + String(gps.location.lng(), 7) +
                   " | idade=" + String(idade) + " ms");
    if (gps.altitude.isValid()) Serial.println("[GPS] altitude=" + String(gps.altitude.meters(), 1) + " m");
    if (gps.speed.isValid())    Serial.println("[GPS] velocidade=" + String(gps.speed.kmph(), 1) + " km/h");
    if (gps.date.isValid() && gps.time.isValid()) {
      Serial.printf("[GPS] hora UTC: %02d/%02d/%04d %02d:%02d:%02d\n",
                    gps.date.day(), gps.date.month(), gps.date.year(),
                    gps.time.hour(), gps.time.minute(), gps.time.second());
    }

    // Diz se a localizacao esta de fato sendo enviada ao banco
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[GPS] ENVIO: bloqueado, sem Wi-Fi.");
    } else if (!reconhecida || recusada) {
      Serial.println("[GPS] ENVIO: bloqueado, coleira nao reconhecida pelo banco.");
    } else {
      long falta = (long)INTERVALO_GPS_MS - (long)(millis() - ultimaLocalizacaoEnviada);
      if (falta < 0) falta = 0;
      Serial.println("[GPS] ENVIO: ok, proximo envio em ~" + String(falta / 1000) + " s.");
    }
  } else {
    Serial.println("[GPS] STATUS: modulo ligado, mas SEM FIX (ainda procurando satelites).");
    Serial.println("[GPS] Va para um local aberto, longe de telhado/janela. O primeiro fix (cold start) pode levar varios minutos.");
  }
}

void processarGPS() {
  alimentarGPS();

  if (!gps.location.isValid()) return;
  if (gps.location.age() > 5000) return; // ignora coordenada antiga
  if (millis() - ultimaLocalizacaoEnviada < INTERVALO_GPS_MS) return;

  // Marca a tentativa para evitar disparar requisicoes repetidas em caso de erro.
  ultimaLocalizacaoEnviada = millis();
  enviarLocalizacao(gps.location.lat(), gps.location.lng());
}

// ---------- Temperatura: leitura, monitoramento e envio ----------

bool enviarTemperatura(float temperatura) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TEMP] Sem Wi-Fi; leitura nao enviada.");
    return false;
  }

  if (!reconhecida || recusada) {
    Serial.println("[TEMP] Coleira nao reconhecida; leitura nao enviada.");
    return false;
  }

  if (isnan(temperatura) ||
      temperatura == DEVICE_DISCONNECTED_C ||
      temperatura < -40.0f ||
      temperatura > 80.0f) {
    Serial.println("[TEMP] Leitura invalida; envio cancelado.");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Prototipo/TCC; validar certificado em producao.

  HTTPClient http;
  http.setTimeout(10000);

  String endpoint =
      String(SUPABASE_URL) +
      "/rest/v1/rpc/registrar_temperatura_coleira";

  if (!http.begin(client, endpoint)) {
    Serial.println("[TEMP] Falha ao iniciar HTTP.");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);

  if (String(SUPABASE_KEY).startsWith("eyJ")) {
    http.addHeader(
        "Authorization",
        String("Bearer ") + SUPABASE_KEY
    );
  }

  String corpo =
      "{\"p_num_serie\":\"" + cfg.numSerie +
      "\",\"p_chave\":\"" + cfg.chave +
      "\",\"p_temperatura\":" + String(temperatura, 2) + "}";

  Serial.printf(
      "[TEMP] Enviando temperatura: %.2f C\n",
      temperatura
  );

  int codigo = http.POST(corpo);
  String resposta = http.getString();
  http.end();

  Serial.printf("[TEMP] Supabase HTTP: %d\n", codigo);
  Serial.println("[TEMP] Resposta: " + resposta);

  if (codigo < 200 || codigo >= 300) {
    Serial.println("[TEMP] Falha ao registrar a leitura.");

    if (codigo == 404) {
      Serial.println(
          "[TEMP] RPC registrar_temperatura_coleira nao encontrada."
      );
    } else if (codigo == 401 || codigo == 403) {
      Serial.println(
          "[TEMP] Verifique a chave e as permissoes da RPC."
      );
    }

    return false;
  }

  // A sua funcao SQL retorna "alerta": true/false.
  if (resposta.indexOf("\"alerta\":true") >= 0 ||
      resposta.indexOf("\"alerta\": true") >= 0) {

    Serial.println(
        "[TEMP] ALERTA DE TEMPERATURA GERADO PELO SUPABASE!"
    );

    if (resposta.indexOf("\"temperatura_alta\"") >= 0) {
      Serial.println(
          "[TEMP] Temperatura acima do limite configurado."
      );
    } else if (
        resposta.indexOf("\"temperatura_baixa\"") >= 0
    ) {
      Serial.println(
          "[TEMP] Temperatura abaixo do limite configurado."
      );
    }

    int inicioId = resposta.indexOf("\"id_alerta\"");
    if (inicioId >= 0) {
      Serial.println(
          "[TEMP] O banco retornou o ID do alerta."
      );
    }

    int inicioTipo = resposta.indexOf("\"tipo\"");
    if (inicioTipo >= 0) {
      Serial.println(
          "[TEMP] Confira o campo tipo na resposta acima."
      );
    }

  } else if (
      resposta.indexOf("\"alerta\":false") >= 0 ||
      resposta.indexOf("\"alerta\": false") >= 0
  ) {

    Serial.println(
        "[TEMP] Leitura registrada; nenhum alerta novo foi criado."
    );

    if (
        resposta.indexOf("ALERTA_DE_TEMPERATURA_DESATIVADO") >= 0
    ) {
      Serial.println(
          "[TEMP] A regra de alerta de temperatura esta desativada."
      );
    } else if (
        resposta.indexOf("ALERTA_RECENTE_JA_REGISTRADO") >= 0
    ) {
      Serial.println(
          "[TEMP] Ja existe um alerta recente deste tipo."
      );
    }

  } else if (
      resposta.indexOf("\"registrado\":false") >= 0 ||
      resposta.indexOf("\"registrado\": false") >= 0
  ) {

    Serial.println(
        "[TEMP] Banco recusou a leitura. Confira o motivo retornado."
    );

  } else {
    Serial.println(
        "[TEMP] Medicao aceita, mas a resposta nao contem o campo alerta esperado."
    );
  }

  return true;
}


// Leitura nao bloqueante a cada 5 s. O envio ao Supabase continua a cada 1 min.
void processarTemperatura() {
  if (!convertendoTemp) {
    if (ultimaLeituraTemp != 0 && millis() - ultimaLeituraTemp < INTERVALO_LEITURA_TEMP_MS) return;
    sensorTemp.requestTemperatures();
    inicioConversaoTemp = millis();
    convertendoTemp = true;
    return;
  }

  if (millis() - inicioConversaoTemp < CONVERSAO_TEMP_MS) return;

  convertendoTemp = false;
  ultimaLeituraTemp = millis();

  float c = sensorTemp.getTempCByIndex(0);

  // -127 = sensor desconectado; 85 = valor de fabrica (alimentacao instavel)
  if (c == DEVICE_DISCONNECTED_C || c == 85.0f) {
    leiturasTempFalha++;
    if (tempMonitorAtivo) {
      Serial.printf("[TEMP] LEITURA INVALIDA (%.1f) | ok %lu falhas %lu | sensores: %d | confira DATA (GPIO %d), resistor 4,7k e 3V3\n",
                    c, (unsigned long)leiturasTempOk, (unsigned long)leiturasTempFalha,
                    sensorTemp.getDeviceCount(), TEMP_PIN);
    }
    return;
  }

  // Estatisticas
  float delta = isnan(ultimaTemperatura) ? 0.0f : c - ultimaTemperatura;
  if (isnan(tempMin) || c < tempMin) tempMin = c;
  if (isnan(tempMax) || c > tempMax) tempMax = c;
  if (!isnan(ultimaTemperatura) && fabs(delta) < 0.01f) leiturasTempIguais++;
  else leiturasTempIguais = 0;
  leiturasTempOk++;
  ultimaTemperatura = c;

  if (tempMonitorAtivo) {
    const char *tendencia = "estavel";
    if (delta > 0.05f) tendencia = "SUBINDO";
    else if (delta < -0.05f) tendencia = "DESCENDO";

    Serial.printf("[TEMP] %.2f C | variacao %+.2f | min %.2f max %.2f | ok %lu falhas %lu | %s\n",
                  c, delta, tempMin, tempMax,
                  (unsigned long)leiturasTempOk, (unsigned long)leiturasTempFalha, tendencia);

    // 12 leituras seguidas iguais = ~1 minuto sem mudar
    if (leiturasTempIguais >= 12) {
      Serial.println("[TEMP] Sem variacao ha ~1 min. E normal em ambiente estavel; "
                     "segure o sensor na mao para ver subir. Se nunca muda, o sensor pode estar travado.");
    }
  }

  // Envio ao banco a cada 1 minuto, com a leitura mais recente
  if (ultimoEnvioTemp == 0 || millis() - ultimoEnvioTemp >= INTERVALO_ENVIO_TEMP_MS) {
    ultimoEnvioTemp = millis();
    enviarTemperatura(c);
  }
}

// ---------- Pareamento BLE com o app ----------
void enviarStatus(const char *txt) {
  if (pStatus == nullptr) return;

  // Atualiza o valor para que o app também possa lê-lo por READ.
  pStatus->setValue((uint8_t *)txt, strlen(txt));

  // notify() só deve ser chamado depois que o serviço foi iniciado
  // e quando existe um cliente conectado.
  if (bleAtivo && dispositivoConectado) {
    pStatus->notify();
  }
}

class ConfigCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String dados = String(c->getValue().c_str());   // SERIAL\nREDE\nSENHA
    int p1 = dados.indexOf('\n');
    if (p1 < 1) return;
    int p2 = dados.indexOf('\n', p1 + 1);
    if (p2 < 0) return;
    bleSerial = dados.substring(0, p1);
    bleSsid   = dados.substring(p1 + 1, p2);
    bleSenha  = dados.substring(p2 + 1);
    novaConfigWifi = true;   // quem conecta é o loop(), não o callback
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    dispositivoConectado = true;
    Serial.println("[ble] cliente conectado");
  }

  void onDisconnect(BLEServer *s) override {
    dispositivoConectado = false;
    Serial.println("[ble] cliente desconectado");
    if (bleAtivo) BLEDevice::startAdvertising();   // continua visível para o app
  }
};

void iniciarBLE() {
  if (bleAtivo) {
    Serial.println("[BLE-TESTE] BLE ja esta ativo.");
    return;
  }

  Serial.println("[BLE-TESTE 1] Preparando teste BLE...");
  Serial.flush();

  // Isola o teste: desliga o radio Wi-Fi antes de inicializar o Bluetooth.
  Serial.println("[BLE-TESTE 2] Desligando Wi-Fi...");
  Serial.flush();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(300);
  Serial.println("[BLE-TESTE 3] Wi-Fi desligado.");
  Serial.flush();

  uint64_t mac = ESP.getEfuseMac();
  char sufixo[5];
  snprintf(sufixo, sizeof(sufixo), "%02X%02X", (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  String nome = "Coleira_" + String(sufixo);

  Serial.println("[BLE-TESTE 4] Chamando BLEDevice::init...");
  Serial.flush();
  BLEDevice::init(nome.c_str());
  Serial.println("[BLE-TESTE 5] BLEDevice::init concluido.");
  Serial.flush();

  Serial.println("[BLE-TESTE 6] Configurando MTU...");
  Serial.flush();
  BLEDevice::setMTU(185);
  Serial.println("[BLE-TESTE 7] Criando servidor BLE...");
  Serial.flush();
  BLEServer *server = BLEDevice::createServer();
  if (!server) {
    Serial.println("[BLE-ERRO] createServer retornou nullptr.");
    Serial.flush();
    return;
  }
  server->setCallbacks(new ServerCallbacks());
  Serial.println("[BLE-TESTE 8] Criando servico BLE...");
  Serial.flush();
  BLEService *service = server->createService(SERVICE_UUID);
  if (!service) {
    Serial.println("[BLE-ERRO] createService retornou nullptr.");
    Serial.flush();
    return;
  }

  Serial.println("[BLE-TESTE 9] Criando caracteristica de configuracao...");
  Serial.flush();
  BLECharacteristic *config = service->createCharacteristic(
      CONFIG_UUID, BLECharacteristic::PROPERTY_WRITE);
  if (!config) {
    Serial.println("[BLE-ERRO] Caracteristica CONFIG retornou nullptr.");
    Serial.flush();
    return;
  }
  config->setCallbacks(new ConfigCallbacks());

  Serial.println("[BLE-TESTE 10] Criando caracteristica de status...");
  Serial.flush();
  pStatus = service->createCharacteristic(
      STATUS_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  if (!pStatus) {
    Serial.println("[BLE-ERRO] Caracteristica STATUS retornou nullptr.");
    Serial.flush();
    return;
  }
  pStatus->addDescriptor(new BLE2902());
  // Define o valor inicial SEM notify(): o serviço ainda não foi iniciado.
  pStatus->setValue("PRONTA");

  BLECharacteristic *idChar = service->createCharacteristic(
      ID_UUID, BLECharacteristic::PROPERTY_READ);
  if (!idChar) {
    Serial.println("[BLE-ERRO] Caracteristica ID retornou nullptr.");
    return;
  }
  idChar->setValue(String(cfg.idProduto).c_str());

  Serial.println("[BLE-TESTE 11] Iniciando servico...");
  Serial.flush();
  service->start();
  Serial.println("[BLE-TESTE 12] Preparando advertising...");
  Serial.flush();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  if (!adv) {
    Serial.println("[BLE-ERRO] getAdvertising retornou nullptr.");
    Serial.flush();
    return;
  }
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  bleAtivo = true;
  dispositivoConectado = false;
  Serial.println("[BLE-TESTE 13] Iniciando advertising...");
  Serial.flush();
  BLEDevice::startAdvertising();


  Serial.println("[BLE-TESTE 14] BLE iniciou sem crash.");
  Serial.println("[ble] pareamento ATIVO: procure '" + nome + "' no app");
  Serial.flush();
}

void pararBLE() {
  if (!bleAtivo) return;
  bleAtivo = false;
  bleEncerrarEm = 0;
  pStatus = nullptr;
  BLEDevice::deinit(true);
  Serial.println("[ble] pareamento ENCERRADO");
}

// Processa a rede enviada pelo app (chamado no loop, nunca dentro do callback do BLE)
void tratarConfigWifi() {
  if (bleSerial != cfg.numSerie) {
    Serial.println("[ble] serial recebido nao confere com o da coleira");
    enviarStatus("ERRO_SERIAL");
    return;
  }

  Serial.println("[ble] recebi a rede '" + bleSsid + "' do app");
  enviarStatus("CONECTANDO");

  cfg.wifiSsid  = bleSsid;
  cfg.wifiSenha = bleSenha;

  if (conectarWifi()) {
    salvarWifi();
    enviarStatus("OK");

    reconhecida = false;
    recusada = false;
    tentarReconhecer();
    enviarStatus(reconhecida ? "RECONHECIDA" : recusada ? "RECUSADA" : "SEM_RESPOSTA");

    bleEncerrarEm = millis() + 6000;   // dá tempo de o app receber o resultado
  } else {
    enviarStatus("ERRO_WIFI");
    cfg.wifiSsid = "";
    cfg.wifiSenha = "";
    // continua em pareamento, esperando o app tentar de novo - sem reiniciar
  }

  ultimaTentativa = millis();
}

// ---------- Comandos digitados no Serial Monitor ----------
void mostrarAjuda() {
  Serial.println("Comandos: setup | pareamento | wifi | redes | status | mostrar | reconhecer | somcalibrar | gps | gpsraw | temp | tempmon | reiniciar");
}

void tratarComandos() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  cmd.toLowerCase();

  if (cmd == "setup") {
    executarSetup();
    Serial.println("Reiniciando para aplicar os novos dados...");
    delay(500);
    ESP.restart();
  } else if (cmd == "pareamento") {
    apagarWifi();
    Serial.println("[BLE-TESTE] Comando pareamento recebido.");
    iniciarBLE();
  } else if (cmd == "status") {
    mostrarStatus();
  } else if (cmd == "mostrar") {
    mostrarConfig();
  } else if (cmd == "reconhecer") {
    recusada = false;
    if (WiFi.status() == WL_CONNECTED) {
      ultimaTentativa = millis();
      tentarReconhecer();
    } else {
      Serial.println("Sem Wi-Fi no momento.");
    }
  } else if (cmd == "somcalibrar") {
    calibrarSensorSom();
  } else if (cmd == "redes") {
    listarRedes();
    WiFi.scanDelete();
    if (cfg.wifiSsid.length()) {
      WiFi.setAutoReconnect(true);
      WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiSenha.c_str());
    }
  } else if (cmd == "wifi") {
    escolherWifi(cfg.wifiSsid, cfg.wifiSenha);
    salvarWifi();
    Serial.println("Wi-Fi salvo. Reiniciando...");
    delay(500);
    ESP.restart();
  } else if (cmd == "gps") {
    gpsMonitorAtivo = !gpsMonitorAtivo;
    Serial.println(String("[GPS] monitor ") + (gpsMonitorAtivo ? "LIGADO" : "DESLIGADO"));
  } else if (cmd == "gpsraw") {
    gpsRaw = !gpsRaw;
    Serial.println(String("[GPS] NMEA bruto ") + (gpsRaw ? "LIGADO" : "DESLIGADO"));
  } else if (cmd == "temp") {
    // Leitura imediata para teste (nao espera o intervalo de leitura)
    Serial.printf("[TEMP] sensores encontrados: %d\n", sensorTemp.getDeviceCount());
    sensorTemp.setWaitForConversion(true);
    sensorTemp.requestTemperatures();
    sensorTemp.setWaitForConversion(false);
    float c = sensorTemp.getTempCByIndex(0);
    if (c == DEVICE_DISCONNECTED_C) {
      Serial.println("[TEMP] sensor nao encontrado. Confira DATA (GPIO " + String(TEMP_PIN) +
                     "), resistor 4,7k e 3V3.");
    } else {
      Serial.printf("[TEMP] %.2f C\n", c);
    }
  } else if (cmd == "tempmon") {
    tempMonitorAtivo = !tempMonitorAtivo;
    Serial.println(String("[TEMP] monitor ") + (tempMonitorAtivo ? "LIGADO" : "DESLIGADO"));
  } else if (cmd == "reiniciar") {
    Serial.println("Reiniciando...");
    delay(500);
    ESP.restart();
  } else if (cmd.length() > 0) {
    Serial.println("Comando desconhecido.");
    mostrarAjuda();
  }
}

// ---------- Sensor de som: calibracao, deteccao e envio ----------
int lerSomSuave() {
  long soma = 0;
  for (int i = 0; i < 8; i++) soma += analogRead(MIC_PIN);
  return soma / 8;
}

int medirNivelSom() {
  unsigned long inicio = millis();
  long soma = 0;
  int n = 0;
  while (millis() - inicio < JANELA_SOM_MS) {
    soma += abs(lerSomSuave() - centroSom);
    n++;
    // Mantem a leitura do GPS fluindo durante a medicao.
    alimentarGPS();
  }
  return n > 0 ? (int)(soma / n) : 0;
}

void calibrarSensorSom() {
  Serial.println("[SOM] Calibrando. Mantenha o ambiente silencioso...");
  analogReadResolution(12);
  analogSetPinAttenuation((gpio_num_t)MIC_PIN, ADC_11db);
  delay(4000);
  long soma = 0;
  for (int i = 0; i < 200; i++) {
    soma += lerSomSuave();
    delay(5);
  }
  centroSom = soma / 200;
  int maxSilencio = 0;
  unsigned long inicio = millis();
  while (millis() - inicio < 3000) {
    int nivel = medirNivelSom();
    if (nivel > maxSilencio) maxSilencio = nivel;
  }
  limiteSom = max(1, (int)(maxSilencio * FATOR_RUIDO));
  contagemSom = 0;
  episodioSomAtivo = false;
  alertaSomEnviado = false;
  alertaSomPendente = false;
  inicioEpisodioSom = 0;
  inicioSilencioSom = 0;
  sensorSomCalibrado = true;
  Serial.printf("[SOM] Centro: %d | ruido maximo: %d | limite: %d\n", centroSom, maxSilencio, limiteSom);
  Serial.println("[SOM] Sensor pronto.");
}

bool enviarAlertaSom() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[SOM] Sem Wi-Fi; alerta permanece pendente.");
    return false;
  }
  if (!reconhecida || recusada || cfg.numSerie.isEmpty() || cfg.chave.isEmpty()) {
    Serial.println("[SOM] Coleira nao reconhecida; envio adiado.");
    return false;
  }
  WiFiClientSecure client;
  client.setInsecure(); // Prototipo/TCC: validar certificado em producao.
  HTTPClient http;
  http.setTimeout(8000);
  String endpoint = String(SUPABASE_URL) + "/rest/v1/rpc/registrar_alerta_coleira";
  if (!http.begin(client, endpoint)) {
    Serial.println("[SOM] Falha ao iniciar HTTP.");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  if (String(SUPABASE_KEY).startsWith("eyJ")) http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  String corpo = "{\"p_num_serie\":\"" + cfg.numSerie + "\",\"p_chave\":\"" + cfg.chave + "\"}";
  Serial.println("[SOM] Enviando episodio ao Supabase...");
  int codigo = http.POST(corpo);
  String resposta = http.getString();
  http.end();
  Serial.printf("[SOM] HTTP: %d\n", codigo);
  Serial.println("[SOM] Resposta: " + resposta);
  if (codigo < 200 || codigo >= 300) return false;
  // HTTP 200 pode conter registrado=false. Isso nao deve causar repeticoes infinitas.
  if (resposta.indexOf("\"registrado\":true") >= 0 || resposta.indexOf("\"registrado\": true") >= 0) {
    Serial.println("[SOM] Alerta registrado.");
    return true;
  }
  if (resposta.indexOf("\"registrado\":false") >= 0 || resposta.indexOf("\"registrado\": false") >= 0) {
    Serial.println("[SOM] Banco respondeu sem registrar. Verifique a regra alertar_latido e o vinculo do pet.");
    return true;
  }
  return false;
}

void processarSensorSom() {
  if (!sensorSomCalibrado) return;
  int nivel = medirNivelSom();
  if (nivel > limiteSom) {
    contagemSom++;
    inicioSilencioSom = 0;
  } else {
    contagemSom = 0;
    if (episodioSomAtivo && inicioSilencioSom == 0) inicioSilencioSom = millis();
  }
  bool detectado = contagemSom >= JANELAS_CONSECUTIVAS;
  if (detectado && !episodioSomAtivo) {
    episodioSomAtivo = true;
    inicioEpisodioSom = millis();
    inicioSilencioSom = 0;
    Serial.println("[SOM] Episodio iniciado.");
  }
  if (episodioSomAtivo && !alertaSomEnviado && millis() - inicioEpisodioSom >= DURACAO_SOM_MS) {
    alertaSomPendente = true;
    alertaSomEnviado = true;
    Serial.println("[SOM] Som persistente detectado; alerta pendente.");
  }
  if (episodioSomAtivo && inicioSilencioSom != 0 && millis() - inicioSilencioSom >= SILENCIO_REARME_MS) {
    episodioSomAtivo = false;
    alertaSomEnviado = false;
    inicioEpisodioSom = 0;
    inicioSilencioSom = 0;
    contagemSom = 0;
    Serial.println("[SOM] Silencio confirmado; sensor rearmado.");
  }
  if (alertaSomPendente && (ultimaTentativaAlerta == 0 || millis() - ultimaTentativaAlerta >= INTERVALO_REENVIO_ALERTA_MS)) {
    ultimaTentativaAlerta = millis();
    if (enviarAlertaSom()) alertaSomPendente = false;
  }
  static unsigned long ultimoDiagnosticoSom = 0;
  if (millis() - ultimoDiagnosticoSom >= 10000) {
    ultimoDiagnosticoSom = millis();
    Serial.printf("[SOM] nivel=%d limite=%d consecutivas=%d pendente=%s\n", nivel, limiteSom, contagemSom, alertaSomPendente ? "sim" : "nao");
  }
}

// ---------- Programa principal ----------
void setup() {
  Serial.begin(115200);
  delay(1500);   // tempo para abrir o Serial Monitor
  GPS.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.println("[GPS] UART1 iniciada a 9600 baud (RX GPIO4, TX GPIO3).");

  pinMode(TEMP_PIN, INPUT_PULLUP);   // pull-up interno, so para teste (sem o resistor de 4,7k)
  sensorTemp.begin();
  sensorTemp.setWaitForConversion(false);   // leitura nao bloqueante
  Serial.printf("[TEMP] DS18B20 no GPIO %d | sensores encontrados: %d\n",
                TEMP_PIN, sensorTemp.getDeviceCount());

  WiFi.onEvent(aoEventoWifi);
  Serial.println("\n=== COLEIRA ===");

  carregarConfig();

  if (!cfg.configurada) {
    Serial.println("Coleira ainda nao configurada.");
    executarSetup();
    Serial.println("Identidade salva. Reiniciando para iniciar o pareamento...");
    delay(500);
    ESP.restart();   // único reinício automático do fluxo: aplica a identidade nova
  }

  Serial.println("Coleira configurada:");
  mostrarConfig();

  calibrarSensorSom();

  bool wifiOk = false;
  if (cfg.wifiSsid.length() > 0) {
    wifiOk = conectarWifi();
    if (wifiOk) {
      ultimaTentativa = millis();
      tentarReconhecer();
    }
  }

  // Teste diagnostico: se nao houver Wi-Fi, inicializa BLE etapa por etapa.
  if (!wifiOk) {
    Serial.println("[BLE-TESTE] Wi-Fi indisponivel ou nao configurado.");
    Serial.println("[BLE-TESTE] Iniciando BLE para localizar a etapa da falha...");
    iniciarBLE();
  }

  Serial.println("\nColeira pronta (firmware de diagnostico BLE).");
  mostrarAjuda();
}

void loop() {
  tratarComandos();

  if (novaConfigWifi) {
    novaConfigWifi = false;
    tratarConfigWifi();
  }

  if (bleAtivo && bleEncerrarEm && millis() > bleEncerrarEm) {
    pararBLE();
  }

  if (cfg.wifiSsid.length() > 0 && millis() - ultimaTentativa > 30000) {
    ultimaTentativa = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi desconectado (" + motivoWifi() + "), tentando de novo...");
      WiFi.disconnect();
      WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiSenha.c_str());
    } else if (!reconhecida && !recusada) {
      tentarReconhecer();
    }
  }

  processarGPS();
  monitorarGPS();
  processarTemperatura();
  processarSensorSom();
}