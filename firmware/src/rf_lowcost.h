// Rádio low-cost para drones sem Remote ID (fase experimental):
//   * nRF24L01+ PA+LNA  → energia canal a canal nos 2,4 GHz (bit RPD, > -64 dBm)
//   * RX5808 (SPI mod)  → RSSI nos 5,8 GHz
// Só ESCUTA. O nRF24 fica sempre em modo de receção; nunca transmite.
//
// Por agora só mede e escreve os varrimentos pela USB (para calibrar com um drone
// conhecido). As regras de alerta vêm depois da calibração.
#pragma once
#include <Arduino.h>

// ---- pinos (ESP32-S3-DevKitC-1; evitam strapping, USB e a PSRAM octal) ----
// nRF24 (SPI)
#define NRF_SCK 12
#define NRF_MOSI 11
#define NRF_MISO 13
#define NRF_CSN 10
#define NRF_CE 9
// RX5808 (SPI de 3 fios por software + RSSI analógico)
#define RX58_DATA 4   // CH1
#define RX58_LE 5     // CH2
#define RX58_CLK 6    // CH3
#define RX58_RSSI 7   // ADC1 (o ADC2 não funciona com o Wi-Fi ligado)

#define RF24_CHANNELS 126          // 2400–2525 MHz, 1 MHz por canal
#define RF58_START_MHZ 5645
#define RF58_STEP_MHZ 5
#define RF58_POINTS 61             // 5645–5945 MHz

struct RfLowcostStatus {
  bool nrfPresent = false;
  bool rx58Present = false;
  uint32_t sweeps24 = 0, sweeps58 = 0;
};

void rfLowcostBegin();                       // deteta os módulos e arranca a tarefa
const RfLowcostStatus &rfLowcostStatus();
