// Detetor de Drones — recetor passivo de Remote ID (firmware v2, sem Wi-Fi de casa).
//
//   ESP32-S3: Wi-Fi 2,4 GHz (salta pelos canais 1–13, com prioridade ao 6) + Bluetooth 4/5 LR
//   ESP32-C5: Wi-Fi 5 GHz (salta pelos canais de 5 GHz, com prioridade ao 149)
//
// Não se liga a nenhuma rede e nunca transmite para drones. Tudo o que descodifica sai
// em JSON, uma linha por mensagem, pela USB para o Raspberry Pi.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "rid_parser.h"

#if CONFIG_IDF_TARGET_ESP32S3
#define DEV_NAME "s3"
#define HAS_BLE 1
#include <NimBLEDevice.h>
// Canal 6 a cada dois saltos: é onde vive o NAN e a maioria dos beacons de Remote ID.
static const uint8_t CHANNELS[] = {6, 1, 6, 2, 6, 3, 6, 4, 6, 5, 6, 7,
                                   6, 8, 6, 9, 6, 10, 6, 11, 6, 12, 6, 13};
#elif CONFIG_IDF_TARGET_ESP32C5
#define DEV_NAME "c5"
#define HAS_BLE 0
// 5 GHz na UE: UNII-1/2/2e e a banda SRD dos 5,8 GHz. O 149 aparece a cada dois saltos.
static const uint8_t CHANNELS[] = {149, 36, 149, 40, 149, 44, 149, 48, 149, 52, 149, 56,
                                   149, 60, 149, 64, 149, 100, 149, 104, 149, 108, 149, 112,
                                   149, 116, 149, 120, 149, 124, 149, 128, 149, 132, 149, 136,
                                   149, 140, 149, 153, 149, 157, 149, 161, 149, 165};
#else
#error "Placa não suportada: usar ESP32-S3 ou ESP32-C5"
#endif

#define FW_VERSION "2.0"
#define DWELL_MS 150            // tempo em cada canal
#define EMIT_INTERVAL_MS 1000   // no máximo 1 linha por drone por segundo
#define DRONE_TIMEOUT_MS 120000

// ---------- fila entre os callbacks de rádio e o loop ----------
struct RawRid {
  RidSource src;
  uint8_t mac[6];
  int8_t rssi;
  uint8_t ch;
  uint16_t len;
  uint8_t data[250];
};
static QueueHandle_t ridQueue;
static volatile uint32_t statFrames = 0, statRid = 0, statDropped = 0;
static volatile uint8_t curChannel = 0;

static void enqueue(RidSource src, const uint8_t *mac, int rssi, uint8_t ch, const uint8_t *p,
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

static void onWifiFrame(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  auto *pkt = (wifi_promiscuous_pkt_t *)buf;
  statFrames++;
  uint8_t mac[6];
  const uint8_t *p;
  size_t pl;
  RidSource s = rid_find_in_wifi(pkt->payload, pkt->rx_ctrl.sig_len, mac, &p, &pl);
  if (s != RID_SRC_NONE) enqueue(s, mac, pkt->rx_ctrl.rssi, pkt->rx_ctrl.channel, p, pl);
}

#if HAS_BLE
class ScanCb : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    const std::vector<uint8_t> &adv = dev->getPayload();
    const uint8_t *p;
    size_t pl;
    if (!rid_find_in_ble_adv(adv.data(), adv.size(), &p, &pl)) return;
    uint8_t mac[6];
    memcpy(mac, dev->getAddress().getVal(), 6);
    enqueue(RID_SRC_BLE, mac, dev->getRSSI(), 0, p, pl);
  }
  void onScanEnd(const NimBLEScanResults &, int) override {
    NimBLEDevice::getScan()->start(0, false, true);
  }
};
static ScanCb scanCb;
#endif

// ---------- tabela de drones ----------
struct Drone {
  bool used;
  uint8_t mac[6];
  RidSource src;
  int8_t rssi;
  uint8_t ch;
  uint32_t firstSeen, lastSeen, lastEmit;
  ODID_UAS_Data uas;
};
static Drone drones[12];

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
  d->firstSeen = millis();
  return d;
}

static void emit(JsonDocument &doc) {
  serializeJson(doc, Serial);
  Serial.write('\n');
}

