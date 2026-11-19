# Firmware dos recetores de Remote ID

Um só código para as duas placas (escolhida pelo `-e`):

| Placa | O que escuta | Como |
|---|---|---|
| ESP32-S3 (`-e s3`) | Wi-Fi 2,4 GHz (beacon + NAN) e Bluetooth 4 + 5 Long Range | salta pelos canais 1–13 a cada 150 ms, com o canal 6 a cada dois saltos |
| ESP32-C5 (`-e c5`) | Wi-Fi 5 GHz (beacon) | salta pelos canais de 5 GHz da UE, com o 149 a cada dois saltos |

Não se liga a nenhuma rede. Tudo sai por USB (115200 baud), uma linha JSON por mensagem:
- `{"t":"hello","dev":"s3","fw":"2.0"}`
- `{"t":"drone","dev":"s3","id":"1581F…","src":"ble","rssi":-71,"lat":…,"lon":…,"h":87.5,"op_lat":…,"op_lon":…,"eu_class":2,…}`
- `{"t":"hb","dev":"c5","up":120,"frames":5012,"rid":3,"drop":0}` a cada 10 s

## Compilar
`pio run -e s3` / `pio run -e c5`. A biblioteca OpenDroneID é descarregada pelo `get_odid.py`. O C5 precisa do core Arduino 3.3+, que vem da plataforma pioarduino. No GitHub, o workflow compila as duas.

## Testes
`test/test_parser.cpp` gera tramas com a biblioteca oficial (beacon, NAN, BLE 4, BLE 5) e confirma que o parser as lê. Corre no CI.

## Limites
- Drones C0 (<250 g) e artesanais podem não emitir Remote ID.
- O ID é a MAC até chegar a mensagem Basic ID. O mesmo drone pode aparecer pelo S3 e pelo C5.
- A cada salto de canal, o rádio está surdo nos outros. O Remote ID repete-se várias vezes por segundo, por isso apanha-se na mesma, só com um pequeno atraso.
