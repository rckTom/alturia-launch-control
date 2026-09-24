import random

import pytest

from radiosim import proto
from radiosim.medium import Medium, Node
from radiosim.rf import RfParams


class FakeAir:
    """A medium on a hand-wound clock, with the datagrams it sends kept."""

    def __init__(self, **rf):
        self.t = 1_000_000_000
        self.timers: list = []
        self.sent: list[tuple[tuple, bytes]] = []
        self.events: list[dict] = []
        self.m = Medium(RfParams(**rf), self.clock, self.call_at, self.send, self.events.append,
                        rng=random.Random(1))

    def clock(self) -> int:
        return self.t

    def call_at(self, t: int, fn) -> None:
        self.timers.append((t, fn))

    def send(self, addr, data) -> None:
        self.sent.append((addr, data))

    def advance(self, us: int) -> None:
        self.t += us
        due = sorted([x for x in self.timers if x[0] <= self.t], key=lambda x: x[0])
        self.timers = [x for x in self.timers if x[0] > self.t]
        for _, fn in due:
            fn()

    def node(self, name: str, x: float, y: float, sf: int = 7, role: int = proto.ROLE_SAT) -> Node:
        addr = ("127.0.0.1", 40000 + len(self.m.entities))
        self.m.datagram(proto.hello(role, sf, name), addr)
        n = self.m.node_by_name(name)
        n.x, n.y = x, y
        return n

    def tx(self, node: Node, data: bytes, airtime_us: int = 20_000, sf: int = 7, power: int = 7):
        self.m.datagram(proto.tx(self.t, airtime_us, sf, power, data), node.addr)
        return self.m.active[-1]

    def received(self, node: Node) -> list[tuple]:
        out = []
        for addr, data in self.sent:
            if addr == node.addr:
                mtype, fields = proto.parse_down(data)
                if mtype == proto.RX:
                    out.append(fields)
        return out

    def outcome(self, tx, node: Node) -> str:
        return next(r["outcome"] for r in tx.results if r["node"] == node.id)


@pytest.fixture
def air():
    return FakeAir()
