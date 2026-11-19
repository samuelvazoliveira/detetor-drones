#!/bin/sh
# Instalação no Raspberry Pi Zero 2 W (Raspberry Pi OS Lite 64-bit, Bookworm).
# Correr como root a partir desta pasta:  sudo sh install.sh
set -e
apt-get update
apt-get install -y python3-venv python3-dbus bluez
# (RTL-SDR fora do âmbito por agora; para o reativar: apt install rtl-sdr, bloquear o
#  módulo dvb_usb_rtl28xxu e pôr "rf_enabled": true no /etc/detetor-drones.json)
# Sem relógio RTC: o fake-hwclock guarda a última hora ao desligar e a app acerta-a ao ligar
mkdir -p /opt/detetor-drones /var/lib/detetor-drones
cp -r detetor /opt/detetor-drones/
python3 -m venv --system-site-packages /opt/detetor-drones/venv
/opt/detetor-drones/venv/bin/pip install bless pyserial
[ -f /etc/detetor-drones.json ] || cp detetor-drones.example.json /etc/detetor-drones.json
cp detetor-drones.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable detetor-drones
echo "Feito. Edita /etc/detetor-drones.json (posição da casa) e reinicia: sudo reboot"
