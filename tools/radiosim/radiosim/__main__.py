"""radiosim: run the radio network simulator and its web UI.

  uv run radiosim                      # from tools/radiosim
  uv run radiosim --scenario sim-run/scenario.json --autostart
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
from pathlib import Path

import uvicorn

from . import proto
from .app import create_app
from .sim import Sim

REPO = Path(__file__).resolve().parents[3]
FIRMWARE = REPO / "firmware"
DEFAULT_EXES = {
    "ctrl": FIRMWARE / "controller" / "build-radiosim" / "zephyr" / "zephyr.exe",
    "sat": FIRMWARE / "satellite" / "build-sim" / "zephyr" / "zephyr.exe",
}


def main() -> None:
    ap = argparse.ArgumentParser(prog="radiosim", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1", help="web UI address (default %(default)s)")
    ap.add_argument("--port", type=int, default=8077, help="web UI port (default %(default)s)")
    ap.add_argument("--udp-port", type=int, default=proto.DEFAULT_PORT,
                    help="port the nodes talk to (default %(default)s)")
    ap.add_argument("--workdir", type=Path, default=Path("sim-run"),
                    help="flash images, recordings and scenarios (default %(default)s)")
    ap.add_argument("--ctrl-exe", type=Path, default=DEFAULT_EXES["ctrl"],
                    help="controller image (default %(default)s)")
    ap.add_argument("--sat-exe", type=Path, default=DEFAULT_EXES["sat"],
                    help="satellite image (default %(default)s)")
    ap.add_argument("--scenario", type=Path, help="scenario to load at start")
    ap.add_argument("--autostart", action="store_true", help="start the scenario's nodes")
    ap.add_argument("--no-record", action="store_true", help="do not write a JSONL recording")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s: %(message)s")
    workdir = args.workdir.resolve()
    exes = {"ctrl": args.ctrl_exe.resolve(), "sat": args.sat_exe.resolve()}
    for role, exe in exes.items():
        if not exe.exists():
            logging.warning("no %s image at %s; build it or pass --%s-exe", role, exe, role)

    asyncio.run(run(args, workdir, exes))


async def run(args, workdir: Path, exes: dict) -> None:
    sim = Sim(workdir, exes, args.udp_port, record=not args.no_record)
    await sim.start()
    if args.scenario:
        await sim.load_scenario(json.loads(args.scenario.read_text()), args.autostart)

    config = uvicorn.Config(create_app(sim), host=args.host, port=args.port, log_level="warning")
    server = uvicorn.Server(config)
    logging.info("web UI on http://%s:%d", args.host, args.port)

    # uvicorn handles SIGINT/SIGTERM and returns from serve().
    try:
        await server.serve()
    finally:
        await sim.stop()


if __name__ == "__main__":
    main()
