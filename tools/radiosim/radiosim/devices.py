"""Virtual transmitters on the map: jammers, foreign traffic, attacker radios.

Each device is an Entity the medium treats like any other transmitter. The
ones that transmit on their own run an asyncio task while enabled.
"""

from __future__ import annotations

import asyncio
import os
from dataclasses import dataclass

from .medium import OPEN, Entity, Medium, Transmission
from .rf import airtime_us


@dataclass
class Device(Entity):
    power_dbm: float = 7.0
    sf: int = 7
    enabled: bool = False

    kind = "device"
    device_type = "device"

    def __post_init__(self):
        self._task: asyncio.Task | None = None
        self._medium: Medium | None = None

    def to_dict(self) -> dict:
        d = super().to_dict()
        d.update(device_type=self.device_type, power_dbm=self.power_dbm, sf=self.sf,
                 enabled=self.enabled)
        return d

    def settings(self) -> dict:
        """What a scenario file keeps."""
        d = self.to_dict()
        for k in ("id", "kind", "tx_power_override"):
            d.pop(k, None)
        return d

    def update(self, d: dict) -> None:
        for k in ("power_dbm",):
            if d.get(k) is not None:
                setattr(self, k, float(d[k]))
        if d.get("sf") is not None:
            self.sf = int(d["sf"])
        if d.get("name"):
            self.name = str(d["name"])

    def attach(self, medium: Medium) -> None:
        self._medium = medium

    def start(self) -> None:
        pass

    def stop(self) -> None:
        if self._task:
            self._task.cancel()
            self._task = None

    def restart(self) -> None:
        self.stop()
        if self.enabled:
            self.start()


@dataclass
class Jammer(Device):
    """RF energy: continuous, or on for duty * period out of every period."""

    mode: str = "continuous"  # or "pulsed"
    period_ms: float = 100.0
    duty: float = 0.5
    # Wideband energy raises the noise floor; otherwise it is a LoRa-like
    # signal on @p sf that only collides as another frame would.
    wideband: bool = True

    device_type = "jammer"

    def __post_init__(self):
        super().__post_init__()
        self._tx: Transmission | None = None

    def to_dict(self) -> dict:
        d = super().to_dict()
        d.update(mode=self.mode, period_ms=self.period_ms, duty=self.duty, wideband=self.wideband)
        return d

    def update(self, d: dict) -> None:
        super().update(d)
        if d.get("mode") in ("continuous", "pulsed"):
            self.mode = d["mode"]
        if d.get("period_ms") is not None:
            self.period_ms = max(1.0, float(d["period_ms"]))
        if d.get("duty") is not None:
            self.duty = min(1.0, max(0.01, float(d["duty"])))
        if d.get("wideband") is not None:
            self.wideband = bool(d["wideband"])

    def _energy(self, t_end: int) -> Transmission:
        m = self._medium
        return m.start(Transmission(id=0, src=self.id, t_start=m.clock(), t_end=t_end, sf=self.sf,
                                    power_dbm=self.power_dbm, data=None, wideband=self.wideband))

    def start(self) -> None:
        if self.mode == "continuous":
            self._tx = self._energy(OPEN)
        else:
            self._task = asyncio.get_running_loop().create_task(self._pulses())

    def stop(self) -> None:
        super().stop()
        if self._tx is not None:
            self._medium.stop(self._tx)
            self._tx = None

    async def _pulses(self) -> None:
        while True:
            on_us = int(self.period_ms * self.duty * 1000)
            self._energy(self._medium.clock() + on_us)
            await asyncio.sleep(self.period_ms / 1000.0)


@dataclass
class TrafficGen(Device):
    """Some other LoRa network on the channel: random frames, Poisson in time."""

    rate_hz: float = 2.0
    len_min: int = 10
    len_max: int = 50

    device_type = "traffic"

    def to_dict(self) -> dict:
        d = super().to_dict()
        d.update(rate_hz=self.rate_hz, len_min=self.len_min, len_max=self.len_max)
        return d

    def update(self, d: dict) -> None:
        super().update(d)
        if d.get("rate_hz") is not None:
            self.rate_hz = max(0.01, float(d["rate_hz"]))
        if d.get("len_min") is not None:
            self.len_min = max(1, min(255, int(d["len_min"])))
        if d.get("len_max") is not None:
            self.len_max = max(self.len_min, min(255, int(d["len_max"])))

    def start(self) -> None:
        self._task = asyncio.get_running_loop().create_task(self._run())

    async def _run(self) -> None:
        m = self._medium
        busy_until = 0
        while True:
            await asyncio.sleep(m.rng.expovariate(self.rate_hz))
            now = m.clock()
            if now < busy_until:
                continue  # one radio: it is still sending the last one
            n = m.rng.randint(self.len_min, self.len_max)
            # A foreign version in the top bits, so no link parser takes it.
            data = bytes([0xC0 | (os.urandom(1)[0] & 0x3F)]) + os.urandom(n - 1)
            air = airtime_us(n, self.sf, m.rf.bw_khz)
            busy_until = now + air
            m.start(Transmission(id=0, src=self.id, t_start=now, t_end=now + air, sf=self.sf,
                                 power_dbm=self.power_dbm, data=data))


@dataclass
class Radio(Device):
    """An attacker's (or a test's) radio: sends frames only when told to."""

    device_type = "radio"

    def send(self, data: bytes, sf: int | None = None, origin: str | None = None) -> Transmission:
        m = self._medium
        sf = sf or self.sf
        now = m.clock()
        return m.start(Transmission(id=0, src=self.id, t_start=now,
                                    t_end=now + airtime_us(len(data), sf, m.rf.bw_khz), sf=sf,
                                    power_dbm=self.power_dbm, data=data, origin=origin))


TYPES = {"jammer": Jammer, "traffic": TrafficGen, "radio": Radio}


def create(device_type: str, eid: int, d: dict) -> Device:
    cls = TYPES[device_type]
    dev = cls(id=eid, name=d.get("name") or f"{device_type}{eid}",
              x=float(d.get("x", 0.0)), y=float(d.get("y", 0.0)))
    dev.update(d)
    dev.enabled = bool(d.get("enabled", False))
    return dev
