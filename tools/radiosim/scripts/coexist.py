"""Two networks on one channel: how long does the second one take to join?

Starts its own radiosim (separate ports and work directory, so a running
instance is not disturbed) and builds this layout, in metres:

    network 2: ctrl2 (-19, -568) polling satB (-345, -2016) and satC (-698, -566)
    network 1: ctrl1 (322, -743) with satA (1344, -109)

Each pair is paired side by side (pairing runs at 0 dBm) and then moved to
its place. Then satA is restarted again and again, and each time the script
measures how long until ctrl1 receives its JOIN_ACK. It also reports the
replies per poll of both networks, and, with --commands, how long ARM and
FIRE take to be acknowledged on network 1 (with --loss, under random loss
on the ctrl1-satA link, so that commands have to be repeated).

    uv run python scripts/coexist.py                        # the current builds
    uv run python scripts/coexist.py --ctrl-exe A --sat-exe B --runs 5

With --nets N (2..4) it builds N networks instead, each a controller
with --sats satellites (1 or 2), the controllers 300-500 m apart around
the field and each satellite 0.9-1.3 km out from its own controller, away
from the others (hidden from the other networks). It reports, per
network, the polls its satellites decoded and the replies its controller
decoded, and each controller's own view (`ctrl link`: utilization, other
networks, CSMA counters). --aligned restarts every controller at the
same instant first; --commands then runs ARM/FIRE on network 1 and
reports the other networks' delivery during it.

    uv run python scripts/coexist.py --nets 4 --sats 2 --aligned --commands
"""

from __future__ import annotations

import argparse
import asyncio
import math
import re
import statistics
from collections import Counter
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import httpx
import websockets

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT.parents[1] / "firmware"
ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")

LAYOUT = {
    "ctrl2": (-19, -568),
    "satB": (-345, -2016),
    "satC": (-698, -566),
    "ctrl1": (322, -743),
    "satA": (1344, -109),
}


class Sim:
    def __init__(self, http_port: int):
        self.port = http_port
        self.c = httpx.Client(base_url=f"http://127.0.0.1:{http_port}", timeout=30)
        self._ids: dict[str, int] = {}

    def state(self, frames_since: int | None = None) -> dict:
        # The simulator stands still while it serializes; ask for no more than needed.
        params = {} if frames_since is None else {"frames_since": frames_since}
        return self.c.get("/api/state", params=params).json()

    def ids(self) -> dict[str, int]:
        if not self._ids:
            self._ids = {e["name"]: e["id"] for e in self.state(1 << 62)["entities"]}
        return self._ids

    def add(self, role: str, name: str, x: float, y: float) -> int:
        r = self.c.post("/api/nodes", json={"role": role, "name": name, "x": x, "y": y})
        r.raise_for_status()
        self._ids = {}
        return r.json()["id"]

    def move(self, name: str, x: float, y: float) -> None:
        self.c.patch(f"/api/entities/{self.ids()[name]}", json={"x": x, "y": y})

    async def shell(self, name: str, cmds: list[str], wait: float = 0.8) -> str:
        eid = self.ids()[name]
        url = f"ws://127.0.0.1:{self.port}/ws/console/{eid}"
        async with websockets.connect(url, max_size=None) as ws:
            end = asyncio.get_running_loop().time() + 0.3
            while (left := end - asyncio.get_running_loop().time()) > 0:
                try:
                    await asyncio.wait_for(ws.recv(), left)
                except TimeoutError:
                    break
            buf = b""
            for cmd in cmds or [""]:
                if cmd:
                    await ws.send((cmd + "\r").encode())
                end = asyncio.get_running_loop().time() + wait
                while (left := end - asyncio.get_running_loop().time()) > 0:
                    try:
                        buf += await asyncio.wait_for(ws.recv(), left)
                    except TimeoutError:
                        break
            return ANSI.sub(b"", buf).decode(errors="replace")

    async def pair(self, ctrl: str, sat: str, slot: int) -> bool:
        """Pair sat into the controller's slot (0 = A), both driven from their shells."""
        await self.shell(ctrl, ["ctrl back", "ctrl back", "ctrl hold", "ctrl down",
                                "ctrl ok", "ctrl up 8"]
                         + ([f"ctrl down {slot}"] if slot else []) + ["ctrl ok"], 0.6)
        await self.shell(sat, ["sat pair"], 0.5)
        for _ in range(20):
            out = await self.shell(ctrl, ["ctrl ok"], 1.5)
            if "compare" in out:
                break
        await self.shell(ctrl, ["ctrl ok"], 0.5)
        out = await self.shell(sat, ["sat pair confirm"], 4.0)
        await self.shell(ctrl, ["ctrl back", "ctrl back"], 0.5)
        return "paired as slot" in out

    def frames_since(self, t_rel: int) -> list[dict]:
        return self.state(t_rel)["frames"]

    def now(self) -> int:
        return self.state(1 << 62)["now"]


