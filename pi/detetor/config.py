"""Configuração do hub. Os valores por omissão podem ser alterados em /etc/detetor-drones.json."""
import json
import os
from dataclasses import dataclass, field, asdict

CONFIG_PATH = os.environ.get("DETETOR_CONFIG", "/etc/detetor-drones.json")


@dataclass
class RfBand:
    name: str          # nome curto mostrado na app
    start_hz: int
    stop_hz: int
    bin_hz: int
    kind: str          # "hopping" (comandos que saltam de frequência) ou "video" (portadora larga)


@dataclass
class Config:
    db_path: str = "/var/lib/detetor-drones/detecoes.db"
    ble_name: str = "DetetorDrones"
    # Posição da casa (opcional): usada para calcular distâncias no relatório.
    home_lat: float | None = None
    home_lon: float | None = None
    # Um drone conta como "novo" (gera alerta) se não foi visto nos últimos N segundos.
    new_drone_after_s: int = 600
    # Registo de posições: no máximo uma linha por drone a cada N segundos.
    track_interval_s: float = 2.0
    # ---- RTL-SDR ----
    rf_enabled: bool = False          # FPV/caseiros fora do âmbito por agora (19/11)
    rf_gain_db: float = 40.0
    rf_integration_s: float = 1.0
    rf_baseline_sweeps: int = 30       # varrimentos usados para aprender o ruído de fundo
    rf_hot_db: float = 6.0             # dB acima do fundo para um bin contar como "quente"
    rf_hopping_min_bins: int = 8       # bins quentes distintos para parecer salto de frequência
    rf_hopping_min_spread: float = 0.30  # fração da banda que os bins quentes têm de abranger
    rf_video_min_mhz: float = 6.0      # largura contínua mínima para parecer vídeo analógico
    rf_confirm_sweeps: int = 2         # varrimentos seguidos para confirmar
    rf_cooldown_s: int = 300           # silêncio entre alertas da mesma banda
    rf_bands: list[RfBand] = field(default_factory=lambda: [
        RfBand("433 MHz", 430_000_000, 436_000_000, 12_500, "hopping"),
        RfBand("868 MHz", 863_000_000, 870_000_000, 25_000, "hopping"),
        RfBand("915 MHz", 902_000_000, 928_000_000, 50_000, "hopping"),
        RfBand("1,2 GHz", 1_080_000_000, 1_360_000_000, 250_000, "video"),
    ])


def load() -> Config:
    cfg = Config()
    if os.path.exists(CONFIG_PATH):
        with open(CONFIG_PATH) as f:
            data = json.load(f)
        bands = data.pop("rf_bands", None)
        for k, v in data.items():
            if hasattr(cfg, k):
                setattr(cfg, k, v)
        if bands:
            cfg.rf_bands = [RfBand(**b) for b in bands]
    return cfg


def dump(cfg: Config) -> str:
    return json.dumps(asdict(cfg), indent=2, ensure_ascii=False)
