"""Leitura das portas USB dos ESP32 (uma thread por porta, com religação automática)."""
import glob
import logging
import threading
import time

log = logging.getLogger("serial")

PATTERNS = ["/dev/serial/by-id/*", "/dev/ttyACM*", "/dev/ttyUSB*"]


def _ports() -> list[str]:
    by_id = glob.glob(PATTERNS[0])
    if by_id:
        # O RTL-SDR não aparece aqui (não é porta série), por isso só sobram os ESP32.
        return sorted(by_id)
    return sorted(glob.glob(PATTERNS[1]) + glob.glob(PATTERNS[2]))


class SerialWatcher:
    def __init__(self, on_line):
        self.on_line = on_line
        self.active: set[str] = set()
        self.lock = threading.Lock()

    def start(self):
        threading.Thread(target=self._scan_loop, daemon=True).start()

    def _scan_loop(self):
        while True:
            for p in _ports():
                with self.lock:
                    if p in self.active:
                        continue
                    self.active.add(p)
                threading.Thread(target=self._reader, args=(p,), daemon=True).start()
            time.sleep(5)

    def _reader(self, port: str):
        import serial  # pyserial
        try:
            with serial.Serial(port, 115200, timeout=2) as s:
                log.info("a ler %s", port)
                buf = b""
                while True:
                    chunk = s.read(512)
                    if not chunk:
                        continue
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        line = line.strip()
                        if line.startswith(b"{"):
                            self.on_line(line.decode(errors="replace"), port)
                    if len(buf) > 8192:
                        buf = b""
        except Exception as e:
            log.warning("porta %s: %s", port, e)
        finally:
            with self.lock:
                self.active.discard(port)
