#!/usr/bin/env python3
"""Front panel engraving for the Alturia Launch Control satellite.

Writes frontpanel_engraving_src.svg (live text) and, via Inkscape, the
laser-ready frontpanel_engraving.svg (text converted to paths).

Coordinates are mm, origin at the panel centre, x right, y down, as seen
from the FRONT of the panel, the way up it is mounted. frontpanel_outline.svg
is a TechDraw view from behind (the PCB sits on +z in front_plate_assembly,
parts point to -z through the panel) and upside down relative to the
mounted panel, so every y from that drawing is negated here.

Channel numbers follow the hardware (J23 = PyroStage1 = LED_CHN_1), which
puts channel 1 on the right.

Layers:
  engrave  - black fills, the only thing the laser should mark
  outline  - panel contour and holes as red hairlines, for alignment only
"""

import math
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
W, H, R_CORNER = 220.0, 117.0, 4.5
CLEAR = 2.0  # minimum distance from any hole edge

# ---- holes (front view) -------------------------------------------------

MOUNT_R = 1.7
MOUNT = [(sx * 105, sy * 53.5) for sx in (-1, 1) for sy in (-1, 1)] + \
        [(sx * 35, sy * 53.5) for sx in (-1, 1) for sy in (-1, 1)] + \
        [(-105, 0), (105, 0)]

PITCH = 21.0
JACK_R = 3.8
LED_R = 3.0
# channel k (0..5): J23, J22, J18, J19, J20, J21 = PyroStage1..6
JACK_NEG = [(43.5 - PITCH * k, 27.5) for k in range(6)]     # pad 1, low side
JACK_POS = [(35.475 - PITCH * k, 37.0) for k in range(6)]   # pad 2, PYRO_H
CH_LED = [(38.75 - PITCH * k, -19.77) for k in range(6)]    # D17..D22
CH_X = [(n[0] + p[0]) / 2 for n, p in zip(JACK_NEG, JACK_POS)]

LED_STATUS = (-77.25, -19.77)  # D23
LED_ARMED = (-77.25, -29.77)   # D4, lit from V_PYRO_ARMED
SMA, SMA_R = (-86.44, -38.9), 3.5

KEY, KEY_R = (71.0, 33.7), 11.15
KEY_NOTCH = (69.9, 72.1, 20.75)    # x0, x1, y of notch end (above circle)
ROCKER = (60.0, -47.5, 82.0, -17.3)  # x0, y0, x1, y1
FUSE = (89.5, -44.65, 100.0, -20.15)  # x0, y0, x1, y1, next to the rocker
BUTTON, BUTTON_R = (71.0, 2.65), 6.0  # continuity test (CONT_TEST_REQ)

# Key positions (SVG angle, 0 = 3 o'clock, -90 = 12 o'clock): OFF at
# 12 o'clock, in line with the anti-rotation notch, ON at 3 o'clock.
KEY_OFF_DEG, KEY_ON_DEG = -90.0, 0.0

# ---- fonts --------------------------------------------------------------

LABEL_FONT = "font-family:'Noto Sans';font-weight:600"
TITLE_FONT = "font-family:'Noto Sans';font-weight:700"
NUM_FONT = "font-family:'Nimbus Sans Narrow';font-weight:700"
CAP = 0.714  # cap height / em for Noto Sans
NUM_CAP = 0.72


def text(x, y, s, cap, style=LABEL_FONT, anchor="middle", spacing=0.0,
         cap_ratio=CAP):
    """Text whose cap height is `cap` mm, vertically centred on y."""
    size = cap / cap_ratio
    return (f'<text x="{x:.3f}" y="{y + cap / 2:.3f}" '
            f'style="{style};font-size:{size:.3f}px;'
            f'letter-spacing:{spacing:.3f}px;text-anchor:{anchor}">{s}</text>')


def rect(x0, y0, x1, y1, r=0.0):
    return (f'<rect x="{x0:.3f}" y="{y0:.3f}" width="{x1 - x0:.3f}" '
            f'height="{y1 - y0:.3f}" rx="{r:.3f}"/>')


def hline(x0, x1, y, w=0.4):
    return rect(x0, y - w / 2, x1, y + w / 2, w / 2)


def vline(x, y0, y1, w=0.4):
    return rect(x - w / 2, y0, x + w / 2, y1, w / 2)


def ring(cx, cy, r_in, r_out):
    def circ(r, sweep):
        return (f"M{cx + r:.3f},{cy:.3f} A{r:.3f},{r:.3f} 0 1 {sweep} "
                f"{cx - r:.3f},{cy:.3f} A{r:.3f},{r:.3f} 0 1 {sweep} "
                f"{cx + r:.3f},{cy:.3f} Z")
    return f'<path d="{circ(r_out, 1)} {circ(r_in, 0)}"/>'


