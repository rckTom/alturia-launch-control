"""Channel statistics over the recent history of the medium."""

from __future__ import annotations

from collections import Counter, defaultdict
from typing import Iterable

from .medium import OPEN, Medium, Transmission

WINDOWS_S = (1, 10, 60)
LINK_WINDOW_S = 30
# A poll's replies arrive within this long after it (slots of up to 255 ms).
POLL_REPLY_WINDOW_US = 300_000


def busy_us(intervals: Iterable[tuple[int, int]], lo: int, hi: int) -> int:
    """Length of the union of @p intervals within [lo, hi)."""
    clipped = sorted((max(a, lo), min(b, hi)) for a, b in intervals if a < hi and b > lo)
    total = 0
    cur_a = cur_b = None
    for a, b in clipped:
        if cur_b is None or a > cur_b:
            if cur_b is not None:
                total += cur_b - cur_a
            cur_a, cur_b = a, b
        else:
            cur_b = max(cur_b, b)
    if cur_b is not None:
        total += cur_b - cur_a
    return total


def _end(t: Transmission, now: int) -> int:
    return now if t.t_end == OPEN else t.t_end


def compute(m: Medium) -> dict:
    now = m.clock()
    longest = max(WINDOWS_S) * 1_000_000
    recent = [t for t in reversed(m.history.values()) if _end(t, now) > now - longest]
    # Continuous jammers may have started long ago.
    recent += [t for t in m.active if t.t_end == OPEN and t not in recent]

    frames = [t for t in recent if t.data is not None]
    energy = [t for t in recent if t.data is None]

    util = {}
    jam = {}
    for w in WINDOWS_S:
        lo = now - w * 1_000_000
        util[w] = busy_us(((t.t_start, _end(t, now)) for t in frames), lo, now) / (w * 1e6)
        jam[w] = busy_us(((t.t_start, _end(t, now)) for t in energy), lo, now) / (w * 1e6)

    # Per transmitter, over 10 s.
    lo10 = now - 10_000_000
    per_src: dict[int, dict] = defaultdict(lambda: {"frames": 0, "airtime_us": 0})
    for t in frames:
        if t.t_start >= lo10:
            s = per_src[t.src]
            s["frames"] += 1
            s["airtime_us"] += _end(t, now) - t.t_start
    sources = {
        src: {
            "frames_10s": s["frames"],
            "airtime_pct": round(100.0 * s["airtime_us"] / 10e6, 2),
        }
        for src, s in per_src.items()
    }

    outcomes: Counter = Counter()
    for t in frames:
        if t.t_start >= lo10 and t.results:
            for r in t.results:
                outcomes[r["outcome"]] += 1

    # Per link, over LINK_WINDOW_S: of the frames a node could have heard
    # (on its SF, while it was up), how many it did.
    lo_link = now - LINK_WINDOW_S * 1_000_000
    links: dict[tuple[int, int], list[int]] = defaultdict(lambda: [0, 0])
    for t in frames:
        if t.t_start < lo_link or not t.results:
            continue
        for r in t.results:
            if r["outcome"] in ("offline", "wrong_sf"):
                continue
            c = links[(t.src, r["node"])]
            c[1] += 1
            if r["outcome"] == "ok":
                c[0] += 1
    link_list = [
        {"src": a, "dst": b, "ok": c[0], "total": c[1], "pdr": round(c[0] / c[1], 3)}
        for (a, b), c in links.items()
        if c[1]
    ]

    return {
        "type": "stats",
        "now": m.rel(now),
        "util": {str(w): round(v, 4) for w, v in util.items()},
        "jam": {str(w): round(v, 4) for w, v in jam.items()},
        "sources": {str(k): v for k, v in sources.items()},
        "outcomes_10s": dict(outcomes),
        "links": link_list,
        "polls": poll_stats(frames, now),
    }


def poll_stats(frames: list[Transmission], now: int) -> list[dict]:
    """Per controller network, over 10 s: poll rate and replies per slot.

    Replies are sealed frames of the poll's network (net_tag) from a
    satellite slot within POLL_REPLY_WINDOW_US of the poll. The poll's mask
    is encrypted, so the ratio is replies per poll, not per addressed poll.
    """
    lo = now - 10_000_000
    polls: dict[str, list[Transmission]] = defaultdict(list)
    replies: dict[str, list[Transmission]] = defaultdict(list)
    for t in frames:
        if t.t_start < lo:
            continue
        info = t.info or {}
        tag = info.get("net_tag")
        if tag is None:
            continue
        if info.get("kind") == "POLL":
            polls[tag].append(t)
        elif info.get("src", 0) != 0 and info.get("kind") in ("DATA", "GROUP_ACK", "JOIN_ACK"):
            replies[tag].append(t)

    out = []
    for tag, ps in polls.items():
        ps.sort(key=lambda t: t.t_start)
        slots: Counter = Counter()
        for r in replies.get(tag, []):
            if any(0 < r.t_start - p.t_end < POLL_REPLY_WINDOW_US for p in ps):
                slots[r.info["src"]] += 1
        period_ms = None
        if len(ps) > 1:
            period_ms = round((ps[-1].t_start - ps[0].t_start) / (len(ps) - 1) / 1000, 1)
        out.append({
            "net_tag": tag,
            "ctrl": ps[-1].src,
            "polls": len(ps),
            "period_ms": period_ms,
            "replies": {str(s): round(n / len(ps), 3) for s, n in sorted(slots.items())},
        })
    return out
