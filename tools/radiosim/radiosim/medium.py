"""The simulated air: who is on the map, what is on air, and who hears it.

Everything that transmits is an Entity with a position: firmware nodes
(connected over UDP) and virtual devices (jammers, traffic generators,
attacker radios, see devices.py). A Transmission is a LoRa frame, or plain
RF energy for a jammer. When a frame ends the medium decides, per node,
whether it was received, and why not otherwise, and sends it to those that
did. Nodes learn of all energy on air as it starts (ENERGY messages), which
their listen-before-talk samples.

Times are host CLOCK_MONOTONIC microseconds, the clock the nodes use too.
The medium takes the clock, a scheduler and the UDP send as callables, so
tests can drive it with a fake clock.
"""

from __future__ import annotations

import math
import random
from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Any, Callable

from . import frames, proto
from .rf import DEMOD_SNR_DB, RfParams, dbm_to_mw, mw_to_dbm

LOST_AFTER_US = 3_500_000
# Transmissions this old no longer overlap anything that is being decided.
ACTIVE_KEEP_US = 3_000_000
HISTORY_MAX = 50_000
OPEN = proto.T_OPEN

OUTCOMES = (
    "ok",
    "offline",
    "cut",
    "half_duplex",
    "wrong_sf",
    "below_sens",
    "jammed",
    "collision",
    "random_loss",
)


@dataclass
class Entity:
    id: int
    name: str
    x: float = 0.0
    y: float = 0.0
    # Overrides the power the firmware asked for, at the antenna.
    tx_power_override: int | None = None

    kind = "entity"

    def to_dict(self) -> dict:
        return {
            "id": self.id,
            "kind": self.kind,
            "name": self.name,
            "x": self.x,
            "y": self.y,
            "tx_power_override": self.tx_power_override,
        }


@dataclass
class Node(Entity):
    """A firmware instance: controller, satellite, or anything with radio_sim.c."""

    role: str = "other"
    uid: int = 0
    pid: int = 0
    sf: int = 7
    leds: int = 0
    addr: tuple | None = None
    last_seen: int = 0
    connected: bool = False
    # Radio off: hears nothing, and nothing it sends goes out.
    isolated: bool = False
    # Started by the simulator's process manager.
    managed: bool = False
    running: bool = False
    device_id: int | None = None
    display: proto.Display | None = None
    tx_count: int = 0

    kind = "node"

    def to_dict(self) -> dict:
        d = super().to_dict()
        d.update(
            role=self.role,
            uid=f"{self.uid:016x}",
            pid=self.pid,
            sf=self.sf,
            leds=self.leds,
            connected=self.connected,
            isolated=self.isolated,
            managed=self.managed,
            running=self.running,
            device_id=self.device_id,
            has_display=self.display is not None,
        )
        return d


@dataclass
class Transmission:
    id: int
    src: int
    t_start: int
    t_end: int  # OPEN while a continuous jammer is on
    sf: int
    power_dbm: float
    data: bytes | None  # None: energy only (jammer)
    wideband: bool = False
    info: dict = field(default_factory=dict)
    results: list[dict] | None = None
    # A replay or an injection names the frame it copies.
    origin: str | None = None

    def overlaps(self, t0: int, t1: int) -> bool:
        return self.t_start < t1 and t0 < self.t_end


@dataclass
class Link:
    extra_db: float = 0.0
    cut: bool = False
    loss_pct: float = 0.0

    def is_default(self) -> bool:
        return self.extra_db == 0 and not self.cut and self.loss_pct == 0