def joined_at(sim: Sim, since: int, sat_id: int, ctrl_id: int) -> int | None:
    """When ctrl received a JOIN_ACK from sat after `since` (sim time, us)."""
    for f in sim.frames_since(since):
        if f["src"] == sat_id and f["info"].get("kind") == "JOIN_ACK" and f["results"]:
            if any(r["node"] == ctrl_id and r["outcome"] == "ok" for r in (f["results"] or [])):
                return f["t_end"]
    return None


async def measure(sim: Sim, args) -> None:
    ids = {}
    near = {"ctrl2": (0, 0), "satB": (5, 0), "satC": (0, 5), "ctrl1": (400, 400), "satA": (405, 400)}
    for name, role in (("ctrl2", "ctrl"), ("satB", "sat"), ("satC", "sat"),
                       ("ctrl1", "ctrl"), ("satA", "sat")):
        ids[name] = sim.add(role, name, *near[name])
    await asyncio.sleep(3)

    print("pairing network 2 ...", flush=True)
    ok = await sim.pair("ctrl2", "satB", 0) and await sim.pair("ctrl2", "satC", 1)
    for name in ("ctrl2", "satB", "satC"):
        sim.move(name, *LAYOUT[name])
    print("pairing network 1 ...", flush=True)
    ok = await sim.pair("ctrl1", "satA", 0) and ok
    for name in ("ctrl1", "satA"):
        sim.move(name, *LAYOUT[name])
    if not ok:
        print("pairing failed; the numbers below mean little", flush=True)

    await asyncio.sleep(10)
    times = []
    for run in range(args.runs):
        start = sim.now()
        sim.c.post(f"/api/nodes/{ids['satA']}/restart")
        t0 = time.monotonic()
        at = None
        while time.monotonic() - t0 < args.timeout:
            await asyncio.sleep(1)
            at = joined_at(sim, start, ids["satA"], ids["ctrl1"])
            if at is not None:
                break
        took = None if at is None else (at - start) / 1e6
        times.append(took)
        print(f"  run {run + 1}: satA joined ctrl1 after "
              + (f"{took:.1f} s" if took is not None else f"> {args.timeout} s (not joined)"),
              flush=True)
        await asyncio.sleep(5)

    await asyncio.sleep(12)
    polls = {p["net_tag"]: p for p in sim.state()["stats"].get("polls", [])}
    ctrl_tag = {}
    for f in sim.state()["frames"]:
        if f["info"].get("kind") == "POLL":
            ctrl_tag[f["src"]] = f["info"]["net_tag"]
    print("\nreplies per poll, last 10 s:")
    for name in ("ctrl2", "ctrl1"):
        p = polls.get(ctrl_tag.get(ids[name], ""), {})
        print(f"  {name}: period {p.get('period_ms')} ms, replies {p.get('replies', {})}")

    done = [t for t in times if t is not None]
    print(f"\njoined {len(done)} of {len(times)}"
          + (f", median {statistics.median(done):.1f} s, max {max(done):.1f} s" if done else ""))

    if args.commands:
        i = sim.ids()
        if args.loss:
            sim.c.put("/api/links", json={"a": i["ctrl1"], "b": i["satA"], "extra_db": 0,
                                          "cut": False, "loss_pct": args.loss})
        await commands(sim, args.loss, args.fire_runs)


