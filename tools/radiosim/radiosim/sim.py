"""The simulator: UDP endpoint for the nodes, the medium, devices, processes."""

from __future__ import annotations

import asyncio
import json
import logging
import math
import secrets
import string
import time
from pathlib import Path
from typing import Any

from . import devices as devmod
from . import proto, scenario, stats
from .devices import Device, Radio
from .medium import Medium, Node
from .procs import ProcessManager
from .recorder import Recorder
from .rf import RfParams

log = logging.getLogger(__name__)

STATS_PERIOD_S = 0.5
LIVENESS_PERIOD_S = 0.5


def now_us() -> int:
    # asyncio's loop.time() is time.monotonic(): the same CLOCK_MONOTONIC.
    return time.monotonic_ns() // 1000


class Bus:
    """Fan-out of JSON events to every WebSocket client."""

    def __init__(self, maxsize: int = 20000):
        self.maxsize = maxsize
        self.queues: set[asyncio.Queue] = set()
        self.taps: list = []

    def subscribe(self) -> asyncio.Queue:
        q: asyncio.Queue = asyncio.Queue(self.maxsize)
        self.queues.add(q)
        return q

    def unsubscribe(self, q: asyncio.Queue) -> None:
        self.queues.discard(q)

    def emit(self, ev: dict) -> None:
        for tap in self.taps:
            tap(ev)
        if not self.queues:
            return
        text = json.dumps(ev, separators=(",", ":"))
        for q in list(self.queues):
            try:
                q.put_nowait(text)
            except asyncio.QueueFull:
                # Too slow: drop it; the page reloads its state on reconnect.
                self.queues.discard(q)


class _Udp(asyncio.DatagramProtocol):
    def __init__(self, sim: "Sim"):
        self.sim = sim

    def datagram_received(self, data: bytes, addr: tuple) -> None:
        self.sim.medium.datagram(data, addr)


