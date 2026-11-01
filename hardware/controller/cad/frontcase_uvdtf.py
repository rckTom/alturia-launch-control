#!/usr/bin/env python3
"""UV DTF transfer sheet with the controller front markings.

Takes the artwork from frontcase_engraving.py and puts as many complete front
sets on one sheet as fit, upright and turned 90 degrees, then fills the space
left over with spare single labels. Each complete set goes on in one piece, so
its marks stay registered to each other; the spares are for patching a label
that did not transfer.

All marks are plain white (#ffffff) on a transparent page, for a white-ink
print on the dark case. Text is converted to paths.

Usage: frontcase_uvdtf.py [A4|A3|<w>x<h>]   (mm, default A4)
Writes frontcase_uvdtf_<sheet>.pdf, the same sheet as a transparent 300 dpi
PNG, a preview PNG on a dark background, and frontcase_uvdtf_single.png: one
set at 300 dpi on a transparent page the size of the flat front face.
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

import frontcase_engraving as fe

HERE = Path(__file__).resolve().parent
SHEETS = {"A4": (210.0, 297.0), "A3": (297.0, 420.0)}
MARGIN = 4.0   # unprinted border of the sheet; 5 lost the fifth A4 set
GAP = 5.0      # between complete sets, room to cut them apart
SPARE_GAP = 3.0
INK = "#ffffff"
PX = 96 / 25.4  # Inkscape query units per mm
DPI = 300


# ---- artwork bounding boxes ----------------------------------------------

def measure(elements):
    """Bounding box (x0, y0, x1, y1) in SVG mm of each element, via Inkscape."""
    w, h = fe.W, fe.H
    body = "\n".join(f'<g id="e{i}">{e}</g>' for i, e in enumerate(elements))
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" '
           f'width="{w}mm" height="{h}mm" viewBox="{-w / 2} {-h / 2} {w} {h}">'
           f'{body}</svg>')
    with tempfile.NamedTemporaryFile("w", suffix=".svg") as f:
        f.write(svg)
        f.flush()
        out = subprocess.run(["inkscape", f.name, "--query-all"], check=True,
                             capture_output=True, text=True).stdout
    boxes = {}
    for line in out.splitlines():
        m = re.fullmatch(r"e(\d+),([-\d.e]+),([-\d.e]+),([-\d.e]+),([-\d.e]+)",
                         line.strip())
        if m:
            x, y, bw, bh = (float(v) / PX for v in m.groups()[1:])
            x0, y0 = x - w / 2, y - h / 2
            boxes[int(m.group(1))] = (x0, y0, x0 + bw, y0 + bh)
    return [boxes[i] for i in range(len(elements))]


def union(boxes):
    return (min(b[0] for b in boxes), min(b[1] for b in boxes),
            max(b[2] for b in boxes), max(b[3] for b in boxes))


class Piece:
    """A group of artwork elements with its bounding box."""

    def __init__(self, elements, box):
        self.elements, self.box = elements, box
        self.w, self.h = box[2] - box[0], box[3] - box[1]

    def place(self, x, y, turned=False):
        """SVG group with the piece's bounding box at (x, y) on the sheet."""
        x0, y0 = self.box[:2]
        if turned:  # 90 degrees clockwise: width and height swap
            t = (f"translate({x + self.h:.3f},{y:.3f}) rotate(90) "
                 f"translate({-x0:.3f},{-y0:.3f})")
        else:
            t = f"translate({x - x0:.3f},{y - y0:.3f})"
        return f'<g transform="{t}">' + "".join(self.elements) + "</g>"


def pieces():
    """The complete set and the spare pieces cut from it."""
    els = fe.engrave()
    boxes = measure(els)
    full = Piece(els, union(boxes))

    def label(s):
        i = next(i for i, e in enumerate(els) if f">{s}<" in e)
        return Piece([els[i]], boxes[i])

    title_idx = [i for i, e in enumerate(els)
                 if ">ALTURIA<" in e or ">LAUNCH CONTROL<" in e
                 or e.startswith("<rect")]
    title = Piece([els[i] for i in title_idx], union([boxes[i] for i in title_idx]))
    ring_idx = next(i for i, e in enumerate(els) if e.startswith("<path"))
    ring = Piece([els[ring_idx]], boxes[ring_idx])
    labels = [label(s) for s in
              ("FILL", "VENT", "ARM", "FIRE", "BACK", "STATUS", "CHG")]
    return full, labels, title, ring


# ---- layout ----------------------------------------------------------------

