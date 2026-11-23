// Detetor de Drones — firmware v3: o ESP32-S3 faz tudo sozinho.
//
//  * Escuta Remote ID por Wi-Fi 2,4 GHz (beacon + NAN, a saltar pelos canais 1–13) e por
//    Bluetooth 4 e 5 Long Range.
//  * Reconhece drones "de Wi-Fi" (Tello, Spark, ANAFI…) pelo nome da rede ou pelo fabricante:
//    alerta de "provável drone", sem identificação.
//  * Guarda tudo na flash (LittleFS) e fala com a app Android por Bluetooth (Nordic UART
//    Service, o mesmo protocolo JSON do antigo hub do Pi).
//  * Nunca transmite para drones. O único rádio que emite é o Bluetooth para o telemóvel.
//
// Também escreve tudo pela USB (115200), para depuração.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <deque>
#include <map>
#include <string>

#include "rf_lowcost.h"
#include "rid_parser.h"

#define FW_VERSION "3.1"
#define BLE_NAME "DetetorDrones"
#define NUS_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define RX_UUID "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define TX_UUID "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

#define DWELL_MS 150                 // tempo em cada canal Wi-Fi
#define NEW_AFTER_S 600              // um drone volta a ser "novo" ao fim de 10 min sem sinal
#define TRACK_INTERVAL_MS 2000       // no máximo um ponto de trajeto por drone a cada 2 s
#define DRONE_TIMEOUT_MS 120000
#define LOG_ROTATE_BYTES 500000      // /ev.log passa a /ev.old a partir daqui (2 ficheiros)

static const uint8_t CHANNELS[] = {6, 1, 6, 2, 6, 3, 6, 4, 6, 5, 6, 7,
                                   6, 8, 6, 9, 6, 10, 6, 11, 6, 12, 6, 13};

// =====================================================================================
// Rádio → fila
// =====================================================================================
struct RawRid {
  RidSource src;
  uint8_t mac[6];
  int8_t rssi;
  uint8_t ch;
  uint16_t len;
  uint8_t data[250];
};
struct WifiHint {
  uint8_t mac[6];
  int8_t rssi;
  uint8_t ch;
  char ssid[33];
  char maker[12];
};
static QueueHandle_t ridQueue, hintQueue;
static volatile uint32_t statFrames = 0, statRid = 0, statDropped = 0;

static void enqueueRid(RidSource src, const uint8_t *mac, int rssi, uint8_t ch, const uint8_t *p,
                       size_t len) {
  RawRid r;
  r.src = src;
  memcpy(r.mac, mac, 6);
  r.rssi = rssi;
  r.ch = ch;
  r.len = len > sizeof(r.data) ? sizeof(r.data) : len;
  memcpy(r.data, p, r.len);
  if (xQueueSend(ridQueue, &r, 0) != pdTRUE) statDropped++;
  statRid++;
}

// ---- drones de Wi-Fi: nomes de rede e fabricantes conhecidos --------------------------
static const char *DRONE_SSIDS[] = {"TELLO-", "Spark-", "Mavic", "DJI-", "Mini SE", "ANAFI",
                                    "Bebop", "Disco-", "WIFI-UFO", "WiFi-720P", "FPV_WIFI",
                                    "FPV-WIFI", "HD-720P", "HD-FPV", "Drone-", "DRONE",
                                    "JJRC", "SYMA", "Hubsan", "Holy Stone", "HolyStone",
                                    "Potensic", "EACHINE", "4DRC", "SJRC", "Snaptain"};
struct Oui {
  uint8_t b[3];
  const char *maker;
};
static const Oui DRONE_OUIS[] = {
    {{0x60, 0x60, 0x1F}, "DJI"},    {{0x34, 0xD2, 0x62}, "DJI"},    {{0x48, 0x1C, 0xB9}, "DJI"},
    {{0x90, 0x03, 0xB7}, "Parrot"}, {{0xA0, 0x14, 0x3D}, "Parrot"}, {{0x00, 0x12, 0x1C}, "Parrot"},
    {{0x00, 0x26, 0x7E}, "Parrot"}, {{0x90, 0x3A, 0xE6}, "Parrot"},
};

