#include "rf_lowcost.h"

#include <SPI.h>

static RfLowcostStatus st;
static SPIClass nrfSpi(FSPI);

// =====================================================================================
// nRF24L01+ (só receção)
// =====================================================================================
static uint8_t nrfRead(uint8_t reg) {
  nrfSpi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(NRF_CSN, LOW);
  nrfSpi.transfer(0x00 | (reg & 0x1F));  // R_REGISTER
  uint8_t v = nrfSpi.transfer(0xFF);
  digitalWrite(NRF_CSN, HIGH);
  nrfSpi.endTransaction();
  return v;
}

static void nrfWrite(uint8_t reg, uint8_t val) {
  nrfSpi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(NRF_CSN, LOW);
  nrfSpi.transfer(0x20 | (reg & 0x1F));  // W_REGISTER
  nrfSpi.transfer(val);
  digitalWrite(NRF_CSN, HIGH);
  nrfSpi.endTransaction();
}

static bool nrfBegin() {
  pinMode(NRF_CSN, OUTPUT);
  pinMode(NRF_CE, OUTPUT);
  digitalWrite(NRF_CSN, HIGH);
  digitalWrite(NRF_CE, LOW);
  nrfSpi.begin(NRF_SCK, NRF_MISO, NRF_MOSI, NRF_CSN);
  // presença: escreve um canal e lê-o de volta. O nRF24 precisa de ~100 ms depois de
  // receber corrente, e o regulador do adaptador ainda demora, por isso tenta várias vezes.
  uint8_t a = 0, b = 0;
  for (int tent = 0; tent < 5; tent++) {
    delay(150);
    nrfWrite(0x05, 0x2A);  // RF_CH
    a = nrfRead(0x05);
    nrfWrite(0x05, 0x10);
    b = nrfRead(0x05);
    if (a == 0x2A && b == 0x10) break;
  }
  // diagnóstico: 0x00 = MISO sem ligação ou sem corrente; 0xFF = MISO no ar; outro = fios trocados
  Serial.printf("{\"t\":\"nrf24_diag\",\"rd1\":%u,\"rd2\":%u,\"status\":%u}\n", a, b, nrfRead(0x07));
  if (a != 0x2A || b != 0x10) return false;
  nrfWrite(0x01, 0x00);  // EN_AA: sem auto-ack (o auto-ack faria o módulo transmitir)
  nrfWrite(0x02, 0x00);  // EN_RXADDR: nenhum pipe
  nrfWrite(0x06, 0x07);  // RF_SETUP: 1 Mbps (largura de banda do recetor mais estreita)
  nrfWrite(0x00, 0x03);  // CONFIG: PWR_UP | PRIM_RX  (receção)
  delay(5);
  return true;
}

// Fração de amostras com RPD=1 (sinal > -64 dBm) em cada canal, 0–100.
static void nrfSweep(uint8_t out[RF24_CHANNELS], int samples) {
  for (int ch = 0; ch < RF24_CHANNELS; ch++) {
    int hits = 0;
    nrfWrite(0x05, ch);
    for (int i = 0; i < samples; i++) {
      digitalWrite(NRF_CE, HIGH);
      delayMicroseconds(170);  // 130 µs de arranque do recetor + 40 µs de medição
      digitalWrite(NRF_CE, LOW);
      if (nrfRead(0x09) & 0x01) hits++;  // RPD
    }
    out[ch] = (uint8_t)(hits * 100 / samples);
    if ((ch & 7) == 7) vTaskDelay(1);  // deixa respirar as outras tarefas
  }
}

// =====================================================================================
// RX5808 / RTC6715 (só receção; SPI de 3 fios, palavras de 25 bits LSB primeiro)
// =====================================================================================
static void rx58Bit(bool b) {
  digitalWrite(RX58_DATA, b);
  delayMicroseconds(1);
  digitalWrite(RX58_CLK, HIGH);
  delayMicroseconds(1);
  digitalWrite(RX58_CLK, LOW);
  delayMicroseconds(1);
}

static void rx58WriteReg(uint8_t addr, uint32_t data) {
  digitalWrite(RX58_LE, LOW);
  for (int i = 0; i < 4; i++) rx58Bit((addr >> i) & 1);
  rx58Bit(1);  // escrita
  for (int i = 0; i < 20; i++) rx58Bit((data >> i) & 1);
  digitalWrite(RX58_LE, HIGH);
}