class Sim:
    def __init__(self, workdir: Path, exes: dict[str, Path | None], udp_port: int = proto.DEFAULT_PORT,
                 record: bool = True):
        self.workdir = workdir
        self.udp_port = udp_port
        self.bus = Bus()
        self.transport: asyncio.DatagramTransport | None = None
        self.recorder = Recorder(workdir) if record else None
        if self.recorder:
            self.bus.taps.append(self.recorder)
        self.medium = Medium(RfParams(), now_us, self._call_at, self._send, self.bus.emit)
        self.medium.on_display = self._display
        self.medium.on_new_node = self._place
        self.procs = ProcessManager(workdir, exes, udp_port, on_exit=self._exited)
        self.screens: dict[int, set[asyncio.Queue]] = {}
        self._tasks: list[asyncio.Task] = []
        self._stopped = False
        self.last_stats: dict = {}

    # ------------------------------------------------------------------ plumbing

    def _call_at(self, t_us: int, fn) -> Any:
        loop = asyncio.get_running_loop()
        return loop.call_at(t_us / 1e6, fn)

    def _send(self, addr: tuple, data: bytes) -> None:
        if self.transport is not None:
            self.transport.sendto(data, addr)

    async def start(self) -> None:
        loop = asyncio.get_running_loop()
        self.transport, _ = await loop.create_datagram_endpoint(
            lambda: _Udp(self), local_addr=("127.0.0.1", self.udp_port)
        )
        self._tasks.append(loop.create_task(self._periodic()))
        log.info("listening for nodes on udp 127.0.0.1:%d", self.udp_port)

    async def stop(self) -> None:
        if self._stopped:
            return
        self._stopped = True
        log.info("stopping")
        for t in self._tasks:
            t.cancel()
        for d in self.devices():
            d.stop()
        await self.procs.kill_all()
        if self.transport:
            self.transport.close()
        if self.recorder:
            self.recorder.close()

    async def _periodic(self) -> None:
        next_stats = 0.0
        while True:
            await asyncio.sleep(LIVENESS_PERIOD_S)
            self.medium.check_liveness()
            if time.monotonic() >= next_stats:
                next_stats = time.monotonic() + STATS_PERIOD_S
                self.last_stats = stats.compute(self.medium)
                self.bus.emit(self.last_stats)

    def snapshot(self) -> dict:
        snap = self.medium.snapshot()
        snap["can_spawn"] = {r: self.procs.can_spawn(r) for r in ("ctrl", "sat")}
        snap["udp_port"] = self.udp_port
        snap["workdir"] = str(self.workdir)
        return snap

    # --------------------------------------------------------------------- nodes

    def _place(self, node: Node) -> None:
        """A default spot for a node that has none: controllers near the
        origin, satellites on an arc 150 m out."""
        if node.x or node.y:
            return
        nodes = self.medium.nodes()
        if node.role == "ctrl" or node.name.startswith("ctrl"):
            k = sum(1 for n in nodes if n.role == "ctrl")
            node.x, node.y = -150.0, 40.0 * k
        else:
            k = sum(1 for n in nodes if n.role != "ctrl")
            a = math.radians(-60 + 25 * k)
            node.x, node.y = -150 + 150 * math.cos(a) + 50, 150 * math.sin(a)

    def _free_name(self, role: str) -> str:
        names = {n.name for n in self.medium.nodes()}
        if role == "ctrl":
            i = 1
            while f"ctrl{i}" in names:
                i += 1
            return f"ctrl{i}"
        for c in string.ascii_uppercase:
            if f"sat{c}" not in names:
                return f"sat{c}"
        i = 27
        while f"sat{i}" in names:
            i += 1
        return f"sat{i}"

    def node(self, eid: int) -> Node:
        ent = self.medium.entities.get(eid)
        if not isinstance(ent, Node):
            raise KeyError(eid)
        return ent

    def add_node(self, role: str, name: str | None = None, x: float | None = None,
                 y: float | None = None, device_id: int | None = None) -> Node:
        name = name or self._free_name(role)
        if self.medium.node_by_name(name):
            raise ValueError(f"a node named {name!r} exists")
        node = Node(id=0, name=name, role=role, managed=True,
                    device_id=device_id if device_id is not None else secrets.randbits(31) | 1)
        if x is not None and y is not None:
            node.x, node.y = float(x), float(y)
        else:
            self._place(node)
        self.medium.add(node)
        return node

    async def start_node(self, node: Node) -> None:
        await self.procs.spawn(node.name, node.role, node.device_id or 1)
        node.running = True
        node.managed = True
        self.medium.emit_entity(node)

    async def kill_node(self, node: Node) -> None:
        await self.procs.kill(node.name)

    async def restart_node(self, node: Node) -> None:
        await self.procs.kill(node.name)
        await self.start_node(node)

    async def remove_node(self, node: Node) -> None:
        await self.procs.kill(node.name)
        self.screens.pop(node.id, None)
        self.medium.remove(node.id)

    def _exited(self, name: str, code: int) -> None:
        node = self.medium.node_by_name(name)
        if node is not None:
            node.running = False
            node.connected = False
            self.medium.emit_entity(node)

    # ------------------------------------------------------------------ displays

    @staticmethod
    def screen_bytes(d: proto.Display) -> bytes:
        import struct

        return struct.pack("<HHBBH", d.width, d.height, d.format, 0, d.stride) + d.data

    def _display(self, node: Node) -> None:
        subs = self.screens.get(node.id)
        if not subs or node.display is None:
            return
        data = self.screen_bytes(node.display)
        for q in list(subs):
            if q.full():
                try:
                    q.get_nowait()  # only the newest frame matters
                except asyncio.QueueEmpty:
                    pass
            q.put_nowait(data)

    def watch_screen(self, eid: int) -> asyncio.Queue:
        q: asyncio.Queue = asyncio.Queue(2)
        self.screens.setdefault(eid, set()).add(q)
        node = self.node(eid)
        if node.display is not None:
            q.put_nowait(self.screen_bytes(node.display))
        return q

    def unwatch_screen(self, eid: int, q: asyncio.Queue) -> None:
        self.screens.get(eid, set()).discard(q)

    # ------------------------------------------------------------------- devices

    def devices(self) -> list[Device]:
        return [e for e in self.medium.entities.values() if isinstance(e, Device)]

    def device(self, eid: int) -> Device:
        ent = self.medium.entities.get(eid)
        if not isinstance(ent, Device):
            raise KeyError(eid)
        return ent

    def add_device(self, device_type: str, d: dict) -> Device:
        dev = devmod.create(device_type, self.medium.new_id(), d)
        dev.attach(self.medium)
        self.medium.add(dev)
        if dev.enabled:
            dev.start()
        return dev

    def update_device(self, dev: Device, d: dict) -> None:
        dev.update(d)
        if d.get("enabled") is not None:
            dev.enabled = bool(d["enabled"])
        dev.restart()
        self.medium.emit_entity(dev)

    def remove_device(self, dev: Device) -> None:
        dev.stop()
        self.medium.remove(dev.id)

    def inject(self, src: int, data: bytes, sf: int | None = None, origin: str = "inject") -> int:
        dev = self.device(src)
        if not isinstance(dev, Radio):
            raise ValueError("only a radio device can send frames")
        return dev.send(data, sf, origin).id

    def replay(self, frame_id: int, src: int) -> int:
        tx = self.medium.history.get(frame_id)
        if tx is None or tx.data is None:
            raise KeyError(frame_id)
        return self.inject(src, tx.data, tx.sf, origin=f"replay of #{frame_id}")

    # ------------------------------------------------------------------ scenario

    def dump_scenario(self) -> dict:
        return scenario.dump(self)

    async def load_scenario(self, data: dict, autostart: bool = False) -> None:
        for n in self.medium.nodes():
            await self.remove_node(n)
        for d in self.devices():
            self.remove_device(d)
        self.medium.links.clear()
        self.medium.rf.update(data.get("rf", {}))
        self.bus.emit({"type": "rf", "rf": self.medium.rf.to_dict()})

        ids: dict[str, int] = {}
        for nd in data.get("nodes", []):
            node = self.add_node(nd.get("role", "sat"), nd["name"], nd.get("x", 0), nd.get("y", 0),
                                 nd.get("device_id"))
            node.managed = nd.get("managed", True)
            node.tx_power_override = nd.get("tx_power_override")
            node.isolated = bool(nd.get("isolated", False))
            self.medium.emit_entity(node)
            ids[node.name] = node.id
        for dd in data.get("devices", []):
            dev = self.add_device(dd["device_type"], dd)
            ids[dev.name] = dev.id
        for lk in data.get("links", []):
            if lk["a"] in ids and lk["b"] in ids:
                self.medium.set_link(ids[lk["a"]], ids[lk["b"]], lk.get("extra_db", 0),
                                     lk.get("cut", False), lk.get("loss_pct", 0))
        self.medium.emit_links()
        if autostart:
            for n in self.medium.nodes():
                if n.managed and self.procs.can_spawn(n.role):
                    await self.start_node(n)
