"""Plaintext header of link frames, per firmware/common/link/lsec_internal.h."""

from __future__ import annotations

LSEC_VERSION = 1

KINDS = {
    1: "PAIR_ADV",
    2: "PAIR_REQ",
    3: "PAIR_RSP",
    4: "PAIR_NC",
    5: "PAIR_NS",
    6: "PAIR_CHK_C",
    7: "PAIR_CHK_S",
    8: "PAIR_ABORT",
    16: "HELLO",
    17: "JOIN_REQ",
    18: "JOIN_RSP",
    32: "JOIN_OK",
    33: "JOIN_ACK",
    34: "GROUP_UPDATE",
    35: "GROUP_ACK",
    36: "DATA",
    37: "POLL",
}


def decode(data: bytes) -> dict:
    """What can be read without keys: kind, and the addressing it carries."""
    if not data:
        return {"kind": "EMPTY"}
    ver, k = data[0] >> 6, data[0] & 0x3F
    if ver != LSEC_VERSION or k not in KINDS:
        return {"kind": "FOREIGN"}
    out: dict = {"kind": KINDS[k]}
    if 1 <= k <= 8:
        if len(data) >= 9:
            out["uid"] = f"{int.from_bytes(data[1:9], 'little'):016x}"
        return out
    if len(data) >= 5:
        out["net_tag"] = f"{int.from_bytes(data[1:3], 'little'):04x}"
        out["src"] = data[3]
        out["dst"] = data[4]
    if k >= 32 and len(data) >= 10:
        out["kid"] = data[5]
        out["ctr"] = int.from_bytes(data[6:10], "little")
    return out