static bool ssidLooksLikeDrone(const char *ssid) {
  for (const char *p : DRONE_SSIDS)
    if (strncasecmp(ssid, p, strlen(p)) == 0) return true;
  return false;
}

static void checkWifiDrone(const uint8_t *f, size_t len, int rssi, uint8_t ch) {
  if (len < 38 || f[0] != 0x80) return;  // só beacons
  const uint8_t *src = f + 10;
  const char *maker = nullptr;
  for (const Oui &o : DRONE_OUIS)
    if (memcmp(src, o.b, 3) == 0) maker = o.maker;
  char ssid[33] = {0};
  if (f[36] == 0 && f[37] <= 32 && 38 + (size_t)f[37] <= len) memcpy(ssid, f + 38, f[37]);
  if (!maker && !ssidLooksLikeDrone(ssid)) return;
  WifiHint h;
  memcpy(h.mac, src, 6);
  h.rssi = rssi;
  h.ch = ch;
  memcpy(h.ssid, ssid, sizeof(h.ssid));
  strncpy(h.maker, maker ? maker : "", sizeof(h.maker) - 1);
  h.maker[sizeof(h.maker) - 1] = 0;
  xQueueSend(hintQueue, &h, 0);
}

static void onWifiFrame(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  auto *pkt = (wifi_promiscuous_pkt_t *)buf;
  statFrames++;
  uint8_t mac[6];
  const uint8_t *p;
  size_t pl;
  size_t len = pkt->rx_ctrl.sig_len;
  RidSource s = rid_find_in_wifi(pkt->payload, len, mac, &p, &pl);
  if (s != RID_SRC_NONE) {
    enqueueRid(s, mac, pkt->rx_ctrl.rssi, pkt->rx_ctrl.channel, p, pl);
    return;
  }
  checkWifiDrone(pkt->payload, len, pkt->rx_ctrl.rssi, pkt->rx_ctrl.channel);
}

class ScanCb : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    const std::vector<uint8_t> &adv = dev->getPayload();
    const uint8_t *p;
    size_t pl;
    if (!rid_find_in_ble_adv(adv.data(), adv.size(), &p, &pl)) return;
    uint8_t mac[6];
    memcpy(mac, dev->getAddress().getVal(), 6);
    enqueueRid(RID_SRC_BLE, mac, dev->getRSSI(), 0, p, pl);
  }
  void onScanEnd(const NimBLEScanResults &, int) override {
    NimBLEDevice::getScan()->start(0, false, true);
  }
};
static ScanCb scanCb;

// =====================================================================================
// Hora: acertada pela app; antes disso é uma estimativa marcada como "por confirmar"
// =====================================================================================
static Preferences prefs;
static uint32_t bootNo = 0;
static int64_t bootEpoch = 0;   // hora Unix correspondente a millis() = 0 nesta sessão
static bool timeSynced = false;
static uint32_t nextId = 1, alertCount = 0;

static int64_t nowEpoch() { return bootEpoch + millis() / 1000; }

static void keyFor(char *k, char c, uint32_t b) { snprintf(k, 16, "%c%lu", c, (unsigned long)b); }

// hora de arranque e se é exata, para uma sessão (boot) qualquer
static bool bootInfo(uint32_t b, int64_t &start) {
  if (b == bootNo) {
    start = bootEpoch;
    return timeSynced;
  }
  char ke[16], kx[16];
  keyFor(ke, 'e', b);
  keyFor(kx, 'x', b);
  start = prefs.getLong64(ke, 0);
  return prefs.getBool(kx, false);
}

