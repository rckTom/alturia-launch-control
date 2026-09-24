"""Every frame and its outcome as JSON lines, for looking at a run afterwards."""

from __future__ import annotations

import json
import time
from pathlib import Path
from typing import IO


class Recorder:
    KEEP = {"frame", "frame_result", "frame_end", "entity", "entity_removed", "links", "rf"}

    def __init__(self, workdir: Path):
        workdir.mkdir(parents=True, exist_ok=True)
        self.path = workdir / time.strftime("record-%Y%m%d-%H%M%S.jsonl")
        self._f: IO[str] | None = self.path.open("w", buffering=1 << 16)

    def __call__(self, ev: dict) -> None:
        if self._f is not None and ev.get("type") in self.KEEP:
            self._f.write(json.dumps(ev, separators=(",", ":")) + "\n")

    def close(self) -> None:
        if self._f is not None:
            self._f.close()
            self._f = None
