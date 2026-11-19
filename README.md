# Detetor de Drones — Casa

Sistema **passivo** para detetar e identificar drones por cima de casa e juntar prova para queixa (PSP/GNR/ANAC). Só escuta: nunca transmite para os drones nem interfere com eles. Sem Wi-Fi de casa nem Home Assistant: tudo chega ao telemóvel por Bluetooth.

```
 antena 2,4 ─ ESP32-S3 ─┐ USB
 antena 5   ─ ESP32-C5 ─┼──── hub ──── Raspberry Pi Zero 2 W ── Bluetooth ──► app Android
 antena SDR ─ RTL-SDR ──┘                (registo SQLite)
```

| Pasta | O quê |
|---|---|
| `firmware/` | ESP32-S3 (Remote ID 2,4 GHz: Wi-Fi + Bluetooth 4/5 LR) e ESP32-C5 (Remote ID 5 GHz). JSON por USB. |
| `pi/` | Hub no Pi: lê os ESP32, varre 433 MHz–1,3 GHz com o RTL-SDR, guarda tudo e serve a app por Bluetooth (Nordic UART). |
| `app-android/` | App: alertas com o ecrã desligado, histórico, mapa do drone e do piloto, relatório PDF. |
| `.github/workflows/` | Compila tudo no GitHub (APK + firmware .bin) e corre os testes. |

## Compilar (sem PC)
Cada `push` corre o workflow **Compilar tudo**. Em *Actions → última execução → Artifacts* ficam:
- `app-detetor-drones` → o APK para instalar no telemóvel
- `firmware-s3`, `firmware-c5` → os `.bin` para gravar nos ESP32

## Gravar os ESP32
Num PC com Chrome: https://espressif.github.io/esptool-js/ → *Connect* → endereço `0x0` → ficheiro `firmware.factory.bin` (ou `pio run -e s3 -t upload` / `-e c5` com o PlatformIO).

## Instalar o Pi
Raspberry Pi OS Lite 64-bit. Copiar a pasta `pi/` e correr `sudo sh install.sh`. Editar `/etc/detetor-drones.json` com a posição da casa e reiniciar.

## Protocolo Bluetooth (NUS)
Linhas JSON terminadas em `\n`. App → Pi: `{"cmd":"time","epoch":…}`, `{"cmd":"hist","since":N}`, `{"cmd":"status"}`. Pi → App: `alert`, `track`, `hist`, `hist_end`, `status`.

## Estado (19/11/2026)
- ✅ Parser de Remote ID testado (beacon, NAN, BLE 4, BLE 5)
- ✅ Hub do Pi testado com dados simulados (alertas, trajetos, histórico, hora, deteção RF)
- ⏳ Firmware e app ainda não compilados: a primeira execução do GitHub Actions é o primeiro teste
- ⏳ Afinação da deteção RF com o equipamento real (limiares em `/etc/detetor-drones.json`)
