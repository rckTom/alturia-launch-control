"""Wire format to the nodes, mirroring firmware/common/lora/radio_sim_proto.h.

One message per UDP datagram, packed little-endian. Times are host
CLOCK_MONOTONIC in microseconds (``time.monotonic_ns() // 1000``).
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

MAGIC = 0x5352
VERSION = 1
DEFAULT_PORT = 7700
T_OPEN = 0xFFFF_FFFF_FFFF_FFFF

# node -> simulator
HELLO = 1
TX = 2
PROFILE = 3
IO = 4
DISPLAY = 5
# simulator -> node
WELCOME = 64
RX = 65
ENERGY = 66
ENERGY_END = 67
BUTTON = 68
INPUT = 69

ROLE_OTHER = 0
ROLE_CTRL = 1
ROLE_SAT = 2
ROLE_NAMES = {ROLE_OTHER: "other", ROLE_CTRL: "ctrl", ROLE_SAT: "sat"}

PIX_MONO01 = 1
PIX_MONO10 = 2

IN_PIN = 1
IN_WHEEL = 2
IN_TOUCH = 3

_HDR = struct.Struct("<HBB")
_HELLO = struct.Struct("<HBBBBHIQ16s")
_TX = struct.Struct("<HBBQIIHBbB")
_PROFILE = struct.Struct("<HBBB")
_IO = struct.Struct("<HBBH")
_DISPLAY = struct.Struct("<HBBHHBBH")
_WELCOME = struct.Struct("<HBBHh")
_RX = struct.Struct("<HBBQhbBB")
_ENERGY = struct.Struct("<HBBIQQh")
_ENERGY_END = struct.Struct("<HBBIQ")
_BUTTON = struct.Struct("<HBBBB")
_INPUT = struct.Struct("<HBBBBHi")

# Sizes of the C structs, checked by the tests against radio_sim_proto.h.
SIZES = {
    "hdr": _HDR.size,
    "hello": _HELLO.size,
    "tx": _TX.size,
    "profile": _PROFILE.size,
    "io": _IO.size,
    "display": _DISPLAY.size,
    "welcome": _WELCOME.size,
    "rx": _RX.size,
    "energy": _ENERGY.size,
    "energy_end": _ENERGY_END.size,
    "button": _BUTTON.size,
    "input": _INPUT.size,
}


class ProtoError(ValueError):
    pass


@dataclass
class Hello:
    role: int
    sf: int
    leds: int
    pid: int
    uid: int
    name: str


@dataclass
class Tx:
    t_start: int
    airtime_us: int
    freq_hz: int
    bw_khz: int
    sf: int
    power_dbm: int
    data: bytes


@dataclass
class Profile:
    sf: int


@dataclass
class Io:
    leds: int


@dataclass
class Display:
    width: int
    height: int
    format: int
    stride: int
    data: bytes


Message = Hello | Tx | Profile | Io | Display


def parse(buf: bytes) -> Message:
    """Parse a node -> simulator message."""
    if len(buf) < _HDR.size:
        raise ProtoError("short datagram")
    magic, version, mtype = _HDR.unpack_from(buf)
    if magic != MAGIC:
        raise ProtoError(f"bad magic {magic:#x}")
    if version != VERSION:
        raise ProtoError(f"protocol version {version}, expected {VERSION}")
    try:
        if mtype == HELLO:
            _, _, _, role, sf, leds, pid, uid, name = _HELLO.unpack_from(buf)
            return Hello(role, sf, leds, pid, uid, name.rstrip(b"\0").decode(errors="replace"))
        if mtype == TX:
            _, _, _, t_start, airtime, freq, bw, sf, power, length = _TX.unpack_from(buf)
            data = bytes(buf[_TX.size : _TX.size + length])
            if len(data) != length:
                raise ProtoError("truncated TX")
            return Tx(t_start, airtime, freq, bw, sf, power, data)
        if mtype == PROFILE:
            return Profile(_PROFILE.unpack_from(buf)[3])
        if mtype == IO:
            return Io(_IO.unpack_from(buf)[3])
        if mtype == DISPLAY:
            _, _, _, w, h, fmt, _, stride = _DISPLAY.unpack_from(buf)
            data = bytes(buf[_DISPLAY.size :])
            if len(data) < stride * h:
                raise ProtoError("truncated DISPLAY")
            return Display(w, h, fmt, stride, data[: stride * h])
    except struct.error as e:
        raise ProtoError(str(e)) from e
    raise ProtoError(f"unknown message type {mtype}")


def welcome(node_id: int, noise_floor_dbm: int) -> bytes:
    return _WELCOME.pack(MAGIC, VERSION, WELCOME, node_id, noise_floor_dbm)


def rx(t_end: int, rssi_dbm: int, snr_db: int, sf: int, data: bytes) -> bytes:
    return _RX.pack(MAGIC, VERSION, RX, t_end, rssi_dbm, snr_db, sf, len(data)) + data


def energy(eid: int, t_start: int, t_end: int, rssi_dbm: int) -> bytes:
    return _ENERGY.pack(MAGIC, VERSION, ENERGY, eid & 0xFFFFFFFF, t_start, min(t_end, T_OPEN), rssi_dbm)


def energy_end(eid: int, t_end: int) -> bytes:
    return _ENERGY_END.pack(MAGIC, VERSION, ENERGY_END, eid & 0xFFFFFFFF, t_end)


def button(index: int, pressed: bool) -> bytes:
    return _BUTTON.pack(MAGIC, VERSION, BUTTON, index, 1 if pressed else 0)


def input_event(itype: int, code: int, value: int) -> bytes:
    return _INPUT.pack(MAGIC, VERSION, INPUT, itype, 0, code, value)


# Node-side builders, for tests and fake nodes.


def hello(role: int, sf: int, name: str, uid: int = 0, pid: int = 0, leds: int = 0) -> bytes:
    return _HELLO.pack(MAGIC, VERSION, HELLO, role, sf, leds, pid, uid, name.encode()[:16])


def tx(t_start: int, airtime_us: int, sf: int, power_dbm: int, data: bytes,
       freq_hz: int = 869_850_000, bw_khz: int = 250) -> bytes:
    return _TX.pack(MAGIC, VERSION, TX, t_start, airtime_us, freq_hz, bw_khz, sf, power_dbm,
                    len(data)) + data


def profile(sf: int) -> bytes:
    return _PROFILE.pack(MAGIC, VERSION, PROFILE, sf)


def io(leds: int) -> bytes:
    return _IO.pack(MAGIC, VERSION, IO, leds)


def parse_down(buf: bytes) -> tuple[int, tuple]:
    """Parse a simulator -> node message (for tests): (type, fields)."""
    _, _, mtype = _HDR.unpack_from(buf)
    table = {
        WELCOME: _WELCOME,
        ENERGY: _ENERGY,
        ENERGY_END: _ENERGY_END,
        BUTTON: _BUTTON,
        INPUT: _INPUT,
    }
    if mtype == RX:
        f = _RX.unpack_from(buf)
        return mtype, f[3:] + (bytes(buf[_RX.size :]),)
    return mtype, table[mtype].unpack_from(buf)[3:]
