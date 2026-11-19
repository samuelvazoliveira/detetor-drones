#!/bin/sh
# Instalação no Raspberry Pi Zero 2 W (Raspberry Pi OS Lite 64-bit, Bookworm).
# Correr como root a partir desta pasta:  sudo sh install.sh
set -e
apt-get update
apt-get install -y python3-venv python3-dbus bluez rtl-sdr i2c-tools
# O driver DVB do kernel "rouba" o RTL-SDR: bloqueá-lo
echo "blacklist dvb_usb_rtl28xxu" > /etc/modprobe.d/blacklist-rtlsdr.conf
# Relógio DS3231 (I2C): hora certa sem internet
CFG=/boot/firmware/config.txt; [ -f $CFG ] || CFG=/boot/config.txt
grep -q "i2c-rtc,ds3231" $CFG || printf "\ndtparam=i2c_arm=on\ndtoverlay=i2c-rtc,ds3231\n" >> $CFG
apt-get remove -y fake-hwclock || true
mkdir -p /opt/detetor-drones /var/lib/detetor-drones
cp -r detetor /opt/detetor-drones/
python3 -m venv --system-site-packages /opt/detetor-drones/venv
/opt/detetor-drones/venv/bin/pip install bless pyserial
[ -f /etc/detetor-drones.json ] || cp detetor-drones.example.json /etc/detetor-drones.json
cp detetor-drones.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable detetor-drones
echo "Feito. Edita /etc/detetor-drones.json (posição da casa) e reinicia: sudo reboot"
