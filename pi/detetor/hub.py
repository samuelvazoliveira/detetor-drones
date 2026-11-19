"""Lógica central do hub: recebe deteções dos ESP32 e do SDR, decide alertas, guarda tudo e
responde aos pedidos da app. Não sabe nada de Bluetooth nem de portas série (testável no PC)."""
import json
import logging
import math
import os
import subprocess
import time

from .config import Config
from .rf import RfFinding
from .store import Store

log = logging.getLogger("hub")

EU_CLASS = {0: None, 1: "C0", 2: "C1", 3: "C2", 4: "C3", 5: "C4", 6: "C5", 7: "C6"}
UA_TYPE = {0: "desconhecido", 1: "avião", 2: "multirotor", 3: "giroplano", 4: "VTOL",
           5: "ornitóptero", 6: "planador", 7: "papagaio", 8: "balão livre", 9: "balão cativo",
           10: "dirigível", 11: "paraquedas", 12: "foguete", 13: "cabo", 14: "solo", 15: "outro"}


def haversine_m(lat1, lon1, lat2, lon2) -> float:
    r = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


class Hub:
    def __init__(self, cfg: Config, store: Store, send, clock=time.time,
                 rtc_present: bool | None = None, set_clock=None):
        self.cfg, self.store, self.send, self.clock = cfg, store, send, clock
        self.rtc_present = os.path.exists("/dev/rtc0") if rtc_present is None else rtc_present
        self.time_synced = False
        self.set_clock = set_clock or self._set_system_clock
        self.last_seen: dict[str, float] = {}     # drone_id -> última vez visto
        self.last_track: dict[str, float] = {}    # drone_id -> último registo de posição
        self.devices: dict[str, dict] = {}        # 's3'/'c5'/'sdr' -> estado

    # ------------------------------------------------------------------ hora
    @property
    def time_ok(self) -> bool:
        return self.rtc_present or self.time_synced

    @staticmethod
    def _set_system_clock(epoch: float):
        subprocess.run(["date", "-u", "-s", f"@{int(epoch)}"], check=False,
                       stdout=subprocess.DEVNULL)
        if os.path.exists("/dev/rtc0"):
            subprocess.run(["hwclock", "-w"], check=False)

    # ------------------------------------------------------------------ entradas
    def on_esp_line(self, line: str, port: str = "?"):
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            return  # lixo de arranque do ESP32
        t, dev = msg.get("t"), msg.get("dev", port)
        now = self.clock()
        st = self.devices.setdefault(dev, {})
        st["last"] = now
        if t == "hello":
            st["fw"] = msg.get("fw")
            log.info("ESP32 %s ligado (fw %s) em %s", dev, msg.get("fw"), port)
        elif t == "hb":
            st.update(up=msg.get("up"), frames=msg.get("frames"), rid=msg.get("rid"))
        elif t == "drone":
            self.on_drone(msg, now)

    def on_drone(self, msg: dict, now: float):
        did = str(msg.get("id") or msg.get("mac"))
        data = self._enrich(msg)
        prev = self.last_seen.get(did)
        self.last_seen[did] = now
        if prev is None or now - prev > self.cfg.new_drone_after_s:
            ev = self.store.add("alert", msg.get("dev", "?"), data | {"pattern": "remote_id"},
                                drone_id=did, time_ok=self.time_ok, ts=now)
            self.last_track[did] = now
            self.send({"t": "alert", **ev, "title": self._title(data)})
            return
        if now - self.last_track.get(did, 0) >= self.cfg.track_interval_s:
            self.last_track[did] = now
            ev = self.store.add("track", msg.get("dev", "?"), data, drone_id=did,
                                time_ok=self.time_ok, ts=now)
            self.send({"t": "track", **ev})

    def on_rf(self, f: RfFinding):
        data = {"band": f.band, "pattern": f.pattern, "hot_bins": f.hot_bins,
                "span_mhz": f.span_mhz, "peak_db": f.peak_db, "peak_mhz": f.peak_mhz}
        ev = self.store.add("alert", "sdr", data, time_ok=self.time_ok, ts=self.clock())
        what = "comando FPV (saltos de frequência)" if f.pattern == "hopping" \
            else "vídeo analógico FPV"
        self.send({"t": "alert", **ev, "title": f"Rádio: {what} em {f.band}"})

    def on_rf_status(self, ok: bool):
        self.devices.setdefault("sdr", {}).update(ok=ok, last=self.clock())

    # ------------------------------------------------------------------ app
    def on_command(self, raw: bytes | str) -> list[dict]:
        try:
            cmd = json.loads(raw)
        except (json.JSONDecodeError, UnicodeDecodeError):
            return [{"t": "error", "msg": "JSON inválido"}]
        c = cmd.get("cmd")
        if c == "time":
            epoch = float(cmd["epoch"])
            delta = epoch - self.clock()
            if abs(delta) > 2:
                self.set_clock(epoch)
                self.store.add("system", "hub", {"msg": "hora acertada pelo telemóvel",
                                                 "delta_s": round(delta, 1)}, ts=epoch)
            self.time_synced = True
            return [self.status()]
        if c == "status":
            return [self.status()]
        if c == "hist":
            since, limit = int(cmd.get("since", 0)), min(int(cmd.get("limit", 300)), 1000)
            rows = self.store.since(since, limit)
            out = [{"t": "hist", **r} for r in rows]
            last = rows[-1]["id"] if rows else since
            out.append({"t": "hist_end", "last_id": last, "more": len(rows) == limit})
            return out
        return [{"t": "error", "msg": f"comando desconhecido: {c}"}]

    def status(self) -> dict:
        now = self.clock()
        devs = {}
        for name in ("s3", "c5", "sdr"):
            st = self.devices.get(name, {})
            alive = bool(st) and now - st.get("last", 0) < 30 and st.get("ok", True)
            devs[name] = {"ok": alive, **{k: v for k, v in st.items() if k != "last"}}
        return {"t": "status", "time_ok": self.time_ok, "rtc": self.rtc_present, "now": now,
                "last_id": self.store.last_id(), "alerts": self.store.count("alert"),
                "devs": devs, "home": [self.cfg.home_lat, self.cfg.home_lon]}

    # ------------------------------------------------------------------ util
    def _enrich(self, msg: dict) -> dict:
        d = {k: v for k, v in msg.items() if k not in ("t",)}
        if "eu_class" in d:
            d["eu_class_txt"] = EU_CLASS.get(d["eu_class"])
        if "ua_type" in d:
            d["ua_type_txt"] = UA_TYPE.get(d["ua_type"], "?")
        h = (self.cfg.home_lat, self.cfg.home_lon)
        if None not in h:
            if "lat" in d and "lon" in d:
                d["dist_m"] = round(haversine_m(*h, float(d["lat"]), float(d["lon"])))
            if "op_lat" in d and "op_lon" in d:
                d["op_dist_m"] = round(haversine_m(*h, float(d["op_lat"]), float(d["op_lon"])))
        return d

    @staticmethod
    def _title(d: dict) -> str:
        parts = [f"Drone {d.get('id')}"]
        if d.get("eu_class_txt"):
            parts.append(d["eu_class_txt"])
        if "h" in d:
            parts.append(f"{d['h']:.0f} m")
        if "dist_m" in d:
            parts.append(f"a {d['dist_m']} m de casa")
        return " · ".join(parts)