def net_names(k: int, sats: int) -> tuple[str, list[str]]:
    return f"ctrl{k}", [f"sat{k}{'ab'[j]}" for j in range(sats)]


def net_layout(nets: int, sats: int) -> dict[str, tuple[float, float]]:
    """Controllers on a 250 m circle; satellites out beyond them, fanned apart."""
    pos = {}
    for k in range(1, nets + 1):
        a = 2 * math.pi * (k - 1) / nets + 0.4
        cx, cy = 250 * math.cos(a), 250 * math.sin(a)
        ctrl, sat_names = net_names(k, sats)
        pos[ctrl] = (round(cx), round(cy))
        for j, name in enumerate(sat_names):
            b = a + (0.5 if j else -0.5)
            d = 900 + 400 * j
            pos[name] = (round(cx + d * math.cos(b)), round(cy + d * math.sin(b)))
    return pos


def delivery(sim: Sim, lo: int, hi: int, nets: int, sats: int) -> dict[str, dict]:
    """Per network over [lo, hi) sim time: polls sent, and per satellite the share of
    polls it decoded and of polls whose reply its controller decoded."""
    ids = sim.ids()
    frames = [f for f in sim.state(lo)["frames"] if f["t_start"] < hi]
    out = {}
    for k in range(1, nets + 1):
        ctrl, sat_names = net_names(k, sats)
        cid = ids[ctrl]
        polls = [f for f in frames if f["src"] == cid and f["info"].get("kind") == "POLL"]
        row = {"polls": len(polls), "sats": {}, "lost": Counter(), "poll_lost": Counter()}
        for name in sat_names:
            sid = ids[name]
            heard = 0
            for p in polls:
                at_sat = [r["outcome"] for r in (p["results"] or []) if r["node"] == sid]
                if "ok" in at_sat:
                    heard += 1
                else:
                    row["poll_lost"][at_sat[0] if at_sat else "?"] += 1
            got = 0
            for p in polls:
                sent = [f for f in frames if f["src"] == sid and 0 < f["t_start"] - p["t_end"] < 400_000]
                at_ctrl = [r["outcome"] for f in sent for r in (f["results"] or []) if r["node"] == cid]
                if "ok" in at_ctrl:
                    got += 1
                elif any(r["node"] == sid and r["outcome"] == "ok" for r in (p["results"] or [])):
                    # The satellite heard the poll: why did its reply not arrive?
                    row["lost"][at_ctrl[0] if at_ctrl else "not sent"] += 1
            n = max(len(polls), 1)
            row["sats"][name] = (heard / n, got / n)
        out[ctrl] = row
    return out


def print_delivery(title: str, d: dict[str, dict]) -> None:
    print(title)
    for ctrl, row in d.items():
        cells = ", ".join(f"{s}: poll {h:.0%} reply {g:.0%}" for s, (h, g) in row["sats"].items())
        lost = ", ".join(f"{k} {v}" for k, v in row["lost"].most_common())
        plost = ", ".join(f"{k} {v}" for k, v in row["poll_lost"].most_common())
        print(f"  {ctrl}: {row['polls']} polls; {cells}" + (f"; polls lost: {plost}" if plost else "")
              + (f"; replies lost: {lost}" if lost else ""))


async def link_views(sim: Sim, nets: int) -> None:
    for k in range(1, nets + 1):
        out = await sim.shell(f"ctrl{k}", ["ctrl link"], 0.8)
        lines = [l.strip() for l in out.splitlines()
                 if l.strip().startswith(("channel", "polls", "csma"))]
        print(f"  ctrl{k}: " + " | ".join(lines))


