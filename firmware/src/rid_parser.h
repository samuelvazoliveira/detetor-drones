// Parser de tramas Remote ID (ASTM F3411 / EN 4709-002) — sem dependências do ESP32,
// para poder ser testado no PC.
#pragma once
#include <stddef.h>
#include <stdint.h>

extern "C" {
#include "opendroneid.h"
}

enum RidSource : uint8_t {
  RID_SRC_NONE = 0,
  RID_SRC_WIFI_BEACON,
  RID_SRC_WIFI_NAN,
  RID_SRC_BLE,
};

const char *rid_source_name(RidSource s);

// Procura Remote ID numa trama 802.11 de gestão (beacon ou NAN action frame).
// Em caso de sucesso preenche mac (endereço de origem), payload (pack ou mensagem)
// e devolve a origem. Não aloca nada; payload aponta para dentro de frame.
RidSource rid_find_in_wifi(const uint8_t *frame, size_t len, uint8_t mac[6],
                           const uint8_t **payload, size_t *payload_len);

// Procura Remote ID nos dados de anúncio BLE (AD structures, legacy ou extended).
bool rid_find_in_ble_adv(const uint8_t *adv, size_t len, const uint8_t **payload,
                         size_t *payload_len);

// Aplica uma mensagem isolada (25 B) ou um message pack a uas (acumula,
// não limpa os campos já recebidos). Devolve true se descodificou alguma coisa.
bool rid_apply_payload(ODID_UAS_Data *uas, const uint8_t *payload, size_t len);