static void rx58SetFreq(int mhz) {
  // registo 1 (sintetizador B): f = 2 * (N*32 + A) + 479
  uint32_t tf = (mhz - 479) / 2;
  uint32_t n = tf / 32, a = tf % 32;
  rx58WriteReg(0x01, (n << 7) | a);
}

static bool rx58Begin() {
  pinMode(RX58_DATA, OUTPUT);
  pinMode(RX58_LE, OUTPUT);
  pinMode(RX58_CLK, OUTPUT);
  digitalWrite(RX58_LE, HIGH);
  digitalWrite(RX58_CLK, LOW);
  analogReadResolution(12);
  analogSetPinAttenuation(RX58_RSSI, ADC_11db);
  // presença: sem módulo, o pino RSSI fica no ar e salta muito, ou fica colado a 0.
  // Com módulo, o RSSI tem um nível de repouso estável (~0,4–1,2 V).
  rx58SetFreq(5800);
  delay(40);
  long sum = 0, mn = 4095, mx = 0;
  for (int i = 0; i < 32; i++) {
    int v = analogRead(RX58_RSSI);
    sum += v;
    mn = min(mn, (long)v);
    mx = max(mx, (long)v);
    delay(1);
  }
  long avg = sum / 32;
  return avg > 200 && avg < 3000 && (mx - mn) < 400;
}

static void rx58Sweep(uint16_t out[RF58_POINTS]) {
  for (int i = 0; i < RF58_POINTS; i++) {
    rx58SetFreq(RF58_START_MHZ + i * RF58_STEP_MHZ);
    vTaskDelay(pdMS_TO_TICKS(30));  // o RSSI do RX5808 precisa de ~25 ms para assentar
    uint32_t s = 0;
    for (int k = 0; k < 4; k++) s += analogRead(RX58_RSSI);
    out[i] = s / 4;
  }
}

// =====================================================================================
// Tarefa: varre as duas bandas em ciclo e escreve pela USB
// =====================================================================================
static void rfTask(void *) {
  static uint8_t s24[RF24_CHANNELS];
  static uint16_t s58[RF58_POINTS];
  char buf[RF24_CHANNELS * 4 + 64];
  for (;;) {
    if (st.nrfPresent) {
      nrfSweep(s24, 20);
      st.sweeps24++;
      int n = snprintf(buf, sizeof(buf), "{\"t\":\"rf24\",\"mhz0\":2400,\"step\":1,\"pct\":[");
      for (int i = 0; i < RF24_CHANNELS && n < (int)sizeof(buf) - 8; i++)
        n += snprintf(buf + n, sizeof(buf) - n, i ? ",%u" : "%u", s24[i]);
      snprintf(buf + n, sizeof(buf) - n, "]}");
      Serial.println(buf);
    }
    if (st.rx58Present) {
      rx58Sweep(s58);
      st.sweeps58++;
      int n = snprintf(buf, sizeof(buf), "{\"t\":\"rf58\",\"mhz0\":%d,\"step\":%d,\"raw\":[",
                       RF58_START_MHZ, RF58_STEP_MHZ);
      for (int i = 0; i < RF58_POINTS && n < (int)sizeof(buf) - 8; i++)
        n += snprintf(buf + n, sizeof(buf) - n, i ? ",%u" : "%u", s58[i]);
      snprintf(buf + n, sizeof(buf) - n, "]}");
      Serial.println(buf);
    }
    if (!st.nrfPresent && !st.rx58Present) vTaskDelete(nullptr);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void rfLowcostBegin() {
  st.nrfPresent = nrfBegin();
  st.rx58Present = rx58Begin();
  Serial.printf("{\"t\":\"rf_lowcost\",\"nrf24\":%s,\"rx5808\":%s}\n",
                st.nrfPresent ? "true" : "false", st.rx58Present ? "true" : "false");
  if (st.nrfPresent || st.rx58Present)
    xTaskCreatePinnedToCore(rfTask, "rf_lowcost", 6144, nullptr, 1, nullptr, 1);
}

const RfLowcostStatus &rfLowcostStatus() { return st; }
