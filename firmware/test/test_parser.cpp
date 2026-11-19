// Teste no PC: gera tramas Remote ID com a lib oficial e confirma que o parser as lê.
// g++ -I../src -I../lib/opendroneid test_parser.cpp ../src/rid_parser.cpp \
//     ../lib/opendroneid/opendroneid.c <odid-src>/libopendroneid/wifi.c
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "rid_parser.h"
extern "C" {
#include "odid_wifi.h"
}

static void fill(ODID_UAS_Data *d) {
  odid_initUasData(d);
  d->BasicID[0].UAType = ODID_UATYPE_HELICOPTER_OR_MULTIROTOR;
  d->BasicID[0].IDType = ODID_IDTYPE_SERIAL_NUMBER;
  strcpy(d->BasicID[0].UASID, "1581F5FJD239C00DW22E");
  d->BasicIDValid[0] = 1;
  d->Location.Status = ODID_STATUS_AIRBORNE;
  d->Location.Latitude = 40.7590; d->Location.Longitude = -8.5700;
  d->Location.Height = 87.5; d->Location.HeightType = ODID_HEIGHT_REF_OVER_TAKEOFF;
  d->Location.AltitudeGeo = 120; d->Location.SpeedHorizontal = 6.25; d->Location.Direction = 90;
  d->LocationValid = 1;
  d->System.OperatorLatitude = 40.7612; d->System.OperatorLongitude = -8.5731;
  d->System.ClassificationType = ODID_CLASSIFICATION_TYPE_EU;
  d->System.CategoryEU = ODID_CATEGORY_EU_OPEN; d->System.ClassEU = ODID_CLASS_EU_CLASS_1;
  d->System.AreaCount = 1;
  d->SystemValid = 1;
  d->OperatorID.OperatorIdType = ODID_OPERATOR_ID;
  strcpy(d->OperatorID.OperatorId, "PRTabcdefgh12345");
  d->OperatorIDValid = 1;
}

static void check(const ODID_UAS_Data &r, const char *what) {
  assert(strcmp(r.BasicID[0].UASID, "1581F5FJD239C00DW22E") == 0);
  assert(r.LocationValid && fabs(r.Location.Latitude - 40.7590) < 1e-5);
  assert(fabs(r.Location.Height - 87.5) < 1);
  assert(r.SystemValid && fabs(r.System.OperatorLongitude + 8.5731) < 1e-5);
  assert(r.System.ClassEU == ODID_CLASS_EU_CLASS_1);
  assert(strcmp(r.OperatorID.OperatorId, "PRTabcdefgh12345") == 0);
  printf("OK  %s\n", what);
}

int main() {
  ODID_UAS_Data d; fill(&d);
  uint8_t buf[1024]; uint8_t mac[6]; const uint8_t *p; size_t pl;
  const char srcmac[6] = {0x60, 0x60, 0x1f, 0x11, 0x22, 0x33};

  // 1) Wi-Fi beacon
  int n = odid_wifi_build_message_pack_beacon_frame(&d, srcmac, "DJI-RID", 7, 100, 1, buf, sizeof buf);
  assert(n > 0);
  assert(rid_find_in_wifi(buf, n, mac, &p, &pl) == RID_SRC_WIFI_BEACON);
  assert(memcmp(mac, srcmac, 6) == 0);
  { ODID_UAS_Data r; odid_initUasData(&r); assert(rid_apply_payload(&r, p, pl)); check(r, "Wi-Fi beacon"); }

  // 2) Wi-Fi NAN
  n = odid_wifi_build_message_pack_nan_action_frame(&d, srcmac, 2, buf, sizeof buf);
  assert(n > 0);
  assert(rid_find_in_wifi(buf, n, mac, &p, &pl) == RID_SRC_WIFI_NAN);
  { ODID_UAS_Data r; odid_initUasData(&r); assert(rid_apply_payload(&r, p, pl)); check(r, "Wi-Fi NAN"); }

  // 3) BLE 4 legacy: uma mensagem por anúncio, acumulada
  {
    ODID_UAS_Data r; odid_initUasData(&r);
    ODID_BasicID_encoded b; encodeBasicIDMessage(&b, &d.BasicID[0]);
    ODID_Location_encoded l; encodeLocationMessage(&l, &d.Location);
    ODID_System_encoded s; encodeSystemMessage(&s, &d.System);
    ODID_OperatorID_encoded o; encodeOperatorIDMessage(&o, &d.OperatorID);
    const void *msgs[] = {&b, &l, &s, &o};
    for (int i = 0; i < 4; i++) {
      // flags(3) + [len=30][0x16][FA FF][0D][contador][25 B] = 34 bytes
      uint8_t adv[34] = {0x02, 0x01, 0x06, 30, 0x16, 0xFA, 0xFF, 0x0D, (uint8_t)i};
      memcpy(adv + 9, msgs[i], 25);
      assert(rid_find_in_ble_adv(adv, sizeof adv, &p, &pl));
      assert(rid_apply_payload(&r, p, pl));
    }
    check(r, "BLE 4 legacy (4 anúncios)");
  }

  // 4) BLE 5 extended: message pack inteiro
  {
    uint8_t pack[300];
    int pn = odid_message_build_pack(&d, pack, sizeof pack);
    assert(pn > 0);
    uint8_t adv[320] = {0x02, 0x01, 0x06, (uint8_t)(pn + 5), 0x16, 0xFA, 0xFF, 0x0D, 7};
    memcpy(adv + 9, pack, pn);
    assert(rid_find_in_ble_adv(adv, 9 + pn, &p, &pl));
    ODID_UAS_Data r; odid_initUasData(&r); assert(rid_apply_payload(&r, p, pl));
    check(r, "BLE 5 Long Range (message pack)");
  }

  // 5) Lixo não pode dar falso positivo
  uint8_t junk[200]; for (int i = 0; i < 200; i++) junk[i] = i * 37;
  junk[0] = 0x80;
  assert(rid_find_in_wifi(junk, sizeof junk, mac, &p, &pl) == RID_SRC_NONE);
  printf("OK  lixo rejeitado\n");
  return 0;
}
