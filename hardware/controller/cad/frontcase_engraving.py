#!/usr/bin/env python3
"""Front engraving for the Alturia Launch Control handheld controller.

Writes frontcase_engraving_src.svg (live text) and, via Inkscape, the
laser-ready frontcase_engraving.svg (text converted to paths). Same style as
the satellite panel (hardware/satellite/cad/frontpanel_engraving.py).

Coordinates are mm in FrontCase.FCStd's Body frame, seen from the front
(+z): SVG x = body x, SVG y = -body y. The origin is the centre of the flat
top face (z = 17), which is 69.92 x 158.92 mm with R7.46 corners; its edge
then rolls over into the side walls. Hole positions are read from the Body
solid (z = 13 face wires).

Button assignment follows the 2026-10-01 mockup: FILL / VENT on the upper
row, ARM / FIRE on the lower row, FIRE bottom right. The two Ø1.8 LED holes
between display and D-pad are D401 STATUS (green, left) and D201 CHG (red);
PCB -> body is x - 99.5, -(y - 101.5).

Every mark is at least MIN_LINE wide, the minimum line width for a UV DTF
transfer (frontcase_uvdtf.py): text() checks the thinnest stroke of each
label and refuses anything thinner.

Layers:
  engrave  - black fills, the only thing the laser should mark
  outline  - flat-face contour and holes as red hairlines, for alignment only
"""

import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
W, H, R_CORNER = 69.92, 158.92, 7.46
CLEAR = 2.0  # minimum distance from any hole edge
MIN_LINE = 0.5  # UV DTF minimum line width
LINE = 0.6      # width of the rules and the D-pad ring

# ---- holes (body frame, y up) -------------------------------------------

DELOCK_R = 6.25        # Ø12.5 hole
BEZEL_R = 8.75         # Ø17.5 Delock bezel, covers the face around the hole
FILL, VENT = (-18.75, -37.0), (18.75, -37.0)
ARM, FIRE = (-18.75, -62.0), (18.75, -62.0)

DPAD = (-12.5, -2.0)   # ENTER, Ø9 round
DPAD_PITCH = 12.5
KEY_HOLE = 8.0         # arrow keycap holes, square
KEYCAP = 9.5           # arrow keycap footprint
ROUND_R = 4.5
BACK = (25.0, -2.0)

WINDOW = (-31.15, 24.64, 31.15, 63.42)   # display opening x0, y0, x1, y1

LED_R = 0.9            # Ø1.8 light holes
LED_STATUS = (9.75, 15.0)   # D401, green
LED_CHG = (18.0, 15.0)      # D201, red

# ---- fonts --------------------------------------------------------------

TITLE_FONT = "font-family:'Noto Sans';font-weight:700"
LABEL_FONT = "font-family:'Noto Sans';font-weight:800"
STRONG_FONT = "font-family:'Noto Sans';font-weight:900"
CAP = 0.714  # cap height / em for Noto Sans
# Thinnest stroke / em per weight: the horizontals (E arms, H bar, O top),
# measured on the installed NotoSans-*.ttf.
THIN = {600: 0.107, 700: 0.124, 800: 0.138, 900: 0.155}


def text(x, y, s, cap, style=LABEL_FONT, anchor="middle", spacing=0.0):
    """Text whose cap height is `cap` mm, vertically centred on body y."""
    size = cap / CAP
    weight = int(style.split("font-weight:")[1])
    if size * THIN[weight] < MIN_LINE:
        raise ValueError(f"{s!r}: {size * THIN[weight]:.2f} mm strokes, "
                         f"under the {MIN_LINE} mm minimum")
    if anchor == "middle":
        x += spacing / 2  # letter-spacing also trails the last glyph
    elif anchor == "end":
        x += spacing
    return (f'<text x="{x:.3f}" y="{-y + cap / 2:.3f}" '
            f'style="{style};font-size:{size:.3f}px;'
            f'letter-spacing:{spacing:.3f}px;text-anchor:{anchor}">{s}</text>')


def rect(x0, y0, x1, y1, r=0.0):
    """Rectangle from body-frame corners."""
    return (f'<rect x="{x0:.3f}" y="{-y1:.3f}" width="{x1 - x0:.3f}" '
            f'height="{y1 - y0:.3f}" rx="{r:.3f}"/>')


def hline(x0, x1, y, w=LINE):
    return rect(x0, y - w / 2, x1, y + w / 2, w / 2)


def ring(cx, cy, r_in, r_out):
    def circ(r, sweep):
        return (f"M{cx + r:.3f},{-cy:.3f} A{r:.3f},{r:.3f} 0 1 {sweep} "
                f"{cx - r:.3f},{-cy:.3f} A{r:.3f},{r:.3f} 0 1 {sweep} "
                f"{cx + r:.3f},{-cy:.3f} Z")
    return f'<path d="{circ(r_out, 1)} {circ(r_in, 0)}"/>'


# ---- artwork ------------------------------------------------------------