static void emitDrone(Drone &d) {
  ODID_UAS_Data &u = d.uas;
  char mac[18];
  sprintf(mac, "%02X:%02X:%02X:%02X:%02X:%02X", d.mac[0], d.mac[1], d.mac[2], d.mac[3],
          d.mac[4], d.mac[5]);

  JsonDocument doc;
  doc["t"] = "drone";
  doc["dev"] = DEV_NAME;
  const char *serial = nullptr;
  for (int i = 0; i < ODID_BASIC_ID_MAX_MESSAGES; i++)
    if (u.BasicIDValid[i] && u.BasicID[i].UASID[0]) {
      if (!serial || u.BasicID[i].IDType == ODID_IDTYPE_SERIAL_NUMBER) serial = u.BasicID[i].UASID;
      doc["ua_type"] = (int)u.BasicID[i].UAType;
    }
  doc["id"] = serial ? serial : mac;
  doc["mac"] = mac;
  doc["src"] = rid_source_name(d.src);
  if (d.ch) doc["ch"] = d.ch;
  doc["rssi"] = d.rssi;

  if (u.LocationValid) {
    doc["status"] = (int)u.Location.Status;
    if (u.Location.Latitude != 0 || u.Location.Longitude != 0) {
      doc["lat"] = serialized(String(u.Location.Latitude, 7));
      doc["lon"] = serialized(String(u.Location.Longitude, 7));
    }
    if (u.Location.Height > -1000) doc["h"] = u.Location.Height;
    if (u.Location.AltitudeGeo > -1000) doc["alt"] = u.Location.AltitudeGeo;
    if (u.Location.SpeedHorizontal < 255) doc["spd"] = u.Location.SpeedHorizontal;
    if (u.Location.Direction <= 360) doc["dir"] = u.Location.Direction;
  }
  if (u.SystemValid) {
    if (u.System.OperatorLatitude != 0 || u.System.OperatorLongitude != 0) {
      doc["op_lat"] = serialized(String(u.System.OperatorLatitude, 7));
      doc["op_lon"] = serialized(String(u.System.OperatorLongitude, 7));
    }
    if (u.System.ClassificationType == ODID_CLASSIFICATION_TYPE_EU) {
      doc["eu_class"] = (int)u.System.ClassEU;     // 0 indefinida, 1 => C0, 2 => C1, ...
      doc["eu_cat"] = (int)u.System.CategoryEU;    // 1 aberta, 2 específica, 3 certificada
    }
    if (u.System.Timestamp) doc["sys_ts"] = u.System.Timestamp;  // s desde 2019-01-01 UTC
  }
  if (u.OperatorIDValid && u.OperatorID.OperatorId[0]) doc["op_id"] = u.OperatorID.OperatorId;
  if (u.SelfIDValid && u.SelfID.Desc[0]) doc["desc"] = u.SelfID.Desc;
  emit(doc);
  d.lastEmit = millis();
}

void setup() {
  Serial.begin(115200);
  ridQueue = xQueueCreate(24, sizeof(RawRid));

  // Wi-Fi só em modo de escuta: nunca se liga a nenhuma rede.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
#if CONFIG_IDF_TARGET_ESP32C5
  esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);
#endif
  wifi_promiscuous_filter_t filt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&onWifiFrame);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(CHANNELS[0], WIFI_SECOND_CHAN_NONE);

#if HAS_BLE
  NimBLEDevice::init("");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCb, true);
  scan->setActiveScan(false);  // passivo: não envia scan requests
  scan->setDuplicateFilter(false);
  scan->setMaxResults(0);
  // O S3 só tem um rádio de 2,4 GHz para Wi-Fi e Bluetooth: com janela de 99% o Wi-Fi quase
  // não tinha tempo (1 trama em 30 s no primeiro teste). 40% do tempo para o Bluetooth.
  scan->setInterval(100);
  scan->setWindow(40);
#if defined(MYNEWT_VAL_BLE_EXT_ADV) && MYNEWT_VAL_BLE_EXT_ADV
  scan->setPhy(NimBLEScan::Phy::SCAN_ALL);  // 1M + Coded (Long Range)
#endif
  scan->start(0, false, true);
#endif

  JsonDocument hello;
  hello["t"] = "hello";
  hello["dev"] = DEV_NAME;
  hello["fw"] = FW_VERSION;
  emit(hello);
}

void loop() {
  static uint32_t lastHop = 0;
  static size_t chIdx = 0;
  uint32_t now = millis();

  if (now - lastHop >= DWELL_MS) {
    lastHop = now;
    chIdx = (chIdx + 1) % sizeof(CHANNELS);
    curChannel = CHANNELS[chIdx];
    esp_wifi_set_channel(curChannel, WIFI_SECOND_CHAN_NONE);
  }

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
    if (d.lastSeen > d.lastEmit && now - d.lastEmit >= EMIT_INTERVAL_MS) emitDrone(d);
  }

  static uint32_t lastHb = 0;
  if (now - lastHb >= 10000) {
    lastHb = now;
    JsonDocument hb;
    hb["t"] = "hb";
    hb["dev"] = DEV_NAME;
    hb["up"] = now / 1000;
    hb["frames"] = statFrames;
    hb["rid"] = statRid;
    hb["drop"] = statDropped;
    emit(hb);
  }
  delay(2);
}
