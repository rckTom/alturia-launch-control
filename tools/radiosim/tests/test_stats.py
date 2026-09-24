from radiosim import stats
from radiosim.medium import Transmission


def test_busy_union():
    assert stats.busy_us([(0, 10), (5, 15), (20, 30)], 0, 100) == 25
    assert stats.busy_us([(0, 10), (5, 15), (20, 30)], 8, 25) == 12
    assert stats.busy_us([], 0, 100) == 0


def test_utilization_of_a_poll_cycle(air):
    """A 250 ms tick with a 20 ms poll and two 36 ms replies."""
    ctrl = air.node("ctrl", 0, 0)
    sats = [air.node("satA", 50, 0), air.node("satB", 0, 50)]
    for _ in range(40):  # 10 s
        air.tx(ctrl, b"\x65\x34\x12\x00\xff" + bytes(20), airtime_us=20_000)
        air.advance(26_000)
        for i, s in enumerate(sats):
            air.tx(s, b"\x64\x34\x12" + bytes([i + 1, 0]) + bytes(26), airtime_us=36_000)
            air.advance(43_000)
        air.advance(250_000 - 26_000 - 86_000)
    st = stats.compute(air.m)
    assert abs(st["util"]["10"] - (20 + 72) / 250) < 0.01
    (poll,) = st["polls"]
    assert poll["net_tag"] == "1234" and abs(poll["period_ms"] - 250) < 1
    assert poll["replies"] == {"1": 1.0, "2": 1.0}
    assert st["sources"][str(ctrl.id)]["frames_10s"] == 40