def arc_band(cx, cy, r_in, r_out, a0, a1):
    """Annular sector from angle a0 to a1 (degrees, SVG orientation)."""
    def pt(r, a):
        t = math.radians(a)
        return cx + r * math.cos(t), cy + r * math.sin(t)
    large = 1 if abs(a1 - a0) > 180 else 0
    p0, p1 = pt(r_out, a0), pt(r_out, a1)
    q1, q0 = pt(r_in, a1), pt(r_in, a0)
    return (f'<path d="M{p0[0]:.3f},{p0[1]:.3f} '
            f'A{r_out:.3f},{r_out:.3f} 0 {large} 1 {p1[0]:.3f},{p1[1]:.3f} '
            f'L{q1[0]:.3f},{q1[1]:.3f} '
            f'A{r_in:.3f},{r_in:.3f} 0 {large} 0 {q0[0]:.3f},{q0[1]:.3f} Z"/>')


def radial(cx, cy, r0, r1, a, w=0.5):
    t = math.radians(a)
    dx, dy = math.cos(t), math.sin(t)
    nx, ny = -dy * w / 2, dx * w / 2
    pts = [(cx + dx * r0 + nx, cy + dy * r0 + ny),
           (cx + dx * r1 + nx, cy + dy * r1 + ny),
           (cx + dx * r1 - nx, cy + dy * r1 - ny),
           (cx + dx * r0 - nx, cy + dy * r0 - ny)]
    return '<path d="M' + " L".join(f"{x:.3f},{y:.3f}" for x, y in pts) + ' Z"/>'


def plus(cx, cy, size=3.0, w=0.6):
    return hline(cx - size / 2, cx + size / 2, cy, w) + \
        vline(cx, cy - size / 2, cy + size / 2, w)


def minus(cx, cy, size=3.0, w=0.6):
    return hline(cx - size / 2, cx + size / 2, cy, w)


def power_icon(cx, cy, r=2.6, w=0.55):
    """IEC 60417-5009 standby/power symbol."""
    gap = 40.0
    return arc_band(cx, cy, r - w / 2, r + w / 2, -90 + gap, 270 - gap) + \
        vline(cx, cy - r - 0.6, cy - 0.3, w)


def polar(c, r, a):
    t = math.radians(a)
    return c[0] + r * math.cos(t), c[1] + r * math.sin(t)


# ---- artwork ------------------------------------------------------------

def engrave():
    e = []
    SMALL = 2.6     # label cap height
    RX = 93.0       # right column label centre
    CX = (JACK_POS[5][0] - JACK_R + JACK_NEG[0][0] + JACK_R) / 2  # channel block centre

    # Right column: main power and fuse side by side, labelled in one row
    # underneath; continuity test button; key
    ry = -12.0
    rx = (ROCKER[0] + ROCKER[2]) / 2
    e.append(power_icon(rx - 9.0, ry, r=2.0, w=0.5))
    e.append(text(rx - 4.9, ry, "POWER", SMALL, anchor="start", spacing=0.3))
    e.append(text((FUSE[0] + FUSE[2]) / 2, ry, "FUSE", SMALL, spacing=0.3))

    # centred in the gap between the button and the (105, 0) mounting hole
    bx = (BUTTON[0] + BUTTON_R + 105.0 - MOUNT_R) / 2
    e.append(text(bx, BUTTON[1] - 2.1, "CONTINUITY", 1.8, spacing=0.2))
    e.append(text(bx, BUTTON[1] + 2.1, "TEST", SMALL, spacing=0.3))

    e.append(text(KEY[0], KEY[1] + KEY_R + CLEAR + 3.5, "PYRO ENABLE", SMALL,
                  spacing=0.3))
    e.append(arc_band(*KEY, 15.3, 15.7, KEY_OFF_DEG, KEY_ON_DEG))
    for a in (KEY_OFF_DEG, KEY_ON_DEG):
        e.append(radial(*KEY, 15.0, 17.5, a, 0.6))
    x, y = polar(KEY, 17.5, KEY_OFF_DEG)    # 12 o'clock: label above
    e.append(text(x, y - 1.0 - SMALL / 2, "OFF", SMALL, spacing=0.2))
    x, y = polar(KEY, 17.5, KEY_ON_DEG)     # 3 o'clock: label right
    e.append(text(x + 1.2, y, "ON", SMALL, anchor="start", spacing=0.2))

    # Separator between channels and controls
    e.append(vline(52.5, -44.0, 46.0, 0.35))

    # Channels
    for k in range(6):
        nx, ny = JACK_NEG[k]
        px, py = JACK_POS[k]
        lx, ly = CH_LED[k]
        e.append(plus(px, py + JACK_R + CLEAR + 2.3))
        e.append(minus(nx, ny - JACK_R - CLEAR - 2.3))
        e.append(text((CH_X[k] + lx) / 2, 4.0, str(k + 1), 9.0,
                      style=NUM_FONT, cap_ratio=NUM_CAP))
        e.append(ring(lx, ly, LED_R + 2.2, LED_R + 2.6))
        if k < 5:
            e.append(vline(CH_X[k] - PITCH / 2, -6.0, 44.0, 0.35))

    e.append(text(CX, -10.5, "CONTINUITY", 2.0, spacing=0.8))

    # Left column
    lx0 = LED_STATUS[0] - LED_R - CLEAR - 2.0
    e.append(text(lx0, LED_STATUS[1], "STATUS", SMALL, anchor="end",
                  spacing=0.3))
    e.append(text(lx0, LED_ARMED[1], "ARMED", SMALL, anchor="end",
                  spacing=0.3))
    e.append(text(SMA[0] - SMA_R - 3.0, SMA[1], "ANT", SMALL, anchor="end",
                  spacing=0.3))

    # Unit ID field (write-on / sticker area)
    box = (-99.0, 26.0, -79.0, 40.0)
    e.append(text((box[0] + box[2]) / 2, box[1] - 4.5, "UNIT", SMALL,
                  spacing=0.9))
    e.append(f'<path fill-rule="evenodd" d="'
             f'M{box[0]},{box[1] + 1.5} a1.5,1.5 0 0 1 1.5,-1.5 H{box[2] - 1.5} '
             f'a1.5,1.5 0 0 1 1.5,1.5 V{box[3] - 1.5} a1.5,1.5 0 0 1 -1.5,1.5 '
             f'H{box[0] + 1.5} a1.5,1.5 0 0 1 -1.5,-1.5 Z '
             f'M{box[0] + 0.4},{box[1] + 1.5} V{box[3] - 1.5} '
             f'a1.1,1.1 0 0 0 1.1,1.1 H{box[2] - 1.5} a1.1,1.1 0 0 0 1.1,-1.1 '
             f'V{box[1] + 1.5} a1.1,1.1 0 0 0 -1.1,-1.1 H{box[0] + 1.5} '
             f'a1.1,1.1 0 0 0 -1.1,1.1 Z"/>')

    # Title
    tx = CX
    e.append(text(tx, -41.0, "ALTURIA", 8.0, style=TITLE_FONT, spacing=3.2))
    e.append(text(tx, -32.0, "LAUNCH CONTROL", 2.8, spacing=1.6))
    e.append(hline(tx - 52.0, tx - 32.0, -32.0, 0.35))
    e.append(hline(tx + 32.0, tx + 52.0, -32.0, 0.35))
    return e