async def rsim(sim: Sim, names: list[str], show: bool) -> None:
    """The simulated radio's lateness figures per node (reading them resets them)."""
    for name in names:
        out = await sim.shell(name, ["rsim"], 0.6)
        if show:
            line = next((l.strip() for l in out.splitlines() if "frames late" in l), "no figures")
            print(f"  {name}: {line}")


async def measure_nets(sim: Sim, args) -> None:
    nets, sats = args.nets, args.sats
    layout = net_layout(nets, sats)
    for k in range(1, nets + 1):
        ctrl, sat_names = net_names(k, sats)
        # Pair side by side far from everyone, then move into place.
        base = (5000 + 3000 * k, 8000)
        sim.add("ctrl", ctrl, *base)
        for j, name in enumerate(sat_names):
            sim.add("sat", name, base[0] + 5 * (j + 1), base[1])
    await asyncio.sleep(3)

    ok = True
    for k in range(1, nets + 1):
        ctrl, sat_names = net_names(k, sats)
        print(f"pairing network {k} ...", flush=True)
        for j, name in enumerate(sat_names):
            ok = await sim.pair(ctrl, name, j) and ok
        for name in (ctrl, *sat_names):
            sim.move(name, *layout[name])
    if not ok:
        print("pairing failed; the numbers below mean little", flush=True)

    await asyncio.sleep(15)
    sat_all = [n for k in range(1, nets + 1) for n in net_names(k, sats)[1]]
    await rsim(sim, sat_all, False)
    if args.aligned:
        ids = sim.ids()
        t0 = sim.now()
        for k in range(1, nets + 1):
            sim.c.post(f"/api/nodes/{ids[f'ctrl{k}']}/restart")
        print(f"\nall {nets} controllers restarted together", flush=True)
        # Satellites keep the old session until it times out (10 s), then rejoin.
        await asyncio.sleep(32)
        for lo, hi in ((0, 10), (10, 20), (20, 30)):
            print_delivery(f"delivery {lo}-{hi} s after the restart:",
                           delivery(sim, t0 + lo * 1_000_000, t0 + hi * 1_000_000, nets, sats))
    else:
        await asyncio.sleep(20)
        t = sim.now()
        print_delivery("\ndelivery, last 20 s:", delivery(sim, t - 20_000_000, t, nets, sats))

    print("controllers' own view:")
    await link_views(sim, nets)
    missed = {}
    for k in range(1, nets + 1):
        for name in net_names(k, sats)[1]:
            missed[name] = (await scrollback(sim, name)).count("missed slot")
    print("satellites' missed slots (late to their own slot):", missed)
    print("simulated radio, satellites:")
    await rsim(sim, sat_all, True)

    if args.commands:
        t0 = sim.now()
        await commands(sim, 0, args.fire_runs, "ctrl1", "sat1a")
        t1 = sim.now()
        print_delivery("delivery during ARM/FIRE on network 1:",
                       delivery(sim, t0, t1, nets, sats))


STAMP = re.compile(r"\[(\d+):(\d+):(\d+)\.(\d+),(\d+)\].*?(Arming\.\.\.|-> ARMED|LAUNCH |Launched|did not|refused)")


def stamps(log: str) -> list[tuple[float, str]]:
    """(seconds of uptime, event) for the controller's arm and launch messages."""
    out = []
    for m in STAMP.finditer(log):
        h, mi, sec, ms, us = (int(x) for x in m.groups()[:5])
        out.append((h * 3600 + mi * 60 + sec + ms / 1e3 + us / 1e6, m.group(6)))
    return out


async def scrollback(sim: Sim, name: str) -> str:
    url = f"ws://127.0.0.1:{sim.port}/ws/console/{sim.ids()[name]}"
    async with websockets.connect(url, max_size=None) as ws:
        return ANSI.sub(b"", await asyncio.wait_for(ws.recv(), 5)).decode(errors="replace")