def fit(span, size, gap):
    return max(0, int((span + gap) // (size + gap)))


def layout_sets(full, uw, uh):
    """Rows of upright sets, then rows of turned ones below. Returns the
    placements (x, y, turned) and the free rectangles left over."""
    best = None
    for up_rows in range(fit(uh, full.h, GAP) + 1):
        used = up_rows * (full.h + GAP)
        rot_rows = fit(uh - used, full.w, GAP)
        n_up, n_rot = fit(uw, full.w, GAP), fit(uw, full.h, GAP)
        total = up_rows * n_up + rot_rows * n_rot
        if best is None or total > best[0]:
            best = (total, up_rows, n_up, rot_rows, n_rot)
    _, up_rows, n_up, rot_rows, n_rot = best

    place, free = [], []
    y = 0.0
    for _ in range(up_rows):
        for c in range(n_up):
            place.append((c * (full.w + GAP), y, False))
        y += full.h + GAP
    if up_rows and n_up:
        x_end = n_up * (full.w + GAP)
        free.append((x_end, 0.0, uw - x_end, y - GAP))
    y_rot = y
    for _ in range(rot_rows):
        for c in range(n_rot):
            place.append((c * (full.h + GAP), y, True))
        y += full.w + GAP
    if rot_rows and n_rot:
        x_end = n_rot * (full.h + GAP)
        free.append((x_end, y_rot, uw - x_end, y - GAP - y_rot))
    free.append((0.0, y, uw, uh - y))
    return place, [r for r in free if r[2] > 0 and r[3] > 0]


def pack_spares(free, labels, title, ring):
    """Shelf-pack spares into the free rectangles: one label kit, a title and
    a ring first, then more label kits for as long as they fit."""
    queue = labels + [title, ring] + labels * 20
    placed = []
    for fx, fy, fw, fh in free:
        x, y, shelf = fx, fy, 0.0
        rest = []
        for p in queue:
            for turned in (False, True):
                w, h = (p.h, p.w) if turned else (p.w, p.h)
                if x + w > fx + fw + 1e-6:  # next shelf
                    nx, ny = fx, y + shelf + SPARE_GAP
                else:
                    nx, ny = x, y
                if nx + w <= fx + fw + 1e-6 and ny + h <= fy + fh + 1e-6:
                    if ny != y:
                        y, shelf = ny, 0.0
                    placed.append(p.place(nx, ny, turned))
                    x, shelf = nx + w + SPARE_GAP, max(shelf, h)
                    break
            else:
                rest.append(p)
        queue = rest
    return placed


# ---- output ----------------------------------------------------------------

def sheet_svg(sw, sh):
    full, labels, title, ring = pieces()
    uw, uh = sw - 2 * MARGIN, sh - 2 * MARGIN
    # Try both page orientations, keep the one with more complete sets
    options = []
    for w, h in ((uw, uh), (uh, uw)):
        place, free = layout_sets(full, w, h)
        options.append((len(place), w, h, place, free))
    n, uw, uh, place, free = max(options, key=lambda o: o[0])
    sw, sh = uw + 2 * MARGIN, uh + 2 * MARGIN

    groups = [full.place(x, y, t) for x, y, t in place]
    spares = pack_spares(free, labels, title, ring)
    svg = "\n".join([
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<svg xmlns="http://www.w3.org/2000/svg" '
        f'width="{sw}mm" height="{sh}mm" viewBox="0 0 {sw} {sh}">',
        '<title>Alturia Launch Control - controller front, UV DTF</title>',
        f'<g transform="translate({MARGIN},{MARGIN})" fill="{INK}" stroke="none">',
        *groups, *spares,
        '</g>',
        '</svg>',
    ]) + "\n"
    return svg, n, len(spares), (sw, sh)


def single_svg():
    """One set on a page the size of the flat face, origin at its centre."""
    w, h = fe.W, fe.H
    return "\n".join([
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<svg xmlns="http://www.w3.org/2000/svg" '
        f'width="{w}mm" height="{h}mm" viewBox="{-w / 2} {-h / 2} {w} {h}">',
        f'<g fill="{INK}" stroke="none">', *fe.engrave(), '</g>',
        '</svg>',
    ]) + "\n"


def png(src, out, **kw):
    """Transparent PNG at DPI unless overridden."""
    args = [f"--export-dpi={kw.get('dpi', DPI)}"]
    if "background" in kw:
        args += [f"--export-background={kw['background']}",
                 "--export-background-opacity=1"]
    subprocess.run(["inkscape", str(src), *args, f"--export-filename={out}"],
                   check=True, capture_output=True)


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "A4"
    if arg.upper() in SHEETS:
        name, (sw, sh) = arg.upper(), SHEETS[arg.upper()]
    else:
        sw, sh = (float(v) for v in arg.lower().split("x"))
        name = f"{sw:g}x{sh:g}"
    svg, n, n_spare, (pw, ph) = sheet_svg(sw, sh)

    src = HERE / f"frontcase_uvdtf_{name}.svg"
    pdf = HERE / f"frontcase_uvdtf_{name}.pdf"
    sheet_png = HERE / f"frontcase_uvdtf_{name}.png"
    preview = HERE / f"frontcase_uvdtf_{name}_preview.png"
    src.write_text(svg)
    subprocess.run(["inkscape", str(src), "--export-type=pdf",
                    "--export-text-to-path", f"--export-filename={pdf}"],
                   check=True, capture_output=True)
    png(src, sheet_png)
    png(src, preview, dpi=150, background="#1f2023")
    src.unlink()

    single = HERE / "frontcase_uvdtf_single.svg"
    single.write_text(single_svg())
    png(single, HERE / "frontcase_uvdtf_single.png")
    single.unlink()
    print(f"{pdf.name}: {pw:g} x {ph:g} mm, {n} complete sets, "
          f"{n_spare} spare pieces; {sheet_png.name}, {preview.name}, "
          "frontcase_uvdtf_single.png")


if __name__ == "__main__":
    main()
