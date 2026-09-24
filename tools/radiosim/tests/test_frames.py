from radiosim import frames


def test_decode():
    assert frames.decode(bytes([0x40 | 37, 0x34, 0x12, 0, 0xFF, 1, 5, 0, 0, 0]))["kind"] == "POLL"
    d = frames.decode(bytes([0x40 | 16, 0xCD, 0xAB, 3, 0]))
    assert d == {"kind": "HELLO", "net_tag": "abcd", "src": 3, "dst": 0}
    d = frames.decode(bytes([0x41]) + (0x1122334455667788).to_bytes(8, "little"))
    assert d == {"kind": "PAIR_ADV", "uid": "1122334455667788"}
    assert frames.decode(bytes([0xC5, 1, 2]))["kind"] == "FOREIGN"
