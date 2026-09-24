"""RF arithmetic: LoRa airtime, path loss, noise and demodulation limits."""

from __future__ import annotations

import math
from dataclasses import dataclass, field

# 869.85 MHz free-space loss at 1 m: 20 log10(4 pi d / lambda)
FSPL_1M_DB = 31.2

# Required SNR to demodulate (SX126x datasheet, typical).
DEMOD_SNR_DB = {5: -2.5, 6: -5.0, 7: -7.5, 8: -10.0, 9: -12.5, 10: -15.0, 11: -17.5, 12: -20.0}

CODING_RATE = 1  # 4/5
PREAMBLE_SYMS = 8


def airtime_us(length: int, sf: int, bw_khz: int = 250) -> int:
    """Semtech's time on air, as link_airtime_us() in common/link/link_sched.c."""
    ldro = sf >= 11 and bw_khz <= 125
    bits = 8 * length - 4 * sf + 16
    per_block = 4 * (sf - (2 if ldro else 0))
    if sf <= 6:
        bits += 20
        preamble_q = 4 * PREAMBLE_SYMS + 25
    else:
        bits += 28
        preamble_q = 4 * PREAMBLE_SYMS + 17
    blocks = (bits + per_block - 1) // per_block if bits > 0 else 0
    quarters = preamble_q + 4 * (8 + blocks * (CODING_RATE + 4))
    return (quarters * (1 << sf) * 1000) // (4 * bw_khz)


def dbm_to_mw(dbm: float) -> float:
    return 10.0 ** (dbm / 10.0)


def mw_to_dbm(mw: float) -> float:
    return 10.0 * math.log10(mw) if mw > 0 else -300.0


@dataclass
class RfParams:
    """Everything about the channel the UI can change."""

    # Log-distance path loss: pl0 at 1 m, then exponent * 10 dB per decade.
    pl0_db: float = FSPL_1M_DB
    exponent: float = 3.0
    noise_figure_db: float = 6.0
    bw_khz: int = 250
    # A frame survives a same-SF interferer this much weaker (capture).
    capture_db: float = 6.0
    # A different-SF interferer must be this much stronger to destroy a frame.
    cross_sf_rejection_db: float = 16.0
    # Random loss on every link, percent.
    loss_pct: float = 0.0
    # How long after a frame ends it is decided, to catch overlapping frames
    # whose TX message is still on its way.
    resolve_delay_us: int = 1500
    # Energy weaker than the noise floor by more than this is not reported.
    energy_cutoff_db: float = 20.0
    extra: dict = field(default_factory=dict)

    def noise_floor_dbm(self) -> float:
        return -174.0 + 10.0 * math.log10(self.bw_khz * 1000.0) + self.noise_figure_db

    def path_loss_db(self, distance_m: float) -> float:
        return self.pl0_db + 10.0 * self.exponent * math.log10(max(distance_m, 1.0))

    def to_dict(self) -> dict:
        return {
            "pl0_db": self.pl0_db,
            "exponent": self.exponent,
            "noise_figure_db": self.noise_figure_db,
            "bw_khz": self.bw_khz,
            "capture_db": self.capture_db,
            "cross_sf_rejection_db": self.cross_sf_rejection_db,
            "loss_pct": self.loss_pct,
            "resolve_delay_us": self.resolve_delay_us,
            "energy_cutoff_db": self.energy_cutoff_db,
            "noise_floor_dbm": round(self.noise_floor_dbm(), 1),
        }

    def update(self, d: dict) -> None:
        for k in ("pl0_db", "exponent", "noise_figure_db", "capture_db",
                  "cross_sf_rejection_db", "loss_pct", "energy_cutoff_db"):
            if k in d and d[k] is not None:
                setattr(self, k, float(d[k]))
        if d.get("resolve_delay_us") is not None:
            self.resolve_delay_us = int(d["resolve_delay_us"])