static void syncTime(int64_t epoch) {
  bootEpoch = epoch - millis() / 1000;
  timeSynced = true;
  char ke[16], kx[16];
  keyFor(ke, 'e', bootNo);
  keyFor(kx, 'x', bootNo);
  prefs.putLong64(ke, bootEpoch);
  prefs.putBool(kx, true);
  prefs.putLong64("last", epoch);
}

// =====================================================================================
// Registo na flash: uma linha JSON por evento
//   {"id":N,"b":boot,"r":seg_desde_arranque,"k":"alert|track","s":"s3","d":"id_drone","x":{...}}
// =====================================================================================
static void storeEvent(uint32_t id, const char *kind, const char *droneId, JsonObjectConst data) {
  File f = LittleFS.open("/ev.log", "a");
  if (!f) return;
  JsonDocument e;
  e["id"] = id;
  e["b"] = bootNo;
  e["r"] = millis() / 1000;
  e["k"] = kind;
  e["s"] = "s3";
  if (droneId) e["d"] = droneId;
  e["x"] = data;
  serializeJson(e, f);
  f.print('\n');
  size_t size = f.size();
  f.close();
  if (size > LOG_ROTATE_BYTES) {
    LittleFS.remove("/ev.old");
    LittleFS.rename("/ev.log", "/ev.old");
    File n = LittleFS.open("/ev.log", "a");
    if (n) n.close();
  }
}

// converte uma linha guardada na mensagem que a app espera (formato do hub do Pi)
static void storedToMsg(JsonDocument &in, const char *t, JsonDocument &out) {
  int64_t start;
  bool exact = bootInfo(in["b"].as<uint32_t>(), start);
  out["t"] = t;
  out["id"] = in["id"];
  out["ts"] = (double)(start + in["r"].as<int64_t>());
  out["time_ok"] = exact;
  out["kind"] = in["k"];
  out["source"] = in["s"];
  out["drone_id"] = in["d"];
  out["data"] = in["x"];
}

// =====================================================================================
// Bluetooth para a app (Nordic UART Service)
// =====================================================================================
static NimBLEServer *server;
static NimBLECharacteristic *txChar;
static volatile bool subscribed = false;
static volatile uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
static QueueHandle_t cmdQueue;
struct CmdChunk {
  uint16_t len;
  char d[244];
};

static void startAdvertising() {
  NimBLEExtAdvertising *adv = NimBLEDevice::getAdvertising();
  NimBLEExtAdvertisement a;
  a.setLegacyAdvertising(true);
  a.setConnectable(true);
  a.setScannable(true);
  a.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  a.addServiceUUID(NUS_UUID);
  NimBLEExtAdvertisement sr;
  sr.setLegacyAdvertising(true);
  sr.setName(BLE_NAME);
  adv->setInstanceData(0, a);
  adv->setScanResponseData(0, sr);
  adv->start(0);
}

class SrvCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &ci) override { connHandle = ci.getConnHandle(); }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
    subscribed = false;
    connHandle = BLE_HS_CONN_HANDLE_NONE;
    startAdvertising();
  }
};
class TxCb : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &, uint16_t sub) override {
    subscribed = sub & 0x01;
  }
};
class RxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &) override {
    NimBLEAttValue v = c->getValue();
    CmdChunk ch;
    ch.len = v.length() > sizeof(ch.d) ? sizeof(ch.d) : v.length();
    memcpy(ch.d, v.data(), ch.len);
    xQueueSend(cmdQueue, &ch, 0);
  }
};

// fila de saída: as mensagens vão em pedaços, sem bloquear o loop
static std::deque<std::string> outq;
static std::string curMsg;
static size_t curOff = 0;
static uint32_t lastChunk = 0;

static void sendToApp(JsonDocument &doc) {
  std::string s;
  serializeJson(doc, s);
  Serial.println(s.c_str());
  if (!subscribed) return;      // o histórico fica guardado; a app pede-o ao ligar
  if (outq.size() > 60) return; // telemóvel lento: não encher a memória
  s += '\n';
  outq.push_back(std::move(s));
}

