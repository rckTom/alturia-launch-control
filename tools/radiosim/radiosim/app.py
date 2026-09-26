"""HTTP API and WebSockets for the web UI.

Every handler is async: the simulator's state belongs to the event loop,
and FastAPI would run plain functions on a thread pool.

  /ws                  JSON events: a snapshot first, then deltas
  /ws/console/{id}     a node's terminal, raw bytes both ways
  /ws/screen/{id}      a controller's display frames, binary
  /api/...             commands
"""

from __future__ import annotations

import asyncio
import json
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Literal

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from . import devices, proto
from .medium import Node
from .sim import Sim

WEB_DIST = Path(__file__).resolve().parent.parent / "web" / "dist"


class NodeCreate(BaseModel):
    role: Literal["ctrl", "sat"]
    name: str | None = None
    x: float | None = None
    y: float | None = None
    start: bool = True


class EntityPatch(BaseModel):
    x: float | None = None
    y: float | None = None
    tx_power_override: int | None = None
    clear_tx_power_override: bool = False
    isolated: bool | None = None


class ButtonEvent(BaseModel):
    index: int = 0
    pressed: bool


class InputEvent(BaseModel):
    type: Literal["pin", "wheel", "touch"]
    code: int = 0
    value: int


class LinkSet(BaseModel):
    a: int
    b: int
    extra_db: float = 0.0
    cut: bool = False
    loss_pct: float = 0.0


class DeviceCreate(BaseModel):
    device_type: Literal["jammer", "traffic", "radio"]
    params: dict = {}


class Inject(BaseModel):
    src: int
    hex: str
    sf: int | None = None


class Replay(BaseModel):
    frame_id: int
    src: int


class ScenarioLoad(BaseModel):
    scenario: dict | None = None
    path: str | None = None
    autostart: bool = False


class ScenarioSave(BaseModel):
    path: str | None = None


INPUT_TYPES = {"pin": proto.IN_PIN, "wheel": proto.IN_WHEEL, "touch": proto.IN_TOUCH}