def outline():
    o = []
    x0, y0 = -W / 2, -H / 2
    o.append(rect(x0, y0, x0 + W, y0 + H, R_CORNER))
    for c in MOUNT:
        o.append(f'<circle cx="{c[0]}" cy="{c[1]}" r="{MOUNT_R}"/>')
    for c in JACK_NEG + JACK_POS:
        o.append(f'<circle cx="{c[0]}" cy="{c[1]}" r="{JACK_R}"/>')
    for c in CH_LED + [LED_STATUS, LED_ARMED]:
        o.append(f'<circle cx="{c[0]}" cy="{c[1]}" r="{LED_R}"/>')
    o.append(f'<circle cx="{SMA[0]}" cy="{SMA[1]}" r="{SMA_R}"/>')
    # key with anti-rotation notch above
    nx0, nx1, ny = KEY_NOTCH
    kx, ky = KEY
    yj = ky - math.sqrt(KEY_R ** 2 - (kx - nx0) ** 2)
    o.append(f'<path d="M{nx1},{yj:.3f} A{KEY_R},{KEY_R} 0 1 1 {nx0},{yj:.3f} '
             f'V{ny} H{nx1} Z"/>')
    o.append(f'<circle cx="{BUTTON[0]}" cy="{BUTTON[1]}" r="{BUTTON_R}"/>')
    o.append(rect(*ROCKER))
    o.append(rect(*FUSE))
    return o


def svg(show_engrave=True, show_outline=True):
    x0, y0 = -W / 2, -H / 2
    parts = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<svg xmlns="http://www.w3.org/2000/svg" '
        'xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
        f'width="{W}mm" height="{H}mm" viewBox="{x0} {y0} {W} {H}">',
        '<title>Alturia Launch Control - satellite front panel engraving</title>',
    ]
    if show_outline:
        parts.append('<g id="outline" inkscape:groupmode="layer" '
                     'inkscape:label="outline (do not engrave)" '
                     'fill="none" stroke="#ff0000" stroke-width="0.1">')
        parts += outline()
        parts.append('</g>')
    if show_engrave:
        parts.append('<g id="engrave" inkscape:groupmode="layer" '
                     'inkscape:label="engrave" fill="#000000" stroke="none">')
        parts += engrave()
        parts.append('</g>')
    parts.append('</svg>')
    return "\n".join(parts) + "\n"


def main():
    src = HERE / "frontpanel_engraving_src.svg"
    out = HERE / "frontpanel_engraving.svg"
    src.write_text(svg())
    subprocess.run(["inkscape", str(src), "--export-text-to-path",
                    "--export-plain-svg", f"--export-filename={out}"],
                   check=True, capture_output=True)
    print(f"wrote {src.name}, {out.name}")


if __name__ == "__main__":
    main()
