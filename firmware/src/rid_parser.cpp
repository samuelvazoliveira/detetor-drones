#include "rid_parser.h"

#include <string.h>

static const uint8_t ASD_STAN_OUI[3] = {0xFA, 0x0B, 0xBC};     // beacon vendor IE
static const uint8_t WFA_OUI[3] = {0x50, 0x6F, 0x9A};          // Wi-Fi Alliance (NAN)
static const uint8_t NAN_DEST[6] = {0x51, 0x6F, 0x9A, 0x01, 0x00, 0x00};
static const uint8_t ODID_SERVICE_ID[6] = {0x88, 0x69, 0x19, 0x9D, 0x92, 0x09};

const char *rid_source_name(RidSource s) {
  switch (s) {
    case RID_SRC_WIFI_BEACON: return "wifi_beacon";
    case RID_SRC_WIFI_NAN: return "wifi_nan";
    case RID_SRC_BLE: return "ble";
    default: return "none";
  }
}

static RidSource parse_beacon(const uint8_t *f, size_t len, const uint8_t **p, size_t *pl) {
  size_t pos = 24 + 12;  // cabeçalho MAC + timestamp/intervalo/capacidades
  while (pos + 2 <= len) {
    uint8_t id = f[pos], ielen = f[pos + 1];
    if (pos + 2 + ielen > len) break;
    const uint8_t *ie = f + pos + 2;
    // IE vendor: OUI(3) + tipo 0x0D + contador(1) + pack
    if (id == 0xDD && ielen > 5 && memcmp(ie, ASD_STAN_OUI, 3) == 0 && ie[3] == 0x0D) {
      *p = ie + 5;
      *pl = ielen - 5;
      return RID_SRC_WIFI_BEACON;
    }
    pos += 2 + ielen;
  }
  return RID_SRC_NONE;
}

static RidSource parse_nan(const uint8_t *f, size_t len, const uint8_t **p, size_t *pl) {
  if (memcmp(f + 4, NAN_DEST, 6) != 0) return RID_SRC_NONE;
  const uint8_t *b = f + 24;
  if (len < 24 + 6) return RID_SRC_NONE;
  if (b[0] != 0x04 || b[1] != 0x09 || memcmp(b + 2, WFA_OUI, 3) != 0 || b[5] != 0x13)
    return RID_SRC_NONE;
  size_t pos = 24 + 6;
  while (pos + 3 <= len) {
    uint8_t id = f[pos];
    uint16_t alen = f[pos + 1] | (f[pos + 2] << 8);
    if (pos + 3 + alen > len) break;
    const uint8_t *a = f + pos + 3;
    // Service Descriptor: service_id(6) instance(1) requestor(1) control(1) info_len(1) info
    if (id == 0x03 && alen >= 10 && memcmp(a, ODID_SERVICE_ID, 6) == 0 && (a[8] & 0x10)) {
      uint8_t info_len = a[9];
      if (10 + (size_t)info_len <= alen && info_len > 1) {
        *p = a + 11;            // salta o contador de mensagens
        *pl = info_len - 1;
        return RID_SRC_WIFI_NAN;
      }
    }
    pos += 3 + alen;
  }
  return RID_SRC_NONE;
}

RidSource rid_find_in_wifi(const uint8_t *f, size_t len, uint8_t mac[6], const uint8_t **p,
                           size_t *pl) {
  if (len < 24) return RID_SRC_NONE;
  uint8_t fc = f[0];
  RidSource s = RID_SRC_NONE;
  if (fc == 0x80) s = parse_beacon(f, len, p, pl);
  else if (fc == 0xD0) s = parse_nan(f, len, p, pl);
  if (s != RID_SRC_NONE) memcpy(mac, f + 10, 6);
  return s;
}

bool rid_find_in_ble_adv(const uint8_t *adv, size_t len, const uint8_t **p, size_t *pl) {
  size_t pos = 0;
  while (pos + 1 < len) {
    uint8_t adlen = adv[pos];
    if (adlen == 0 || pos + 1 + adlen > len) break;
    const uint8_t *ad = adv + pos + 1;
    // Service Data 16-bit (0x16), UUID 0xFFFA, app code 0x0D, contador, mensagem/pack
    if (ad[0] == 0x16 && adlen >= 5 && ad[1] == 0xFA && ad[2] == 0xFF && ad[3] == 0x0D) {
      *p = ad + 5;
      *pl = adlen - 5;
      return *pl >= ODID_MESSAGE_SIZE;
    }
    pos += 1 + adlen;
  }
  return false;
}

bool rid_apply_payload(ODID_UAS_Data *uas, const uint8_t *payload, size_t len) {
  if (len < ODID_MESSAGE_SIZE) return false;
  uint8_t type = payload[0] >> 4;
  if (type == ODID_MESSAGETYPE_PACKED) {
    if (len < 3) return false;
    uint8_t n = payload[2];
    if (payload[1] != ODID_MESSAGE_SIZE || n == 0 || n > ODID_PACK_MAX_MESSAGES) return false;
    if (3 + (size_t)n * ODID_MESSAGE_SIZE > len) return false;
    bool any = false;
    for (uint8_t i = 0; i < n; i++)
      if (decodeOpenDroneID(uas, payload + 3 + i * ODID_MESSAGE_SIZE) != ODID_MESSAGETYPE_INVALID)
        any = true;
    return any;
  }
  return decodeOpenDroneID(uas, payload) != ODID_MESSAGETYPE_INVALID;
}