static void pumpBle() {
  if (!subscribed || connHandle == BLE_HS_CONN_HANDLE_NONE) {
    outq.clear();
    curMsg.clear();
    curOff = 0;
    return;
  }
  if (millis() - lastChunk < 8) return;
  if (curOff >= curMsg.size()) {
    if (outq.empty()) return;
    curMsg = std::move(outq.front());
    outq.pop_front();
    curOff = 0;
  }
  uint16_t mtu = server->getPeerMTU(connHandle);
  size_t chunk = mtu > 3 ? mtu - 3 : 20;
  if (chunk > 180) chunk = 180;
  size_t n = std::min(chunk, curMsg.size() - curOff);
  if (txChar->notify((const uint8_t *)curMsg.data() + curOff, n, connHandle)) {
    curOff += n;
    lastChunk = millis();
  }
}

// ---- histórico: lido da flash aos poucos, à medida que a fila de saída esvazia ------
struct HistJob {
  bool active = false;
  uint32_t since = 0, limit = 0, sent = 0, lastId = 0;
  int file = 0;     // 0 = /ev.old, 1 = /ev.log
  size_t pos = 0;
};
static HistJob hist;

static void pumpHistory() {
  if (!hist.active || outq.size() > 6) return;
  static const char *files[] = {"/ev.old", "/ev.log"};
  for (int budget = 0; budget < 20 && hist.active; budget++) {
    if (hist.sent >= hist.limit || hist.file > 1) {
      JsonDocument end;
      end["t"] = "hist_end";
      end["last_id"] = hist.lastId;
      end["more"] = hist.sent >= hist.limit;
      sendToApp(end);
      hist.active = false;
      return;
    }
    File f = LittleFS.open(files[hist.file], "r");
    if (!f || hist.pos >= f.size()) {
      if (f) f.close();
      hist.file++;
      hist.pos = 0;
      continue;
    }
    f.seek(hist.pos);
    String line = f.readStringUntil('\n');
    hist.pos = f.position();
    f.close();
    // filtro rápido pelo id antes de interpretar o JSON todo
    int idx = line.indexOf("\"id\":");
    if (idx < 0) continue;
    uint32_t id = strtoul(line.c_str() + idx + 5, nullptr, 10);
    if (id <= hist.since) continue;
    JsonDocument in, out;
    if (deserializeJson(in, line)) continue;
    storedToMsg(in, "hist", out);
    sendToApp(out);
    hist.sent++;
    hist.lastId = id;
    if (outq.size() > 6) return;
  }
}

// =====================================================================================
// Drones
// =====================================================================================
static const char *EU_CLASS[] = {nullptr, "C0", "C1", "C2", "C3", "C4", "C5", "C6"};
static const char *UA_TYPE[] = {"desconhecido", "avião", "multirotor", "giroplano", "VTOL",
                                "ornitóptero", "planador", "papagaio", "balão livre",
                                "balão cativo", "dirigível", "paraquedas", "foguete", "cabo",
                                "solo", "outro"};

struct Drone {
  bool used;
  uint8_t mac[6];
  RidSource src;
  int8_t rssi;
  uint8_t ch;
  uint32_t lastSeen, lastEmit;
  ODID_UAS_Data uas;
};
static Drone drones[12];

struct Seen {
  uint32_t lastSeenS;
  uint32_t lastTrackMs;
};
static std::map<std::string, Seen> seen;  // id do drone → última vez visto

static Drone *findDrone(const uint8_t *mac) {
  Drone *freeSlot = nullptr, *oldest = &drones[0];
  for (auto &d : drones) {
    if (d.used && memcmp(d.mac, mac, 6) == 0) return &d;
    if (!d.used && !freeSlot) freeSlot = &d;
    if (d.lastSeen < oldest->lastSeen) oldest = &d;
  }
  Drone *d = freeSlot ? freeSlot : oldest;
  memset(d, 0, sizeof(*d));
  d->used = true;
  memcpy(d->mac, mac, 6);
  odid_initUasData(&d->uas);
  return d;
}