class Medium:
    def __init__(
        self,
        params: RfParams,
        clock: Callable[[], int],
        call_at: Callable[[int, Callable[[], None]], Any],
        send: Callable[[tuple, bytes], None],
        emit: Callable[[dict], None] = lambda ev: None,
        rng: random.Random | None = None,
    ):
        self.rf = params
        self.clock = clock
        self.call_at = call_at
        self.send = send
        self.emit = emit
        self.rng = rng or random.Random()
        self.epoch = clock()
        self.entities: dict[int, Entity] = {}
        self.links: dict[tuple[int, int], Link] = {}
        self.active: list[Transmission] = []
        self.history: OrderedDict[int, Transmission] = OrderedDict()
        self._next_entity = 1
        self._next_tx = 1
        # Called with the node and display whenever a node's screen changes.
        self.on_display: Callable[[Node], None] = lambda node: None
        # Called when an unknown node says hello, before it is placed.
        self.on_new_node: Callable[[Node], None] = lambda node: None

    # ------------------------------------------------------------------ entities

    def new_id(self) -> int:
        eid = self._next_entity
        self._next_entity += 1
        return eid

    def add(self, entity: Entity) -> Entity:
        if not entity.id:
            entity.id = self.new_id()
        self._next_entity = max(self._next_entity, entity.id + 1)
        self.entities[entity.id] = entity
        self.emit_entity(entity)
        return entity

    def remove(self, eid: int) -> None:
        ent = self.entities.pop(eid, None)
        if ent is None:
            return
        for key in [k for k in self.links if eid in k]:
            del self.links[key]
        self.emit({"type": "entity_removed", "id": eid})

    def nodes(self) -> list[Node]:
        return [e for e in self.entities.values() if isinstance(e, Node)]

    def node_by_name(self, name: str) -> Node | None:
        for n in self.nodes():
            if n.name == name:
                return n
        return None

    def node_by_addr(self, addr: tuple) -> Node | None:
        for n in self.nodes():
            if n.addr == addr and n.connected:
                return n
        return None

    def emit_entity(self, entity: Entity) -> None:
        self.emit({"type": "entity", "entity": entity.to_dict()})

    def move(self, eid: int, x: float, y: float) -> None:
        ent = self.entities[eid]
        ent.x, ent.y = float(x), float(y)
        self.emit_entity(ent)
        # Continuous energy changes level at every node with the move.
        for tx in self.active:
            if tx.t_end == OPEN and (tx.src == eid or isinstance(ent, Node)):
                self._announce_energy(tx)

    def link(self, a: int, b: int) -> Link:
        return self.links.get((min(a, b), max(a, b)), Link())

    def set_link(self, a: int, b: int, extra_db: float, cut: bool, loss_pct: float) -> None:
        key = (min(a, b), max(a, b))
        lk = Link(float(extra_db), bool(cut), float(loss_pct))
        if lk.is_default():
            self.links.pop(key, None)
        else:
            self.links[key] = lk
        self.emit_links()

    def emit_links(self) -> None:
        self.emit({"type": "links", "links": self.links_list()})

    def links_list(self) -> list[dict]:
        return [
            {"a": a, "b": b, "extra_db": lk.extra_db, "cut": lk.cut, "loss_pct": lk.loss_pct}
            for (a, b), lk in self.links.items()
        ]

    # --------------------------------------------------------------- propagation

    def rssi(self, src: Entity, dst: Entity, power_dbm: float) -> float:
        d = math.hypot(src.x - dst.x, src.y - dst.y)
        return power_dbm - self.rf.path_loss_db(d) - self.link(src.id, dst.id).extra_db

    def rel(self, t: int) -> int | None:
        """Host time as microseconds since the simulator started, for the UI."""
        return None if t == OPEN else t - self.epoch

    # ------------------------------------------------------------------ messages

    def datagram(self, buf: bytes, addr: tuple) -> None:
        try:
            msg = proto.parse(buf)
        except proto.ProtoError:
            return
        now = self.clock()

        if isinstance(msg, proto.Hello):
            self._hello(msg, addr, now)
            return

        node = self.node_by_addr(addr)
        if node is None:
            return  # not registered yet; its next HELLO will
        node.last_seen = now

        if isinstance(msg, proto.Tx):
            node.tx_count += 1
            if node.isolated:
                return
            power = node.tx_power_override if node.tx_power_override is not None else msg.power_dbm
            self.start(
                Transmission(
                    id=0,
                    src=node.id,
                    t_start=msg.t_start,
                    t_end=msg.t_start + msg.airtime_us,
                    sf=msg.sf,
                    power_dbm=power,
                    data=msg.data,
                )
            )
        elif isinstance(msg, proto.Profile):
            node.sf = msg.sf
            self.emit_entity(node)
        elif isinstance(msg, proto.Io):
            node.leds = msg.leds
            self.emit({"type": "leds", "id": node.id, "leds": node.leds})
        elif isinstance(msg, proto.Display):
            node.display = msg
            self.on_display(node)

    def _hello(self, msg: proto.Hello, addr: tuple, now: int) -> None:
        node = self.node_by_name(msg.name)
        if node is None:
            node = Node(id=0, name=msg.name, role=proto.ROLE_NAMES.get(msg.role, "other"))
            self.on_new_node(node)
            self.add(node)
        changed = (
            not node.connected
            or node.addr != addr
            or node.sf != msg.sf
            or node.leds != msg.leds
            or node.pid != msg.pid
        )
        was_connected = node.connected and node.addr == addr
        node.addr = addr
        node.role = proto.ROLE_NAMES.get(msg.role, "other")
        node.uid = msg.uid
        node.pid = msg.pid
        node.sf = msg.sf
        node.leds = msg.leds
        node.last_seen = now
        node.connected = True
        self.send(addr, proto.welcome(node.id, round(self.rf.noise_floor_dbm())))
        if not was_connected:
            # Tell it about energy that is already on air.
            for tx in self.active:
                if tx.src != node.id and (tx.t_end == OPEN or tx.t_end > now):
                    self._energy_to(tx, node)
        if changed:
            self.emit_entity(node)

    def check_liveness(self) -> None:
        now = self.clock()
        for n in self.nodes():
            if n.connected and now - n.last_seen > LOST_AFTER_US:
                n.connected = False
                self.emit_entity(n)

    def send_to(self, node: Node, data: bytes) -> bool:
        if node.addr is None or not node.connected:
            return False
        self.send(node.addr, data)
        return True

    # ------------------------------------------------------------- transmissions

    def start(self, tx: Transmission) -> Transmission:
        now = self.clock()
        tx.id = self._next_tx
        self._next_tx += 1
        if tx.data is not None and not tx.info:
            tx.info = frames.decode(tx.data)

        self.active = [
            t for t in self.active if t.t_end == OPEN or t.t_end > now - ACTIVE_KEEP_US
        ]
        self.active.append(tx)
        self.history[tx.id] = tx
        while len(self.history) > HISTORY_MAX:
            self.history.popitem(last=False)

        self._announce_energy(tx)
        self.emit({"type": "frame", "frame": self.frame_dict(tx)})

        if tx.data is not None:
            self.call_at(tx.t_end + self.rf.resolve_delay_us, lambda: self.resolve(tx))
        return tx

    def stop(self, tx: Transmission, t_end: int | None = None) -> None:
        """End a continuous transmission."""
        if tx.t_end != OPEN:
            return
        tx.t_end = t_end if t_end is not None else self.clock()
        for n in self.nodes():
            if n.id != tx.src:
                self.send_to(n, proto.energy_end(tx.id, tx.t_end))
        self.emit({"type": "frame_end", "id": tx.id, "t_end": self.rel(tx.t_end)})

    def _announce_energy(self, tx: Transmission) -> None:
        for n in self.nodes():
            if n.id != tx.src and n.connected:
                self._energy_to(tx, n)

    def _energy_to(self, tx: Transmission, node: Node) -> None:
        src = self.entities.get(tx.src)
        if src is None or node.isolated:
            return
        if self.link(tx.src, node.id).cut:
            return
        level = self.rssi(src, node, tx.power_dbm)
        if level < self.rf.noise_floor_dbm() - self.rf.energy_cutoff_db:
            return
        self.send_to(node, proto.energy(tx.id, tx.t_start, tx.t_end, round(level)))

    def overlapping(self, tx: Transmission) -> list[Transmission]:
        return [t for t in self.active if t is not tx and t.overlaps(tx.t_start, tx.t_end)]

    def resolve(self, tx: Transmission) -> list[dict]:
        """Decide who received frame @p tx, deliver it to them, and report."""
        src = self.entities.get(tx.src)
        others = self.overlapping(tx)
        results = []
        for node in self.nodes():
            if node.id == tx.src:
                continue
            res = self._receive(tx, src, node, others)
            results.append(res)
            if res["outcome"] == "ok":
                self.send_to(
                    node,
                    proto.rx(tx.t_end, round(res["rssi"]), max(-20, min(15, round(res["snr"]))),
                             tx.sf, tx.data),
                )
        tx.results = results
        self.emit({"type": "frame_result", "id": tx.id, "results": results})
        return results

    def _receive(self, tx: Transmission, src: Entity | None, node: Node,
                 others: list[Transmission]) -> dict:
        res: dict = {"node": node.id, "outcome": "ok", "rssi": None, "snr": None, "sinr": None}
        if not node.connected or node.isolated or src is None:
            res["outcome"] = "offline"
            return res
        lk = self.link(tx.src, node.id)
        if lk.cut:
            res["outcome"] = "cut"
            return res

        rssi = self.rssi(src, node, tx.power_dbm)
        noise_mw = dbm_to_mw(self.rf.noise_floor_dbm())
        snr = rssi - mw_to_dbm(noise_mw)
        res["rssi"] = round(rssi, 1)
        res["snr"] = round(snr, 1)

        # A half-duplex radio hears nothing while it transmits.
        if any(t.src == node.id for t in others):
            res["outcome"] = "half_duplex"
            return res
        if node.sf != tx.sf:
            res["outcome"] = "wrong_sf"
            return res

        demod = DEMOD_SNR_DB.get(tx.sf, -7.5)
        jam_mw = co_mw = 0.0
        cross_worst_db = -300.0
        for t in others:
            other_src = self.entities.get(t.src)
            if other_src is None or self.link(t.src, node.id).cut:
                continue
            level = self.rssi(other_src, node, t.power_dbm)
            if t.wideband or t.data is None:
                jam_mw += dbm_to_mw(level)
            elif t.sf == tx.sf:
                co_mw += dbm_to_mw(level)
            else:
                cross_worst_db = max(cross_worst_db, level)

        cross_mw = dbm_to_mw(cross_worst_db - self.rf.cross_sf_rejection_db) if cross_worst_db > -300 else 0.0
        res["sinr"] = round(rssi - mw_to_dbm(noise_mw + jam_mw + co_mw + cross_mw), 1)

        if snr < demod:
            res["outcome"] = "below_sens"
        elif rssi - mw_to_dbm(noise_mw + jam_mw) < demod:
            res["outcome"] = "jammed"
        elif co_mw > 0 and rssi - mw_to_dbm(co_mw) < self.rf.capture_db:
            res["outcome"] = "collision"
        elif cross_worst_db > -300 and rssi - cross_worst_db < -self.rf.cross_sf_rejection_db:
            res["outcome"] = "collision"
        else:
            loss = 1.0 - (1.0 - self.rf.loss_pct / 100.0) * (1.0 - lk.loss_pct / 100.0)
            if loss > 0 and self.rng.random() < loss:
                res["outcome"] = "random_loss"
        return res

    def frame_dict(self, tx: Transmission) -> dict:
        return {
            "id": tx.id,
            "src": tx.src,
            "t_start": self.rel(tx.t_start),
            "t_end": self.rel(tx.t_end),
            "sf": tx.sf,
            "power": tx.power_dbm,
            "len": len(tx.data) if tx.data is not None else None,
            "hex": tx.data.hex() if tx.data is not None else None,
            "info": tx.info,
            "wideband": tx.wideband,
            "energy": tx.data is None,
            "origin": tx.origin,
            "results": tx.results,
        }

    # ------------------------------------------------------------------ controls

    def button(self, node: Node, index: int, pressed: bool) -> bool:
        return self.send_to(node, proto.button(index, pressed))

    def input(self, node: Node, itype: int, code: int, value: int) -> bool:
        return self.send_to(node, proto.input_event(itype, code, value))

    def snapshot(self) -> dict:
        return {
            "type": "snapshot",
            "now": self.rel(self.clock()),
            "rf": self.rf.to_dict(),
            "entities": [e.to_dict() for e in self.entities.values()],
            "links": self.links_list(),
            "frames": [self.frame_dict(t) for t in list(self.history.values())[-2000:]],
        }
