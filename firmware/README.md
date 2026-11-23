# Firmware dos recetores de Remote ID

Placa: **ESP32-S3** (o C5 ficou de fora; o `-e c5` está no `platformio.ini` mas não é compilado no CI).

| O que escuta | Como |
|---|---|
| Remote ID por Wi-Fi 2,4 GHz (beacon + NAN) | salta pelos canais 1–13 a cada 150 ms, com o canal 6 a cada dois saltos |
| Remote ID por Bluetooth 4 + 5 Long Range | scan NimBLE com janela de 40 % (para o Wi-Fi não ficar surdo) |
| Pista Wi-Fi de drones sem Remote ID | SSIDs TELLO-, Spark-, Mavic, DJI-, ANAFI… e OUIs DJI/Parrot |
| nRF24L01+ (2400–2525 MHz) | bit RPD (> −64 dBm) canal a canal, só receção (`rf_lowcost.cpp`) |
| RX5808 (5645–5945 MHz) | RSSI pelo ADC a cada 5 MHz, via SPI (`rf_lowcost.cpp`) |

Não se liga a nenhuma rede. Guarda os eventos em LittleFS (`/ev.log`) e serve a app por Bluetooth (Nordic UART). Pela USB (115200) sai uma linha JSON por mensagem, por exemplo:
- `{"t":"rf_lowcost","nrf24":true,"rx5808":true}` ao arrancar
- `{"t":"rf24","mhz0":2400,"step":1,"pct":[…]}` e `{"t":"rf58","mhz0":5645,"step":5,"raw":[…]}` a cada varrimento

## Compilar
`pio run -e s3`. A biblioteca OpenDroneID é descarregada pelo `get_odid.py`. Core Arduino 2.0.17 (espressif32@6.9.0) com NimBLE-Arduino 2.5.2. No GitHub, o workflow compila e junta tudo num `detetor-s3-completo.bin`.

## Testes
`test/test_parser.cpp` gera tramas com a biblioteca oficial (beacon, NAN, BLE 4, BLE 5) e confirma que o parser as lê. Corre no CI.

## Limites
- Drones C0 (<250 g) e artesanais podem não emitir Remote ID.
- O ID é a MAC até chegar a mensagem Basic ID.
- A cada salto de canal, o rádio está surdo nos outros. O Remote ID repete-se várias vezes por segundo, por isso apanha-se na mesma, só com um pequeno atraso.
