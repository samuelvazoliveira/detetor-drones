"""Camada de RF: varre bandas com o rtl_power e procura padrões de drone.

Não identifica drones. Procura dois padrões, para não disparar com cada comando de garagem:
  * "hopping": muitos canais estreitos ativos, espalhados pela banda (ExpressLRS, Crossfire…).
    Um comando de garagem ou uma estação meteorológica ocupa 1–2 canais fixos.
  * "video":   uma faixa larga e contínua ocupada (vídeo analógico FPV, 1,2–1,3 GHz).
"""
import asyncio
import logging
import statistics
from collections import deque
from dataclasses import dataclass

from .config import Config, RfBand

log = logging.getLogger("rf")


def parse_rtl_power(lines) -> list[tuple[int, float]]:
    """Converte linhas CSV do rtl_power numa lista ordenada (frequência_Hz, dB)."""
    out = []
    for line in lines:
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 7:
            continue
        try:
            lo, step = float(parts[2]), float(parts[4])
            vals = [float(v) for v in parts[6:] if v and v.lower() != "nan"]
        except ValueError:
            continue
        for i, v in enumerate(vals):
            out.append((int(lo + i * step), v))
    out.sort()
    return out


@dataclass
class RfFinding:
    band: str
    pattern: str        # "hopping" | "video"
    hot_bins: int
    span_mhz: float     # largura abrangida pelos bins quentes
    peak_db: float      # quanto o bin mais forte está acima do fundo
    peak_mhz: float


class BandDetector:
    def __init__(self, band: RfBand, cfg: Config):
        self.band, self.cfg = band, cfg
        self.history: deque[list[float]] = deque(maxlen=cfg.rf_baseline_sweeps)
        self.streak = 0
        self.last_alert = float("-inf")

    def baseline(self) -> list[float] | None:
        if len(self.history) < min(5, self.cfg.rf_baseline_sweeps):
            return None
        n = min(len(h) for h in self.history)
        return [statistics.median(h[i] for h in self.history) for i in range(n)]

    def feed(self, spectrum: list[tuple[int, float]], now: float) -> RfFinding | None:
        if not spectrum:
            return None
        powers = [p for _, p in spectrum]
        base = self.baseline()
        finding = None
        if base:
            n = min(len(base), len(powers))
            excess = [powers[i] - base[i] for i in range(n)]
            hot = [i for i in range(n) if excess[i] >= self.cfg.rf_hot_db]
            finding = self._classify(spectrum, excess, hot, n)
        # Só aprende o fundo com varrimentos "calmos", para um drone não virar ruído de fundo.
        if finding is None:
            self.history.append(powers)
            self.streak = 0
            return None
        self.streak += 1
        if self.streak < self.cfg.rf_confirm_sweeps:
            return None
        if now - self.last_alert < self.cfg.rf_cooldown_s:
            return None
        self.last_alert = now
        return finding

    def _classify(self, spectrum, excess, hot, n) -> RfFinding | None:
        if not hot:
            return None
        bin_hz = self.band.bin_hz
        span_bins = hot[-1] - hot[0] + 1
        peak_i = max(hot, key=lambda i: excess[i])
        common = dict(band=self.band.name, hot_bins=len(hot),
                      span_mhz=round(span_bins * bin_hz / 1e6, 2),
                      peak_db=round(excess[peak_i], 1),
                      peak_mhz=round(spectrum[peak_i][0] / 1e6, 3))
        if self.band.kind == "video":
            run = best = 1
            for a, b in zip(hot, hot[1:]):
                run = run + 1 if b - a <= 2 else 1
                best = max(best, run)
            if best * bin_hz / 1e6 >= self.cfg.rf_video_min_mhz:
                return RfFinding(pattern="video", **common)
            return None
        # hopping: muitos bins, espalhados, e sem um único bloco a dominar
        if len(hot) < self.cfg.rf_hopping_min_bins:
            return None
        if span_bins / n < self.cfg.rf_hopping_min_spread:
            return None
        clusters, prev = 1, hot[0]
        for i in hot[1:]:
            if i - prev > 3:
                clusters += 1
            prev = i
        if clusters < 4:
            return None
        return RfFinding(pattern="hopping", **common)


class RfScanner:
    """Corre o rtl_power banda a banda, em ciclo, e chama on_finding quando vê um padrão."""

    def __init__(self, cfg: Config, on_finding, on_status):
        self.cfg = cfg
        self.on_finding, self.on_status = on_finding, on_status
        self.detectors = [BandDetector(b, cfg) for b in cfg.rf_bands]
        self.ok = False

    async def sweep(self, band: RfBand) -> list[tuple[int, float]]:
        cmd = ["rtl_power", "-f", f"{band.start_hz}:{band.stop_hz}:{band.bin_hz}",
               "-i", str(self.cfg.rf_integration_s), "-g", str(self.cfg.rf_gain_db), "-1", "-"]
        proc = await asyncio.create_subprocess_exec(
            *cmd, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
        out, _ = await asyncio.wait_for(proc.communicate(), timeout=60)
        if proc.returncode != 0:
            raise RuntimeError(f"rtl_power saiu com {proc.returncode}")
        return parse_rtl_power(out.decode(errors="replace").splitlines())

    async def run(self):
        loop = asyncio.get_running_loop()
        while True:
            for det in self.detectors:
                try:
                    spec = await self.sweep(det.band)
                    if not self.ok:
                        self.ok = True
                        self.on_status(True)
                    f = det.feed(spec, loop.time())
                    if f:
                        self.on_finding(f)
                except FileNotFoundError:
                    log.error("rtl_power não está instalado (apt install rtl-sdr)")
                    self.on_status(False)
                    return
                except Exception as e:  # SDR desligado, ocupado, etc.
                    if self.ok:
                        log.warning("RTL-SDR falhou: %s", e)
                    self.ok = False
                    self.on_status(False)
                    await asyncio.sleep(10)
