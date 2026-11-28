// Rádio low-cost para drones sem Remote ID (fase experimental):
//   * nRF24L01+ PA+LNA  → energia canal a canal nos 2,4 GHz (bit RPD, > -64 dBm)
//   * RX5808 (SPI mod)  → RSSI nos 5,8 GHz
// Só ESCUTA. O nRF24 fica sempre em modo de receção; nunca transmite.
//
// Escreve os varrimentos pela USB e, nos 2,4 GHz, calcula uma pontuação de "atividade
// tipo DJI": quantos canais estão ocupados em cada varrimento, sem contar o canal mais
// forte (o router da casa) e os vizinhos dele, nem os 3 canais de anúncio Bluetooth, em média nos últimos RF24_WIN varrimentos.
// Calibrado a 28/11 com o Mini 5 Pro: casa calma 0,2–0,6; drone ligado 2–6.
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
  float score24 = 0;        // média de canais ocupados fora do router
  bool active24 = false;    // há atividade tipo drone nos 2,4 GHz
  uint16_t peakMhz24 = 0;   // canal extra mais usado no último varrimento
};

#define RF24_WIN 30          // ~20 s de varrimentos
#define RF24_ON 2.0f         // liga acima disto
#define RF24_OFF 1.0f        // desliga abaixo disto (calibrado 28/11: casa calma 0,3–0,7)
#define RF24_WARMUP_MS 60000 // ignora o primeiro minuto (o telemóvel a ligar-se e a puxar o histórico faz ruído)

// Pura (testada no CI): canais >= 10 % fora do mais forte e dos vizinhos dele.
int rf24BusyChannels(const uint8_t *pct, int n, int *peakExtra);

void rfLowcostBegin();                       // deteta os módulos e arranca a tarefa
const RfLowcostStatus &rfLowcostStatus();
