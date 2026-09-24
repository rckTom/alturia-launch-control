"""Save and load the map: nodes, devices, links and RF parameters, as JSON."""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .sim import Sim


def dump(sim: "Sim") -> dict:
    m = sim.medium
    ids = {}
    nodes = []
    for n in m.nodes():
        ids[n.id] = n.name
        nodes.append({
            "name": n.name,
            "role": n.role,
            "x": n.x,
            "y": n.y,
            "device_id": n.device_id,
            "managed": n.managed,
            "tx_power_override": n.tx_power_override,
            "isolated": n.isolated,
        })
    devices = []
    for d in sim.devices():
        ids[d.id] = d.name
        devices.append(d.settings())
    links = [
        {"a": ids[a], "b": ids[b], "extra_db": lk.extra_db, "cut": lk.cut, "loss_pct": lk.loss_pct}
        for (a, b), lk in m.links.items()
        if a in ids and b in ids
    ]
    return {"version": 1, "rf": m.rf.to_dict(), "nodes": nodes, "devices": devices, "links": links}
