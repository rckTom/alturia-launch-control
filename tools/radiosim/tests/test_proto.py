import re
from pathlib import Path

from radiosim import proto, rf

HEADER = Path(__file__).resolve().parents[3] / "firmware/common/lora/radio_sim_proto.h"

C_TYPES = {"uint8_t": 1, "int8_t": 1, "uint16_t": 2, "int16_t": 2, "uint32_t": 4, "int32_t": 4,
           "uint64_t": 8, "struct rsim_hdr": 4}


def c_struct_sizes() -> dict[str, int]:
    """Sizes of the packed structs in radio_sim_proto.h, flexible arrays excluded."""
    text = HEADER.read_text()
    sizes = {}
    for name, body in re.findall(r"struct rsim_(\w+) \{(.*?)\} __attribute__", text, re.S):
        size = 0
        for line in body.splitlines():
            line = line.split("/*")[0].strip().rstrip(";")
            if not line:
                continue
            m = re.match(r"(struct rsim_hdr|u?int\d+_t|char) (\w+)(\[(\d*)\])?", line)
            assert m, line
            ctype, _, arr, n = m.groups()
            unit = 1 if ctype == "char" else C_TYPES[ctype]
            if arr is None:
                size += unit
            elif n:
                size += unit * int(n)
        sizes[name] = size
    return sizes


def test_struct_sizes_match_c_header():
    c = c_struct_sizes()
    for name, size in proto.SIZES.items():
        assert c[name] == size, name


def test_constants_match_c_header():
    text = HEADER.read_text()
    assert f"RSIM_VERSION {proto.VERSION}" in text
    for name in ("HELLO", "TX", "PROFILE", "IO", "DISPLAY", "WELCOME", "RX", "ENERGY",
                 "ENERGY_END", "BUTTON", "INPUT"):
        m = re.search(rf"RSIM_{name} = (\d+)", text)
        assert int(m.group(1)) == getattr(proto, name), name


def test_roundtrip_up():
    h = proto.parse(proto.hello(proto.ROLE_SAT, 8, "satA", uid=0x1122, pid=7, leds=5))
    assert (h.role, h.sf, h.name, h.uid, h.pid, h.leds) == (proto.ROLE_SAT, 8, "satA", 0x1122, 7, 5)
    t = proto.parse(proto.tx(123, 4567, 7, -3, b"\x41\x02\x03"))
    assert (t.t_start, t.airtime_us, t.sf, t.power_dbm, t.data) == (123, 4567, 7, -3, b"\x41\x02\x03")


def test_roundtrip_down():
    mtype, f = proto.parse_down(proto.rx(99, -80, 7, 7, b"abc"))
    assert mtype == proto.RX and f == (99, -80, 7, 7, 3, b"abc")
    mtype, f = proto.parse_down(proto.energy(5, 1, proto.T_OPEN, -60))
    assert f == (5, 1, proto.T_OPEN, -60)


def ms(us: int) -> int:
    return -(-us // 1000)


def test_airtime_matches_link_sched():
    # The Semtech calculator values common/link/tests/src/sched.c checks.
    for length, sf, bw, want in ((22, 7, 250, 28), (50, 7, 250, 49), (63, 7, 250, 59),
                                 (22, 9, 125, 206), (22, 5, 250, 9), (22, 12, 125, 1483)):
        assert abs(ms(rf.airtime_us(length, sf, bw)) - want) <= 1, (length, sf, bw)