async def commands(sim: Sim, loss: float, args_runs_cmd: int = 3, ctrl: str = "ctrl1",
                   sat: str = "satA") -> None:
    """ARM and LAUNCH pad 2 (satellite A, IGN 2) on ctrl; time the acknowledgements
    from the controller's own log."""
    print(f"\nARM / FIRE on network 1, {loss:g} % random loss on the {ctrl}-{sat} link:")
    for run in range(args_runs_cmd):
        # BACK disarms, or in SAFE clears the ticks; then tick pad 2 alone
        out = await sim.shell(ctrl, ["ctrl back", "ctrl select 2", "ctrl arm"], 0.4)
        out += await sim.shell(ctrl, [], 2.0)
        out += await sim.shell(ctrl, ["ctrl fire on"], 1.3)
        out += await sim.shell(ctrl, ["ctrl fire off"], 4.0)
        ev = stamps(out)
        t = {name: at for at, name in ev}
        arm = t.get("-> ARMED", 0) - t.get("Arming...", 0) if "-> ARMED" in t else None
        fire = t.get("Launched", 0) - t.get("LAUNCH ", 0) if "Launched" in t else None
        fmt = lambda v: f"{v:.2f} s" if v is not None else "failed"
        print(f"  run {run + 1}: ARM acknowledged after {fmt(arm)}, "
              f"launch confirmed after {fmt(fire)} (1 s pulse included)")
        if arm is None or fire is None:
            print("    controller said:", "; ".join(
                l.split("controller: ", 1)[1] for l in out.splitlines() if "controller: " in l))
            log = (await scrollback(sim, sat)).splitlines()
            print(f"    {sat} said:", "; ".join(l.split("> ", 1)[-1] for l in log[-400:]
                                              if "commands:" in l or "satellite:" in l or "sat_link" in l)[-900:])
    fired = (await scrollback(sim, sat)).count("FIRING, mask")
    print(f"  {sat} executed FIRE {fired} times for {args_runs_cmd} launches")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ctrl-exe", type=Path,
                    default=FIRMWARE / "controller/build-radiosim/zephyr/zephyr.exe")
    ap.add_argument("--sat-exe", type=Path, default=FIRMWARE / "satellite/build-sim/zephyr/zephyr.exe")
    ap.add_argument("--runs", type=int, default=5, help="satA restarts to time")
    ap.add_argument("--timeout", type=float, default=60, help="seconds to wait for a join")
    ap.add_argument("--commands", action="store_true", help="also time ARM and FIRE")
    ap.add_argument("--fire-runs", type=int, default=3, help="ARM/FIRE rounds with --commands")
    ap.add_argument("--loss", type=float, default=0, help="%% random loss ctrl1-satA during ARM/FIRE")
    ap.add_argument("--nets", type=int, default=0, help="build 2..4 networks instead (see above)")
    ap.add_argument("--sats", type=int, default=2, choices=(1, 2), help="satellites per network")
    ap.add_argument("--aligned", action="store_true", help="with --nets: restart all controllers together")
    ap.add_argument("--http-port", type=int, default=8079)
    ap.add_argument("--udp-port", type=int, default=7711)
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix="coexist-") as work:
        proc = subprocess.Popen(
            [sys.executable, "-m", "radiosim", "--workdir", work, "--port", str(args.http_port),
             "--udp-port", str(args.udp_port), "--ctrl-exe", str(args.ctrl_exe),
             "--sat-exe", str(args.sat_exe), "--no-record"],
            cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        try:
            sim = Sim(args.http_port)
            for _ in range(50):
                try:
                    sim.state()
                    break
                except httpx.HTTPError:
                    time.sleep(0.2)
            asyncio.run(measure_nets(sim, args) if args.nets else measure(sim, args))
        finally:
            proc.terminate()
            proc.wait(timeout=10)


if __name__ == "__main__":
    main()
