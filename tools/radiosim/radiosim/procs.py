"""Starts and stops the firmware images, each on its own pseudo-terminal.

A node's Zephyr shell UART is its stdin/stdout (--uart_stdinout), and those
are the slave side of a pty the simulator owns, so the web UI's terminal
gets exactly what a serial terminal would: echo, completion, colours. The
output is also kept in a scrollback buffer for terminals that attach later.

Every instance gets its own flash file (pairings survive a restart), its
own hwinfo device id (the satellite's uid) and a fresh entropy seed (the
fake entropy source of native_sim is otherwise the same in every process).
"""

from __future__ import annotations

import asyncio
import ctypes
import logging
import os
import pty
import secrets
import signal
import termios
import tty
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

log = logging.getLogger(__name__)

SCROLLBACK = 256 * 1024
PR_SET_PDEATHSIG = 1


def _die_with_parent() -> None:
    """In the child: get SIGTERM when the simulator goes, however it goes."""
    try:
        ctypes.CDLL(None, use_errno=True).prctl(PR_SET_PDEATHSIG, signal.SIGTERM)
    except (OSError, AttributeError):
        pass


@dataclass
class Console:
    """A node's terminal: scrollback plus whoever is attached."""

    buf: bytearray = field(default_factory=bytearray)
    listeners: set[Callable[[bytes], None]] = field(default_factory=set)
    writer: Callable[[bytes], None] | None = None

    def feed(self, data: bytes) -> None:
        self.buf += data
        if len(self.buf) > SCROLLBACK:
            del self.buf[: len(self.buf) - SCROLLBACK]
        for fn in list(self.listeners):
            fn(data)

    def write(self, data: bytes) -> bool:
        if self.writer is None:
            return False
        self.writer(data)
        return True


@dataclass
class Proc:
    name: str
    role: str
    args: list[str]
    process: asyncio.subprocess.Process
    master_fd: int


class ProcessManager:
    def __init__(self, workdir: Path, exes: dict[str, Path | None], sim_port: int,
                 on_exit: Callable[[str, int], None] = lambda name, code: None):
        self.workdir = workdir
        self.exes = exes
        self.sim_port = sim_port
        self.on_exit = on_exit
        self.procs: dict[str, Proc] = {}
        self.consoles: dict[str, Console] = {}

    def console(self, name: str) -> Console:
        return self.consoles.setdefault(name, Console())

    def running(self, name: str) -> bool:
        p = self.procs.get(name)
        return p is not None and p.process.returncode is None

    def can_spawn(self, role: str) -> bool:
        exe = self.exes.get(role)
        return exe is not None and exe.exists()

    async def spawn(self, name: str, role: str, device_id: int) -> Proc:
        if self.running(name):
            return self.procs[name]
        exe = self.exes.get(role)
        if exe is None or not exe.exists():
            raise FileNotFoundError(f"no firmware image for role {role!r}: {exe}")

        self.workdir.mkdir(parents=True, exist_ok=True)
        args = [
            str(exe),
            "--uart_stdinout",
            f"--radio_sim=127.0.0.1:{self.sim_port}",
            f"--radio_name={name}",
            f"--radio_role={role}",
            f"--seed={secrets.randbits(31)}",
            f"--device_id={device_id}",
            f"--flash={self.workdir / (name + '.flash')}",
        ]

        master, slave = pty.openpty()
        # The Zephyr shell does its own echo and line editing, but boot
        # messages end lines with a bare \n: keep the tty's output newline
        # translation.
        tty.setraw(slave)
        attrs = termios.tcgetattr(slave)
        attrs[1] |= termios.OPOST | termios.ONLCR
        termios.tcsetattr(slave, termios.TCSANOW, attrs)
        os.set_blocking(master, False)
        process = await asyncio.create_subprocess_exec(
            *args, stdin=slave, stdout=slave, stderr=slave, cwd=self.workdir,
            start_new_session=True, preexec_fn=_die_with_parent,
        )
        os.close(slave)

        proc = Proc(name, role, args, process, master)
        self.procs[name] = proc
        con = self.console(name)
        con.feed(f"\r\n\x1b[2m--- {name}: {' '.join(args)}\x1b[0m\r\n".encode())
        con.writer = lambda data, fd=master: self._write(fd, data)

        loop = asyncio.get_running_loop()
        loop.add_reader(master, self._read, name, master)
        loop.create_task(self._wait(proc))
        log.info("started %s (%s), pid %d", name, role, process.pid)
        return proc

    def _read(self, name: str, fd: int) -> None:
        try:
            data = os.read(fd, 65536)
        except BlockingIOError:
            return
        except OSError:
            data = b""
        if not data:
            asyncio.get_running_loop().remove_reader(fd)
            return
        self.console(name).feed(data)

    @staticmethod
    def _write(fd: int, data: bytes) -> None:
        try:
            os.write(fd, data)
        except OSError:
            pass

    async def _wait(self, proc: Proc) -> None:
        code = await proc.process.wait()
        loop = asyncio.get_running_loop()
        loop.remove_reader(proc.master_fd)
        # Whatever it printed last.
        self._read(proc.name, proc.master_fd)
        try:
            os.close(proc.master_fd)
        except OSError:
            pass
        con = self.console(proc.name)
        if con.writer is not None and self.procs.get(proc.name) is proc:
            con.writer = None
        con.feed(f"\r\n\x1b[2m--- {proc.name} exited ({code})\x1b[0m\r\n".encode())
        if self.procs.get(proc.name) is proc:
            del self.procs[proc.name]
        log.info("%s exited (%s)", proc.name, code)
        self.on_exit(proc.name, code)

    async def kill(self, name: str, timeout: float = 2.0) -> None:
        proc = self.procs.get(name)
        if proc is None or proc.process.returncode is not None:
            return
        proc.process.send_signal(signal.SIGTERM)
        try:
            await asyncio.wait_for(proc.process.wait(), timeout)
        except TimeoutError:
            proc.process.kill()
            await proc.process.wait()
        # Let _wait() clean up.
        await asyncio.sleep(0)

    async def kill_all(self) -> None:
        await asyncio.gather(*(self.kill(n) for n in list(self.procs)))
