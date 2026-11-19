import json
import random

from detetor.config import Config, RfBand
from detetor.hub import Hub
from detetor.rf import BandDetector, parse_rtl_power
from detetor.store import Store


class Clock:
    def __init__(self): self.t = 1_763_550_000.0
    def __call__(self): return self.t


def make_hub(**cfg_kw):
    cfg = Config(db_path=":memory:", home_lat=40.7600, home_lon=-8.5710, **cfg_kw)
    sent, clock, setc = [], Clock(), []
    hub = Hub(cfg, Store(":memory:"), sent.append, clock=clock, rtc_present=False,
              set_clock=setc.append)
    return hub, sent, clock, setc


DRONE = {"t": "drone", "dev": "s3", "id": "1581F5FJD239C00DW22E", "mac": "60:60:1F:11:22:33",
         "src": "wifi_nan", "rssi": -71, "status": 2, "lat": 40.7590, "lon": -8.5700, "h": 87.5,
         "op_lat": 40.7612, "op_lon": -8.5731, "eu_class": 2, "eu_cat": 1, "op_id": "PRTabc"}


def test_alerta_novo_drone_e_registo_de_trajeto():
    hub, sent, clock, _ = make_hub()
    hub.on_esp_line('lixo de arranque\r')
    hub.on_esp_line(json.dumps({"t": "hello", "dev": "s3", "fw": "2.0"}))
    hub.on_esp_line(json.dumps(DRONE))
    a = sent[-1]
    assert a["t"] == "alert" and a["drone_id"] == DRONE["id"]
    assert a["data"]["eu_class_txt"] == "C1" and 100 < a["data"]["dist_m"] < 200
    assert "C1" in a["title"] and "88 m" in a["title"]
    assert a["time_ok"] is False            # sem RTC nem telemóvel: hora marcada como incerta
    # 1 s depois: nem alerta nem trajeto (intervalo de 2 s)
    clock.t += 1; hub.on_esp_line(json.dumps(DRONE)); assert len(sent) == 1
    clock.t += 1.5; hub.on_esp_line(json.dumps(DRONE)); assert sent[-1]["t"] == "track"
    # 11 min depois volta a ser "novo"
    clock.t += 660; hub.on_esp_line(json.dumps(DRONE)); assert sent[-1]["t"] == "alert"


def test_hora_pelo_telemovel_e_historico():
    hub, sent, clock, setc = make_hub()
    hub.on_esp_line(json.dumps(DRONE))
    r = hub.on_command(json.dumps({"cmd": "time", "epoch": clock.t + 3600}))
    assert setc == [clock.t + 3600] and r[0]["time_ok"] is True
    clock.t += 3600
    hub.on_esp_line(json.dumps(DRONE | {"id": "OUTRO"}))
    assert sent[-1]["time_ok"] is True
    h = hub.on_command(b'{"cmd":"hist","since":0}')
    kinds = [m["t"] for m in h]
    assert kinds == ["hist", "hist", "hist_end"]          # eventos de sistema não vão
    h2 = hub.on_command(json.dumps({"cmd": "hist", "since": h[-1]["last_id"]}))
    assert [m["t"] for m in h2] == ["hist_end"]
    assert hub.on_command(b"{nada")[0]["t"] == "error"


def test_estado_dos_aparelhos():
    hub, _, clock, _ = make_hub()
    hub.on_esp_line(json.dumps({"t": "hb", "dev": "c5", "up": 10, "frames": 5, "rid": 0}))
    hub.on_rf_status(True)
    st = hub.status()
    assert st["devs"]["c5"]["ok"] and st["devs"]["sdr"]["ok"] and not st["devs"]["s3"]["ok"]
    clock.t += 60
    assert not hub.status()["devs"]["c5"]["ok"]


# ---------------------------------------------------------------- RF
def spectrum(n, base=-40.0, hot=(), extra=15.0, seed=0):
    rnd = random.Random(seed)
    return [(868_000_000 + i * 25_000, base + rnd.uniform(-1.5, 1.5) + (extra if i in hot else 0))
            for i in range(n)]


def learn(det, n=280, sweeps=10):
    for s in range(sweeps):
        assert det.feed(spectrum(n, seed=s), now=s) is None


def test_rtl_power_csv():
    lines = ["2026-11-19, 12:00:00, 863000000, 865000000, 25000.00, 100, -40.1, -39.5, nan, -41",
             "2026-11-19, 12:00:00, 865000000, 867000000, 25000.00, 100, -38.0", "lixo"]
    s = parse_rtl_power(lines)
    assert s[0] == (863_000_000, -40.1) and len(s) == 4 and s[-1][0] == 865_000_000


def test_salto_de_frequencia_dispara():
    cfg = Config(db_path=":memory:")
    det = BandDetector(RfBand("868 MHz", 863_000_000, 870_000_000, 25_000, "hopping"), cfg)
    learn(det)
    hop = set(range(5, 275, 17))                              # 16 canais espalhados
    assert det.feed(spectrum(280, hot=hop, seed=50), now=100) is None   # 1.º varrimento: espera
    f = det.feed(spectrum(280, hot=hop, seed=51), now=101)
    assert f and f.pattern == "hopping" and f.hot_bins >= 8
    assert det.feed(spectrum(280, hot=hop, seed=52), now=102) is None   # cooldown


def test_comando_de_garagem_nao_dispara():
    cfg = Config(db_path=":memory:")
    det = BandDetector(RfBand("433 MHz", 430_000_000, 436_000_000, 12_500, "hopping"), cfg)
    learn(det, n=480)
    for s in range(5):                                         # sinal forte mas em 3 bins fixos
        assert det.feed(spectrum(480, hot={200, 201, 202}, extra=30, seed=60 + s), now=200 + s) is None
    # bloco largo mas único (ex.: LoRa/contador) também não
    for s in range(5):
        assert det.feed(spectrum(480, hot=set(range(100, 160)), seed=70 + s), now=300 + s) is None


def test_video_analogico_dispara():
    cfg = Config(db_path=":memory:")
    det = BandDetector(RfBand("1,2 GHz", 1_080_000_000, 1_360_000_000, 250_000, "video"), cfg)
    learn(det, n=1120)
    vid = set(range(400, 440))                                 # 10 MHz contínuos
    det.feed(spectrum(1120, hot=vid, seed=80), now=100)
    f = det.feed(spectrum(1120, hot=vid, seed=81), now=101)
    assert f and f.pattern == "video" and f.span_mhz >= 6
    # portadora estreita (2 MHz) não
    det2 = BandDetector(RfBand("1,2 GHz", 1_080_000_000, 1_360_000_000, 250_000, "video"), cfg)
    learn(det2, n=1120)
    for s in range(4):
        assert det2.feed(spectrum(1120, hot=set(range(400, 408)), seed=90 + s), now=s) is None


def test_alerta_rf_chega_a_app():
    from detetor.rf import RfFinding
    hub, sent, _, _ = make_hub()
    hub.on_rf(RfFinding("868 MHz", "hopping", 16, 6.8, 15.2, 866.1))
    assert sent[-1]["t"] == "alert" and "FPV" in sent[-1]["title"]