def create_app(sim: Sim) -> FastAPI:
    @asynccontextmanager
    async def lifespan(app: FastAPI):
        yield
        # Here, not after uvicorn returns: it re-raises SIGINT/SIGTERM on its
        # way out, which would leave the nodes running.
        await sim.stop()

    app = FastAPI(title="radiosim", lifespan=lifespan)

    def node_or_404(eid: int) -> Node:
        try:
            return sim.node(eid)
        except KeyError:
            raise HTTPException(404, f"no node {eid}")

    def device_or_404(eid: int) -> devices.Device:
        try:
            return sim.device(eid)
        except KeyError:
            raise HTTPException(404, f"no device {eid}")

    # ------------------------------------------------------------------ state

    @app.get("/api/state")
    async def state(frames_since: int | None = None):
        """The whole state. frames_since (sim time, us) trims the frame list.

        Serialized with json.dumps: FastAPI's own encoder took ~200 ms for
        2000 frames, and the event loop that delivers frames on time stood
        still meanwhile (the firmware's reply slots start 6 ms after a poll).
        """
        snap = sim.snapshot()
        if frames_since is not None:
            snap["frames"] = [f for f in snap["frames"] if f["t_start"] >= frames_since]
        snap["stats"] = sim.last_stats
        return Response(json.dumps(snap), media_type="application/json")

    @app.get("/api/frames/{fid}")
    async def frame(fid: int):
        tx = sim.medium.history.get(fid)
        if tx is None:
            raise HTTPException(404, "frame not in history")
        return sim.medium.frame_dict(tx)

    @app.put("/api/rf")
    async def set_rf(params: dict):
        sim.medium.rf.update(params)
        sim.bus.emit({"type": "rf", "rf": sim.medium.rf.to_dict()})
        return sim.medium.rf.to_dict()

    # ------------------------------------------------------------------ nodes

    @app.post("/api/nodes")
    async def add_node(req: NodeCreate):
        if req.start and not sim.procs.can_spawn(req.role):
            raise HTTPException(400, f"no firmware image for {req.role}; see radiosim --help")
        try:
            node = sim.add_node(req.role, req.name, req.x, req.y)
        except ValueError as e:
            raise HTTPException(409, str(e))
        if req.start:
            await sim.start_node(node)
        return node.to_dict()

    @app.post("/api/nodes/{eid}/start")
    async def start_node(eid: int):
        node = node_or_404(eid)
        if not sim.procs.can_spawn(node.role):
            raise HTTPException(400, f"no firmware image for {node.role}")
        await sim.start_node(node)
        return node.to_dict()

    @app.post("/api/nodes/{eid}/kill")
    async def kill_node(eid: int):
        await sim.kill_node(node_or_404(eid))
        return {"ok": True}

    @app.post("/api/nodes/{eid}/restart")
    async def restart_node(eid: int):
        node = node_or_404(eid)
        await sim.restart_node(node)
        return node.to_dict()

    @app.delete("/api/nodes/{eid}")
    async def remove_node(eid: int):
        await sim.remove_node(node_or_404(eid))
        return {"ok": True}

    @app.post("/api/nodes/{eid}/button")
    async def button(eid: int, ev: ButtonEvent):
        if not sim.medium.button(node_or_404(eid), ev.index, ev.pressed):
            raise HTTPException(409, "node not connected")
        return {"ok": True}

    @app.post("/api/nodes/{eid}/input")
    async def node_input(eid: int, ev: InputEvent):
        if not sim.medium.input(node_or_404(eid), INPUT_TYPES[ev.type], ev.code, ev.value):
            raise HTTPException(409, "node not connected")
        return {"ok": True}

    @app.patch("/api/entities/{eid}")
    async def patch_entity(eid: int, p: EntityPatch):
        ent = sim.medium.entities.get(eid)
        if ent is None:
            raise HTTPException(404, f"no entity {eid}")
        if p.x is not None and p.y is not None:
            sim.medium.move(eid, p.x, p.y)
        if p.clear_tx_power_override:
            ent.tx_power_override = None
        elif p.tx_power_override is not None:
            ent.tx_power_override = p.tx_power_override
        if p.isolated is not None and isinstance(ent, Node):
            ent.isolated = p.isolated
        sim.medium.emit_entity(ent)
        return ent.to_dict()

    @app.put("/api/links")
    async def set_link(lk: LinkSet):
        if lk.a not in sim.medium.entities or lk.b not in sim.medium.entities:
            raise HTTPException(404, "no such entity")
        sim.medium.set_link(lk.a, lk.b, lk.extra_db, lk.cut, lk.loss_pct)
        return sim.medium.links_list()

    # ---------------------------------------------------------------- devices

    @app.post("/api/devices")
    async def add_device(req: DeviceCreate):
        return sim.add_device(req.device_type, req.params).to_dict()

    @app.patch("/api/devices/{eid}")
    async def update_device(eid: int, params: dict):
        dev = device_or_404(eid)
        sim.update_device(dev, params)
        return dev.to_dict()

    @app.delete("/api/devices/{eid}")
    async def remove_device(eid: int):
        sim.remove_device(device_or_404(eid))
        return {"ok": True}

    @app.post("/api/inject")
    async def inject(req: Inject):
        try:
            data = bytes.fromhex(req.hex.replace(" ", ""))
        except ValueError:
            raise HTTPException(400, "not hex")
        if not 1 <= len(data) <= 255:
            raise HTTPException(400, "1 to 255 bytes")
        try:
            return {"id": sim.inject(req.src, data, req.sf)}
        except (KeyError, ValueError) as e:
            raise HTTPException(400, str(e))

    @app.post("/api/replay")
    async def replay(req: Replay):
        try:
            return {"id": sim.replay(req.frame_id, req.src)}
        except KeyError:
            raise HTTPException(404, "frame not in history")
        except ValueError as e:
            raise HTTPException(400, str(e))

    # --------------------------------------------------------------- scenario

    @app.get("/api/scenario")
    async def get_scenario():
        return JSONResponse(sim.dump_scenario(),
                            headers={"Content-Disposition": 'attachment; filename="scenario.json"'})

    @app.post("/api/scenario/save")
    async def save_scenario(req: ScenarioSave):
        path = Path(req.path) if req.path else sim.workdir / "scenario.json"
        path.write_text(json.dumps(sim.dump_scenario(), indent=2))
        return {"path": str(path)}

    @app.post("/api/scenario/load")
    async def load_scenario(req: ScenarioLoad):
        if req.scenario is not None:
            data = req.scenario
        else:
            path = Path(req.path) if req.path else sim.workdir / "scenario.json"
            if not path.exists():
                raise HTTPException(404, f"{path} not found")
            data = json.loads(path.read_text())
        await sim.load_scenario(data, req.autostart)
        return {"ok": True}

    # ------------------------------------------------------------- websockets

    @app.websocket("/ws")
    async def events(ws: WebSocket):
        await ws.accept()
        q = sim.bus.subscribe()
        try:
            snap = sim.snapshot()
            await ws.send_text(json.dumps(snap, separators=(",", ":")))
            if sim.last_stats:
                await ws.send_text(json.dumps(sim.last_stats, separators=(",", ":")))
            while True:
                try:
                    text = await asyncio.wait_for(q.get(), 1.0)
                except TimeoutError:
                    if q not in sim.bus.queues:
                        break  # dropped for being too slow; the page reconnects
                    continue
                await ws.send_text(text)
        except WebSocketDisconnect:
            pass
        finally:
            sim.bus.unsubscribe(q)

    @app.websocket("/ws/console/{eid}")
    async def console(ws: WebSocket, eid: int):
        try:
            node = sim.node(eid)
        except KeyError:
            await ws.close(code=4404)
            return
        await ws.accept()
        con = sim.procs.console(node.name)
        out: asyncio.Queue[bytes] = asyncio.Queue()
        listener = out.put_nowait
        if con.buf:
            await ws.send_bytes(bytes(con.buf))
        con.listeners.add(listener)

        async def pump():
            while True:
                data = await out.get()
                while not out.empty():
                    data += out.get_nowait()
                await ws.send_bytes(data)

        task = asyncio.create_task(pump())
        try:
            while True:
                msg = await ws.receive()
                if msg["type"] == "websocket.disconnect":
                    break
                data = msg.get("bytes") or (msg.get("text") or "").encode()
                if data and not con.write(data):
                    await ws.send_bytes(b"\r\n\x1b[33m[not attached: the simulator did not start this node]\x1b[0m\r\n")
        except WebSocketDisconnect:
            pass
        finally:
            con.listeners.discard(listener)
            task.cancel()

    @app.websocket("/ws/screen/{eid}")
    async def screen(ws: WebSocket, eid: int):
        try:
            q = sim.watch_screen(eid)
        except KeyError:
            await ws.close(code=4404)
            return
        await ws.accept()

        async def ignore_input():
            while True:
                msg = await ws.receive()
                if msg["type"] == "websocket.disconnect":
                    return

        reader = asyncio.create_task(ignore_input())
        try:
            while not reader.done():
                getter = asyncio.create_task(q.get())
                done, _ = await asyncio.wait({getter, reader}, return_when=asyncio.FIRST_COMPLETED)
                if getter in done:
                    await ws.send_bytes(getter.result())
                else:
                    getter.cancel()
        except WebSocketDisconnect:
            pass
        finally:
            reader.cancel()
            sim.unwatch_screen(eid, q)

    # ---------------------------------------------------------------- web UI

    if WEB_DIST.exists():
        app.mount("/assets", StaticFiles(directory=WEB_DIST / "assets"), name="assets")

        @app.get("/{path:path}")
        async def spa(path: str):
            f = WEB_DIST / path
            if path and f.is_file():
                return FileResponse(f)
            return FileResponse(WEB_DIST / "index.html")
    else:

        @app.get("/")
        async def no_ui():
            return JSONResponse({"error": "web UI not built: cd tools/radiosim/web && npm install && npm run build"})

    return app