static void macStr(const uint8_t *m, char *out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void droneData(Drone &d, JsonObject x) {
  ODID_UAS_Data &u = d.uas;
  char mac[18];
  macStr(d.mac, mac);
  const char *serial = nullptr;
  for (int i = 0; i < ODID_BASIC_ID_MAX_MESSAGES; i++)
    if (u.BasicIDValid[i] && u.BasicID[i].UASID[0]) {
      if (!serial || u.BasicID[i].IDType == ODID_IDTYPE_SERIAL_NUMBER) serial = u.BasicID[i].UASID;
      int t = u.BasicID[i].UAType;
      x["ua_type"] = t;
      if (t >= 0 && t < 16) x["ua_type_txt"] = UA_TYPE[t];
    }
  x["id"] = serial ? serial : mac;
  x["mac"] = mac;
  x["src"] = rid_source_name(d.src);
  x["pattern"] = "remote_id";
  if (d.ch) x["ch"] = d.ch;
  x["rssi"] = d.rssi;
  if (u.LocationValid) {
    x["status"] = (int)u.Location.Status;
    if (u.Location.Latitude != 0 || u.Location.Longitude != 0) {
      x["lat"] = serialized(String(u.Location.Latitude, 7));
      x["lon"] = serialized(String(u.Location.Longitude, 7));
    }
    if (u.Location.Height > -1000) x["h"] = u.Location.Height;
    if (u.Location.AltitudeGeo > -1000) x["alt"] = u.Location.AltitudeGeo;
    if (u.Location.SpeedHorizontal < 255) x["spd"] = u.Location.SpeedHorizontal;
    if (u.Location.Direction <= 360) x["dir"] = u.Location.Direction;
  }
  if (u.SystemValid) {
    if (u.System.OperatorLatitude != 0 || u.System.OperatorLongitude != 0) {
      x["op_lat"] = serialized(String(u.System.OperatorLatitude, 7));
      x["op_lon"] = serialized(String(u.System.OperatorLongitude, 7));
    }
    if (u.System.ClassificationType == ODID_CLASSIFICATION_TYPE_EU) {
      int c = (int)u.System.ClassEU;
      x["eu_class"] = c;
      if (c > 0 && c < 8) x["eu_class_txt"] = EU_CLASS[c];
      x["eu_cat"] = (int)u.System.CategoryEU;
    }
  }
  if (u.OperatorIDValid && u.OperatorID.OperatorId[0]) x["op_id"] = u.OperatorID.OperatorId;
  if (u.SelfIDValid && u.SelfID.Desc[0]) x["desc"] = u.SelfID.Desc;
}

// regista e envia: alerta se é novo, ponto de trajeto se não
static void report(const std::string &did, JsonObjectConst data, bool trackable) {
  uint32_t nowS = millis() / 1000;
  auto it = seen.find(did);
  bool isNew = it == seen.end() || nowS - it->second.lastSeenS > NEW_AFTER_S;
  Seen &s = seen[did];
  s.lastSeenS = nowS;
  const char *kind;
  if (isNew) {
    kind = "alert";
    alertCount++;
    prefs.putULong("alerts", alertCount);
  } else {
    if (!trackable || millis() - s.lastTrackMs < TRACK_INTERVAL_MS) return;
    kind = "track";
  }
  s.lastTrackMs = millis();
  uint32_t id = nextId++;
  if (nextId % 20 == 0) prefs.putULong("next", nextId);  // poupar a flash: grava de 20 em 20
  storeEvent(id, kind, did.c_str(), data);

  JsonDocument msg;
  msg["t"] = kind;
  msg["id"] = id;
  msg["ts"] = (double)nowEpoch();
  msg["time_ok"] = timeSynced;
  msg["kind"] = kind;
  msg["source"] = "s3";
  msg["drone_id"] = did.c_str();
  msg["data"] = data;
  sendToApp(msg);
}

static void sendStatus() {
  JsonDocument st;
  st["t"] = "status";
  st["time_ok"] = timeSynced;
  st["rtc"] = false;
  st["now"] = (double)nowEpoch();
  st["last_id"] = nextId - 1;
  st["alerts"] = alertCount;
  JsonObject s3 = st["devs"]["s3"].to<JsonObject>();
  s3["ok"] = true;
  s3["fw"] = FW_VERSION;
  s3["frames"] = statFrames;
  s3["rid"] = statRid;
  s3["up"] = millis() / 1000;
  s3["nrf24"] = rfLowcostStatus().nrfPresent;
  s3["rx5808"] = rfLowcostStatus().rx58Present;
  JsonArray home = st["home"].to<JsonArray>();
  home.add(nullptr);
  home.add(nullptr);
  sendToApp(st);
}

// ---- comandos da app ---------------------------------------------------------------
static std::string cmdBuf;

static void handleCommand(const std::string &line) {
  JsonDocument c;
  if (deserializeJson(c, line)) return;
  const char *cmd = c["cmd"] | "";
  if (!strcmp(cmd, "time")) {
    syncTime((int64_t)c["epoch"].as<double>());
    sendStatus();
  } else if (!strcmp(cmd, "status")) {
    sendStatus();
  } else if (!strcmp(cmd, "hist")) {
    hist = HistJob();
    hist.active = true;
    hist.since = c["since"] | 0;
    hist.limit = c["limit"] | 300;
    if (hist.limit > 1000) hist.limit = 1000;
    hist.lastId = hist.since;
  }
}

// =====================================================================================
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // sem PC ligado (na caixa), escrever na USB não pode bloquear
  ridQueue = xQueueCreate(24, sizeof(RawRid));
  hintQueue = xQueueCreate(16, sizeof(WifiHint));
  cmdQueue = xQueueCreate(16, sizeof(CmdChunk));

  prefs.begin("detetor", false);
  bootNo = prefs.getULong("boot", 0) + 1;
  prefs.putULong("boot", bootNo);
  nextId = prefs.getULong("next", 1) + 20;  // salta os que podem não ter sido gravados
  alertCount = prefs.getULong("alerts", 0);
  // hora estimada (como o fake-hwclock): a última que se conheceu, marcada como incerta
  bootEpoch = prefs.getLong64("last", 0);
  char ke[16];
  keyFor(ke, 'e', bootNo);
  prefs.putLong64(ke, bootEpoch);

  if (!LittleFS.begin(true)) Serial.println("{\"t\":\"erro\",\"msg\":\"LittleFS\"}");
  // cria os ficheiros vazios: evita os avisos "does not exist" ao ler o histórico
  for (const char *fn : {"/ev.old", "/ev.log"}) {
    File f = LittleFS.open(fn, "a");
    if (f) f.close();
  }

  // Wi-Fi só em modo de escuta: nunca se liga a nenhuma rede.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&onWifiFrame);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(CHANNELS[0], WIFI_SECOND_CHAN_NONE);

  NimBLEDevice::init(BLE_NAME);
  NimBLEDevice::setMTU(247);
  server = NimBLEDevice::createServer();
  server->setCallbacks(new SrvCb());
  NimBLEService *svc = server->createService(NUS_UUID);
  txChar = svc->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
  txChar->setCallbacks(new TxCb());
  NimBLECharacteristic *rx =
      svc->createCharacteristic(RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCb());
  svc->start();
  startAdvertising();

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCb, true);
  scan->setActiveScan(false);  // passivo: não envia scan requests
  scan->setDuplicateFilter(false);
  scan->setMaxResults(0);
  // Um só rádio para Wi-Fi e Bluetooth: 40% do tempo para o Bluetooth.
  scan->setInterval(100);
  scan->setWindow(40);
