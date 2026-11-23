# Detetor de Drones — Casa

Sistema **passivo** para detetar e identificar drones por cima de casa e juntar prova para queixa (PSP/GNR/ANAC). Só escuta: nunca transmite para os drones nem interfere com eles. Sem Wi-Fi de casa nem Home Assistant: o ESP32-S3 fala diretamente com a app do telemóvel por Bluetooth.

Foco: **drones comerciais**. FPV e caseiros ficam para depois.

```
 antena 2,4 ──── ESP32-S3 ─────────── Bluetooth ──► app Android
 nRF24L01+ PA/LNA ──┤  (Remote ID + histórico          (alertas, histórico,
 RX5808 (5,8 GHz) ──┘   em LittleFS)                     mapa, relatório PDF)
```

| Pasta | O quê |
|---|---|
| `firmware/` | ESP32-S3: Remote ID (Wi-Fi beacon + NAN, Bluetooth 4 e 5 Long Range), pista de drones por Wi-Fi (TELLO-, DJI, Parrot…), varrimento low-cost de 2,4 e 5,8 GHz. Guarda o histórico e serve a app por Bluetooth (Nordic UART). |
| `app-android/` | App: liga-se sozinha ao detetor, alertas com o ecrã desligado, histórico, mapa do drone e do piloto, relatório PDF. |
| `.github/workflows/` | Compila tudo no GitHub (APK assinado + firmware `.bin`), corre os testes e publica na release **ultima**. |
| `pi/` | ⚠️ Obsoleto (versão antiga com Raspberry Pi). Fica só como referência. |

## Descarregar (sem compilar nada)
[Releases → ultima](../../releases/tag/ultima):
- `DetetorDrones.apk` → instalar no telemóvel (Android 12+). Assinado sempre com a mesma chave, por isso atualiza por cima.
- `detetor-s3-completo.bin` → gravar no ESP32-S3 no endereço `0x0`.

## Gravar o ESP32-S3
Num PC com Chrome: https://web.esphome.io → *Connect* → escolher a porta → *Install* (⬆) → `detetor-s3-completo.bin`. Se não ligar: carregar em BOOT, carregar em RST, largar BOOT e voltar a tentar. Para ver os registos, usar *Logs* no mesmo site.

## Ligações dos rádios low-cost
| Módulo | Pino → GPIO do S3 |
|---|---|
| nRF24L01+ PA/LNA (com adaptador 5 V) | VCC→5V, GND, CE→9, CSN→10, MOSI→11, SCK→12, MISO→13, IRQ não ligado |
| RX5808 (com o mod SPI) | +5V, GND, CH1→4, CH2→5, CH3→6, RSSI→7 |

Montar sempre as antenas antes de ligar a alimentação. Ao arrancar, os registos mostram `{"t":"rf_lowcost","nrf24":true,"rx5808":true}` se os dois módulos foram detetados. Sem eles, o firmware funciona igual, só sem varrimento.

## Protocolo Bluetooth (NUS)
Linhas JSON terminadas em `\n`.
- App → S3: `{"cmd":"time","epoch":…}`, `{"cmd":"hist","since":N,"limit":300}`, `{"cmd":"status"}`
- S3 → App: `alert`, `track`, `hist`, `hist_end`, `status`

## Estado (23/11/2026)
- ✅ Parser de Remote ID testado (beacon, NAN, BLE 4, BLE 5); corre no CI
- ✅ FW 3.0 no S3 + app: liga, acerta a hora, histórico persistente; teste com hotspot "TELLO-TESTE" deu alerta no telemóvel
- ✅ APK com chave fixa (GitHub Secrets)
- 🟡 FW 3.1: varrimento nRF24 (2400–2525 MHz) + RX5808 (5645–5945 MHz). Compila, mas falta testar com as peças. Por agora só mede, ainda sem alertas
- ⏳ Regras "provável DJI (rádio)": dependem dos registos com o Mini 5 Pro ligado
- ⏳ Teste com drone real com Remote ID; montagem na caixa; alcance do Bluetooth no exterior

## Limites
- Drones C0 (<250 g) e caseiros podem não emitir Remote ID. Para esses fica só o rádio, que deteta atividade mas não identifica o drone nem o piloto.
- DJI O4 (Mini 5 Pro, etc.): DroneID cifrado. Com componentes baratos dá para detetar atividade provável, mas não para ler a posição.
- Drones do Estado (polícia, militares) estão isentos de Remote ID.
