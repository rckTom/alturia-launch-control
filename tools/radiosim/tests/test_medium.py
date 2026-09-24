from radiosim import proto
from radiosim.medium import OPEN, Transmission


def test_delivers_in_range(air):
    a = air.node("ctrl1", 0, 0, role=proto.ROLE_CTRL)
    b = air.node("satA", 100, 0)
    tx = air.tx(a, b"\x65hello")
    air.advance(30_000)
    assert air.outcome(tx, b) == "ok"
    (t_end, rssi, snr, sf, length, data), = air.received(b)
    assert t_end == tx.t_end and data == b"\x65hello" and sf == 7
    # 7 dBm - (31.2 + 30 * 2) dB
    assert rssi == round(7 - 91.2)


def test_sender_does_not_hear_itself(air):
    a = air.node("a", 0, 0)
    air.node("b", 10, 0)
    air.tx(a, b"x")
    air.advance(30_000)
    assert air.received(a) == []


def test_below_sensitivity(air):
    a = air.node("a", 0, 0)
    b = air.node("b", 5000, 0)
    tx = air.tx(a, b"x")
    air.advance(30_000)
    assert air.outcome(tx, b) == "below_sens"
    assert air.received(b) == []


def test_capture_and_collision(air):
    rx = air.node("rx", 0, 0)
    near = air.node("near", 20, 0)
    far = air.node("far", 200, 0)
    t1 = air.tx(near, b"near")
    air.advance(5_000)
    t2 = air.tx(far, b"far")
    air.advance(60_000)
    # 30 dB apart: the strong one survives, the weak one is lost.
    assert air.outcome(t1, rx) == "ok"
    assert air.outcome(t2, rx) == "collision"


def test_equal_power_collide(air):
    rx = air.node("rx", 0, 0)
    a = air.node("a", 100, 0)
    b = air.node("b", -100, 0)
    t1 = air.tx(a, b"a")
    air.advance(1_000)
    t2 = air.tx(b, b"b")
    air.advance(60_000)
    assert air.outcome(t1, rx) == "collision"
    assert air.outcome(t2, rx) == "collision"


def test_other_sf_is_quasi_orthogonal(air):
    rx = air.node("rx", 0, 0)
    a = air.node("a", 100, 0)
    b = air.node("b", -100, 0, sf=8)
    t1 = air.tx(a, b"a")
    air.tx(b, b"b", sf=8)
    air.advance(60_000)
    assert air.outcome(t1, rx) == "ok"


def test_wrong_sf(air):
    a = air.node("a", 0, 0)
    b = air.node("b", 50, 0, sf=8)
    tx = air.tx(a, b"x", sf=7)
    air.advance(30_000)
    assert air.outcome(tx, b) == "wrong_sf"


def test_half_duplex(air):
    a = air.node("a", 0, 0)
    b = air.node("b", 50, 0)
    t1 = air.tx(a, b"a")
    air.advance(2_000)
    air.tx(b, b"b")
    air.advance(60_000)
    assert air.outcome(t1, b) == "half_duplex"


def test_wideband_jammer(air):
    from radiosim.medium import Entity

    a = air.node("a", 0, 0)
    b = air.node("b", 300, 0)
    j = air.m.add(Entity(id=0, name="jam", x=310, y=0))
    air.m.start(Transmission(id=0, src=j.id, t_start=air.t, t_end=OPEN, sf=7, power_dbm=0,
                             data=None, wideband=True))
    # b hears the jammer's energy for listen-before-talk.
    energy = [proto.parse_down(d) for addr, d in air.sent if addr == b.addr]
    assert any(t == proto.ENERGY and f[2] == proto.T_OPEN for t, f in energy)
    tx = air.tx(a, b"x")
    air.advance(30_000)
    assert air.outcome(tx, b) == "jammed"


def test_cut_link_and_isolation(air):
    a = air.node("a", 0, 0)
    b = air.node("b", 50, 0)
    c = air.node("c", 60, 0)
    air.m.set_link(a.id, b.id, 0, True, 0)
    c.isolated = True
    tx = air.tx(a, b"x")
    air.advance(30_000)
    assert air.outcome(tx, b) == "cut"
    assert air.outcome(tx, c) == "offline"


def test_random_loss(air):
    a = air.node("a", 0, 0)
    b = air.node("b", 50, 0)
    air.m.set_link(a.id, b.id, 0, False, 100)
    tx = air.tx(a, b"x")
    air.advance(30_000)
    assert air.outcome(tx, b) == "random_loss"


def test_liveness(air):
    a = air.node("a", 0, 0)
    air.advance(4_000_000)
    air.m.check_liveness()
    assert not a.connected