#if CONFIG_BT_NIMBLE_EXT_ADV
  scan->setPhy(NimBLEScan::Phy::SCAN_ALL);  // 1M + Coded (Long Range)
#endif
  scan->start(0, false, true);

  rfLowcostBegin();  // nRF24 / RX5808, se estiverem ligados (só medem, por agora)

  JsonDocument hello;
  hello["t"] = "hello";
  hello["dev"] = "s3";
  hello["fw"] = FW_VERSION;
  hello["boot"] = bootNo;
  hello["next_id"] = nextId;
  serializeJson(hello, Serial);
  Serial.println();
}

void loop() {
  static uint32_t lastHop = 0;
  static size_t chIdx = 0;
  uint32_t now = millis();

  if (now - lastHop >= DWELL_MS) {
    lastHop = now;
    chIdx = (chIdx + 1) % sizeof(CHANNELS);
    esp_wifi_set_channel(CHANNELS[chIdx], WIFI_SECOND_CHAN_NONE);
  }

  // comandos da app
  CmdChunk ch;
  while (xQueueReceive(cmdQueue, &ch, 0) == pdTRUE) {
    cmdBuf.append(ch.d, ch.len);
    size_t nl;
    while ((nl = cmdBuf.find('\n')) != std::string::npos) {
      std::string line = cmdBuf.substr(0, nl);
      cmdBuf.erase(0, nl + 1);
      if (!line.empty()) handleCommand(line);
    }
    if (cmdBuf.size() > 2048) cmdBuf.clear();
  }

  // Remote ID
  RawRid r;
  while (xQueueReceive(ridQueue, &r, 0) == pdTRUE) {
    Drone *d = findDrone(r.mac);
    if (rid_apply_payload(&d->uas, r.data, r.len)) {
      d->src = r.src;
      d->rssi = r.rssi;
      d->ch = r.ch;
      d->lastSeen = now;
    }
  }
  for (auto &d : drones) {
    if (!d.used) continue;
    if (now - d.lastSeen > DRONE_TIMEOUT_MS) {
      d.used = false;
      continue;
    }
    if (d.lastSeen > d.lastEmit && now - d.lastEmit >= 1000) {
      d.lastEmit = now;
      JsonDocument data;
      droneData(d, data.to<JsonObject>());
      report(data["id"].as<std::string>(), data.as<JsonObjectConst>(), true);
    }
  }

  // drones de Wi-Fi (sem identificação)
  WifiHint h;
  while (xQueueReceive(hintQueue, &h, 0) == pdTRUE) {
    char mac[18];
    macStr(h.mac, mac);
    JsonDocument data;
    data["pattern"] = "wifi_drone";
    data["mac"] = mac;
    if (h.ssid[0]) data["ssid"] = h.ssid;
    if (h.maker[0]) data["maker"] = h.maker;
    data["rssi"] = h.rssi;
    data["ch"] = h.ch;
    data["src"] = "wifi_beacon";
    report(std::string("wifi:") + mac, data.as<JsonObjectConst>(), false);
  }

  pumpHistory();
  pumpBle();

  // limpeza e estado
  static uint32_t lastHb = 0, lastSave = 0;
  if (now - lastHb >= 30000) {
    lastHb = now;
    sendStatus();
    uint32_t nowS = now / 1000;
    for (auto it = seen.begin(); it != seen.end();)
      it = (nowS - it->second.lastSeenS > 3600) ? seen.erase(it) : std::next(it);
  }
  if (now - lastSave >= 300000) {  // guarda a hora a cada 5 min (estimativa após falha de luz)
    lastSave = now;
    if (bootEpoch) prefs.putLong64("last", nowEpoch());
  }
  delay(2);
}