def engrave():
    e = []
    SMALL = 2.8   # label cap height, the least that gives MIN_LINE strokes
    BIG = 3.4     # ARM / FIRE

    # Title in the strip above the display
    e.append(text(0.0, 73.4, "ALTURIA", 5.0, style=TITLE_FONT, spacing=2.0))
    e.append(text(0.0, 67.4, "LAUNCH CONTROL", SMALL, spacing=0.4))
    e.append(hline(-30.0, -23.0, 67.4))
    e.append(hline(23.0, 30.0, 67.4))

    # D-pad: ring around the cluster, clear of the keycap corners
    e.append(ring(*DPAD, 19.0, 19.0 + LINE))
    e.append(text(BACK[0], BACK[1] - ROUND_R - CLEAR - SMALL / 2, "BACK",
                  SMALL, spacing=0.3))

    # LEDs: labels above, in the free strip under the display. The holes are
    # only 8.25 mm apart, so each label ends over its own LED and runs
    # outward; centred they read as one phrase.
    yl = LED_STATUS[1] + LED_R + CLEAR + SMALL / 2
    e.append(text(LED_STATUS[0] + LED_R, yl, "STATUS", SMALL, anchor="end",
                  spacing=0.15))
    e.append(text(LED_CHG[0] - LED_R, yl, "CHG", SMALL, anchor="start",
                  spacing=0.15))

    # Upper row labelled above, lower row below, so the gap between the rows
    # separates the propellant valves from the launch pair.
    ya = FILL[1] + BEZEL_R + CLEAR + SMALL / 2
    e.append(text(FILL[0], ya, "FILL", SMALL, spacing=0.3))
    e.append(text(VENT[0], ya, "VENT", SMALL, spacing=0.3))
    yb = ARM[1] - BEZEL_R - CLEAR - BIG / 2
    e.append(text(ARM[0], yb, "ARM", BIG, style=STRONG_FONT, spacing=0.5))
    e.append(text(FIRE[0], yb, "FIRE", BIG, style=STRONG_FONT, spacing=0.5))
    return e


def outline():
    o = [rect(-W / 2, -H / 2, W / 2, H / 2, R_CORNER), rect(*WINDOW)]
    for c in (FILL, VENT, ARM, FIRE):
        o.append(f'<circle cx="{c[0]}" cy="{-c[1]}" r="{DELOCK_R}"/>')
        o.append(f'<circle cx="{c[0]}" cy="{-c[1]}" r="{BEZEL_R}" '
                 'stroke-dasharray="0.6 0.6"/>')
    for c in (DPAD, BACK):
        o.append(f'<circle cx="{c[0]}" cy="{-c[1]}" r="{ROUND_R}"/>')
    for c in (LED_STATUS, LED_CHG):
        o.append(f'<circle cx="{c[0]}" cy="{-c[1]}" r="{LED_R}"/>')
    for dx, dy in ((0, 1), (0, -1), (-1, 0), (1, 0)):
        x, y = DPAD[0] + dx * DPAD_PITCH, DPAD[1] + dy * DPAD_PITCH
        h = KEY_HOLE / 2
        o.append(rect(x - h, y - h, x + h, y + h))
    return o


def preview_svg():
    """Light marks on the dark case, holes and Delock bezels filled."""
    face, mark, hole, bezel = "#1f2023", "#e9e7e2", "#f4f3f0", "#8a8c90"
    o = outline()
    bezels = [x.replace('stroke-dasharray="0.6 0.6"', f'fill="{bezel}"')
              for x in o if "stroke-dasharray" in x]
    holes = [x for x in o[1:] if "stroke-dasharray" not in x]
    body = [o[0].replace("<rect ", f'<rect fill="{face}" ')] + holes + bezels
    parts = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<svg xmlns="http://www.w3.org/2000/svg" '
        f'width="{W}mm" height="{H}mm" viewBox="{-W / 2} {-H / 2} {W} {H}">',
        f'<g fill="{hole}" stroke="none">', *body, '</g>',
        f'<g fill="{mark}" stroke="none">', *engrave(), '</g>',
        '</svg>',
    ]
    return "\n".join(parts) + "\n"


def svg(show_engrave=True, show_outline=True):
    parts = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<svg xmlns="http://www.w3.org/2000/svg" '
        'xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" '
        f'width="{W}mm" height="{H}mm" viewBox="{-W / 2} {-H / 2} {W} {H}">',
        '<title>Alturia Launch Control - controller front engraving</title>',
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
    src = HERE / "frontcase_engraving_src.svg"
    out = HERE / "frontcase_engraving.svg"
    src.write_text(svg())
    subprocess.run(["inkscape", str(src), "--export-text-to-path",
                    "--export-plain-svg", f"--export-filename={out}"],
                   check=True, capture_output=True)
    prev_svg = HERE / "frontcase_engraving_preview.svg"
    prev = HERE / "frontcase_engraving_preview.png"
    prev_svg.write_text(preview_svg())
    subprocess.run(["inkscape", str(prev_svg), "--export-dpi=400",
                    "--export-background=#ffffff",
                    f"--export-filename={prev}"], check=True, capture_output=True)
    prev_svg.unlink()
    print(f"wrote {src.name}, {out.name}, {prev.name}")


if __name__ == "__main__":
    main()
