"""Sealed D-pad: hard caps over a printed TPU membrane.

Run with the FreeCAD GUI (colours and links need it), e.g. headless:
    xvfb-run -a freecad hardware/controller/cad/dpad_membrane.py
It writes DpadMembrane.FCStd next to this file and, if DPAD_RENDER is set to a
directory, preview PNGs into it. DPAD_STL=<dir> also exports the printed parts.

Frame: FrontCase.FCStd coordinates. z = 13 is the front face, the panel is
z 10.5..13, the controller PCB front face is z 4.2 (6.3 mm standoffs) and the
TS-1187A plungers top out at z 5.7.

Stack, front to back:
  cap      PETG/PLA, through the panel hole, flange under the panel, Ø3 stem
  frame    2.1 mm pad on the panel's inner face, one well per key; meant to be
           merged into FrontCase (FramePad here is the solid to add)
  membrane TPU sheet, flat on the front, hub + nub per key on the back
  retainer PETG plate, screwed to the frame with 4× M2×5 ISO 7380 (same
           screws as the PCB), clamps the membrane

The web of each key is the free sheet between the hub (r HUB_R) and the well
edge (WELL), about 4 mm. Its stiffness goes with WEB_T³, so WEB_T is the knob
for the feel. The stem pre-deflects the web by PRELOAD so the cap sits firm
against the panel; the nub then just touches the plunger.

Water that gets past the caps stays in the wells above the membrane. A
slanted lip on the membrane's back, around all five wells, is squeezed into a
groove in the retainer and seals. Splash-proof, not submersible.

hardware/controller/sim/dpad_membrane_fem checks key force and seal force in
CalculiX, reading the constants below.
"""
import os

import FreeCAD as App
import Part
from FreeCAD import Vector as V, Placement as Pl

try:
    import FreeCADGui as Gui
    GUI = App.GuiUp
except ImportError:
    GUI = False

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "DpadMembrane.FCStd")
FRONTCASE = os.path.join(HERE, "FrontCase.FCStd")
SWITCH_STEP = os.path.normpath(os.path.join(
    HERE, "../controller/3d/SW_Push_1P1T_XKB_TS-1187A.step"))

# ---- case (from FrontCase / controller.kicad_pcb) -------------------------
PANEL_OUT = 13.0
PANEL_IN = 10.5
PCB_FRONT = 4.2
PCB_T = 1.6
SW_TOP = PCB_FRONT + 1.5        # TS-1187A, Ø2.16 plunger
CENTER = (-12.5, -2.0)          # Enter = SW5
PITCH = 12.5
HOLE_SQ = 8.0                   # panel holes, Sketch005
HOLE_RD = 9.0
KEYS = [("Up", (0, 1), "sq"), ("Down", (0, -1), "sq"),
        ("Left", (-1, 0), "sq"), ("Right", (1, 0), "sq"),
        ("Enter", (0, 0), "rd")]

# ---- cap ------------------------------------------------------------------
CAP_CLR = 0.2                   # per side in the panel hole
CAP_PROUD = 1.2                 # above the front face
CAP_R = 0.4                     # top edge round
FLANGE_OVER = 0.8               # flange overlap under the panel, per side
FLANGE_T = 0.8
STEM_D = 3.0

# ---- frame / membrane / retainer --------------------------------------------
WELL = 5.65                     # well half-width / radius; 1.2 mm rib between keys
WELL_R = 1.5                    # corner radius of the square wells
MEM_TOP = 8.4                   # frame underside = membrane front
WEB_T = 0.4                     # 2 layers at 0.2
HUB_R = 1.6
HUB_T = 0.6
NUB_D = 2.4
PRELOAD = 0.1                   # stem pushes the web down this far at rest
OUTLINE = 21.5                  # half size of the frame/retainer square
OUTLINE_R = 3.0
MEM_INSET = 0.25                # membrane outline inside the retainer
RET_T = 2.4
RIB_H = 1.0                     # stiffening ribs under the retainer, 0 = none
RIB_W = 1.5
# seal: a slanted TPU lip on the membrane underside, squeezed into a groove in
# the retainer. A flat crush ridge into the trapped 0.4 mm sheet needed ~15 N
# per mm of ridge (sim/dpad_membrane_fem/seal.py); a lip bends instead.
LIP_OFF = 1.0                   # lip root centreline, outward from the wells
LIP_W = 0.5                     # one TPU perimeter
LIP_H = 1.0                     # 0.8 kinks at the tip before full squeeze (seal.py)
LIP_SLANT = 0.35                # tip leans outward by this much
LIP_SQUEEZE = 0.3               # groove floor sits this far above the free tip
GROOVE_IN = 0.6                 # groove edges, outward from the wells
GROOVE_OUT = 2.5
SEAL_CLOSE = 3.0                # closing radius that turns the 5 wells into one outline
SCREW = 14.5                    # screws at (±SCREW, ±SCREW) from Enter
SCREW_PILOT = 1.7               # M2 self-tapping into the frame + panel
SCREW_CLR = 2.3
MEM_SCREW_CLR = 2.6
HEAD_D = 4.4                    # ISO 7380 M2: Ø3.5 head, 1.04 tall
HEAD_DEPTH = 1.0
SCREW_LEN = 5.0

MEM_BOT = MEM_TOP - WEB_T
RET_BOT = MEM_BOT - RET_T
NUB_BOT = SW_TOP + PRELOAD      # free state; touches the plunger once preloaded
STEM_BOT = MEM_TOP - PRELOAD
FLANGE_BOT = PANEL_IN - FLANGE_T


def key_pos(off):
    return V(CENTER[0] + off[0] * PITCH, CENTER[1] + off[1] * PITCH, 0)


def cyl(r, c, z0, z1):
    return Part.makeCylinder(r, z1 - z0, V(c.x, c.y, z0))


def rsq(half, r, c, z0, z1):
    """Square prism with rounded vertical edges."""
    b = Part.makeBox(2 * half, 2 * half, z1 - z0, V(c.x - half, c.y - half, z0))
    if r <= 0:
        return b
    return b.makeFillet(r, [e for e in b.Edges
                            if abs(e.Vertexes[0].Point.z - e.Vertexes[1].Point.z) > 1e-6])


def fuse_all(shapes):
    out = shapes[0]
    for s in shapes[1:]:
        out = out.fuse(s)
    return out.removeSplitter()


def wells(z0, z1, grow=0.0):
    return fuse_all([rsq(WELL + grow, WELL_R + grow, key_pos(o), z0, z1) if k == "sq"
                     else cyl(WELL + grow, key_pos(o), z0, z1) for _, o, k in KEYS])


def seal_face(d, z):
    """One outline around all five wells (they are separate, 1.2 mm ribs apart),
    closed with SEAL_CLOSE and offset outward by d, as a planar face at z."""
    sec = wells(z - 1, z + 1, grow=SEAL_CLOSE).slice(V(0, 0, 1), z)
    assert len(sec) == 1, "seal outline did not close"
    return Part.Face(sec[0]).makeOffset2D(d - SEAL_CLOSE, join=0)


def seal_ring(d0, d1, z0, z1):
    return seal_face(d1, z0).extrude(V(0, 0, z1 - z0)).cut(
        seal_face(d0, z0 - 1).extrude(V(0, 0, z1 - z0 + 2)))


def make_lip():
    """Slanted lip under the sheet, root at MEM_BOT, tip LIP_H lower and LIP_SLANT out."""
    z0, z1 = MEM_BOT + 0.01, MEM_BOT - LIP_H

    def loft(d):
        return Part.makeLoft([seal_face(d, z0).OuterWire,
                              seal_face(d + LIP_SLANT, z1).OuterWire], True, True)

    try:
        lip = loft(LIP_OFF + LIP_W / 2).cut(loft(LIP_OFF - LIP_W / 2))
        if lip.isValid() and abs(lip.Volume) > 0:
            return lip
    except Part.OCCError:
        pass
    steps = 5   # fallback: stepped, which is what the printer makes anyway
    dz = LIP_H / steps
    return fuse_all([seal_ring(LIP_OFF - LIP_W / 2 + LIP_SLANT * (i + 0.5) / steps,
                               LIP_OFF + LIP_W / 2 + LIP_SLANT * (i + 0.5) / steps,
                               MEM_BOT - (i + 1) * dz, MEM_BOT - i * dz + 0.01)
                     for i in range(steps)])


def screws():
    c = key_pos((0, 0))
    return [c + V(sx * SCREW, sy * SCREW, 0) for sx in (-1, 1) for sy in (-1, 1)]


def make_cap(kind, c):
    if kind == "sq":
        half = HOLE_SQ / 2 - CAP_CLR
        body = rsq(half, 0.3, c, PANEL_IN, PANEL_OUT + CAP_PROUD)
        flange = rsq(HOLE_SQ / 2 + FLANGE_OVER, 1.0, c, FLANGE_BOT, PANEL_IN)
    else:
        body = cyl(HOLE_RD / 2 - CAP_CLR, c, PANEL_IN, PANEL_OUT + CAP_PROUD)
        flange = cyl(HOLE_RD / 2 + FLANGE_OVER - 0.1, c, FLANGE_BOT, PANEL_IN)
    top = PANEL_OUT + CAP_PROUD
    try:
        body = body.makeFillet(CAP_R, [e for e in body.Edges
                                       if abs(e.BoundBox.ZMin - top) < 1e-6
                                       and abs(e.BoundBox.ZMax - top) < 1e-6])
    except Part.OCCError:
        pass
    stem = cyl(STEM_D / 2, c, STEM_BOT, FLANGE_BOT + 0.01)
    return fuse_all([body, flange, stem])


def make_frame():
    c = key_pos((0, 0))
    pad = rsq(OUTLINE, OUTLINE_R, c, MEM_TOP, PANEL_IN + 0.01).cut(wells(MEM_TOP - 1, PANEL_IN + 1))
    pilots = fuse_all([cyl(SCREW_PILOT / 2, p, MEM_TOP - 0.01, RET_BOT + HEAD_DEPTH + SCREW_LEN + 0.3)
                       for p in screws()])
    return pad.cut(pilots), pilots


def make_membrane():
    c = key_pos((0, 0))
    sheet = rsq(OUTLINE - MEM_INSET, OUTLINE_R - MEM_INSET, c, MEM_BOT, MEM_TOP)
    sheet = sheet.cut(fuse_all([cyl(MEM_SCREW_CLR / 2, p, MEM_BOT - 1, MEM_TOP + 1) for p in screws()]))
    parts = [sheet]
    for _, o, _ in KEYS:
        p = key_pos(o)
        parts.append(cyl(HUB_R, p, MEM_BOT - HUB_T, MEM_BOT + 0.01))
        parts.append(cyl(NUB_D / 2, p, NUB_BOT, MEM_BOT - HUB_T + 0.01))
    parts.append(make_lip())
    return fuse_all(parts)


def make_retainer():
    c = key_pos((0, 0))
    plate = rsq(OUTLINE, OUTLINE_R, c, RET_BOT, MEM_BOT).cut(wells(RET_BOT - 1, MEM_BOT + 1))
    # round the clamp edge so the web is not cut when it flexes
    try:
        plate = plate.makeFillet(0.3, [e for e in plate.Edges
                                       if abs(e.BoundBox.ZMin - MEM_BOT) < 1e-6
                                       and abs(e.BoundBox.ZMax - MEM_BOT) < 1e-6
                                       and e.BoundBox.XLength < 2 * OUTLINE - 1])
    except Part.OCCError:
        pass
    plate = plate.cut(seal_ring(GROOVE_IN, GROOVE_OUT, MEM_BOT - (LIP_H - LIP_SQUEEZE), MEM_BOT + 1))
    if RIB_H > 0:
        plate = plate.fuse(make_ribs())
    for p in screws():
        plate = plate.cut(cyl(SCREW_CLR / 2, p, RET_BOT - RIB_H - 1, MEM_BOT + 1))
        plate = plate.cut(cyl(HEAD_D / 2, p, RET_BOT - RIB_H - 1, RET_BOT + HEAD_DEPTH))
    return plate.removeSplitter()


def make_ribs():
    """Ribs under the retainer: a band under the seal loop (and the ribs between
    the wells), the outer rim, and a diagonal through each screw."""
    c = key_pos((0, 0))
    z0, z1 = RET_BOT - RIB_H, RET_BOT + 0.01
    outline = rsq(OUTLINE, OUTLINE_R, c, z0, z1)
    band = seal_face(GROOVE_OUT + 0.3, z0).extrude(V(0, 0, z1 - z0))
    rim = outline.cut(rsq(OUTLINE - RIB_W, OUTLINE_R - RIB_W, c, z0 - 1, z1 + 1))
    diag = []
    for sx in (-1, 1):
        for sy in (-1, 1):
            b = Part.makeBox(2 * OUTLINE, RIB_W, z1 - z0, V(0, -RIB_W / 2, z0))
            b.rotate(V(0, 0, 0), V(0, 0, 1), (45 if sx * sy > 0 else -45) + (180 if sx < 0 else 0))
            diag.append(b.translate(c))
    ribs = fuse_all([band, rim] + diag).common(outline)
    return ribs.cut(wells(z0 - 1, z1 + 1))


def make_reference():
    sw = Part.read(SWITCH_STEP)
    switches = [sw.copy().translate(key_pos(o) + V(0, 0, PCB_FRONT)) for _, o, _ in KEYS]
    c = key_pos((0, 0))
    pcb = rsq(OUTLINE + 6, 0, c, PCB_FRONT - PCB_T, PCB_FRONT).common(
        Part.makeBox(69, 200, 20, V(-34.5, -100, -10)))   # stand-in, case inner wall at x = -35
    heads = fuse_all([cyl(3.5 / 2, p, RET_BOT + HEAD_DEPTH - 1.04, RET_BOT + HEAD_DEPTH) for p in screws()])
    return switches, pcb, heads


def frontcase_shape():
    d = App.openDocument(FRONTCASE)
    s = d.getObject("Body").Shape.copy()
    App.closeDocument(d.Name)
    return s


def add(doc, name, shape, color, transparency=0):
    o = doc.addObject("Part::Feature", name)
    o.Shape = shape
    if GUI:
        o.ViewObject.ShapeColor = color
        o.ViewObject.Transparency = transparency
    return o


def check(case, caps, frame, membrane, retainer, switches, pcb, heads):
    import sys
    print = lambda *a: sys.__stdout__.write(" ".join(map(str, a)) + "\n")

    def vol(a, b):
        return a.common(b).Volume

    print("== interference (mm³, want ~0)")
    print("  caps / case     %.3f" % sum(vol(c, case) for c in caps))
    print("  frame / case    %.3f" % vol(frame, case))
    print("  retainer / case %.3f" % vol(retainer, case))
    print("  membrane / case %.3f" % vol(membrane, case))
    print("  membrane / ret  %.3f (lip squeeze, intended)" % vol(membrane, retainer))
    print("  retainer / sw   %.3f" % sum(vol(retainer, s) for s in switches))
    print("  membrane / sw   %.3f (nub free state, %.2f above plunger)" %
          (sum(vol(membrane, s) for s in switches), NUB_BOT - SW_TOP))
    print("  heads / pcb     %.3f, gap %.2f" % (vol(heads, pcb), heads.BoundBox.ZMin - PCB_FRONT))
    print("== stack")
    print("  web span %.2f..%.2f mm, flange-to-membrane gap %.2f, flange-in-well clearance sq %.2f"
          % (WELL - HUB_R, WELL * 2 ** 0.5 - HUB_R, FLANGE_BOT - MEM_TOP,
             WELL - HOLE_SQ / 2 - FLANGE_OVER))
    print("  screw engagement %.1f mm, skin in front of pilot %.1f mm"
          % (RET_BOT + HEAD_DEPTH + SCREW_LEN - MEM_TOP,
             PANEL_OUT - (RET_BOT + HEAD_DEPTH + SCREW_LEN + 0.3)))


def build():
    for d in list(App.listDocuments().values()):
        if d.Label == "DpadMembrane":
            App.closeDocument(d.Name)
    case = frontcase_shape()
    doc = App.newDocument("DpadMembrane")
    doc.saveAs(OUT)

    caps = []
    for name, o, kind in KEYS:
        cap = make_cap(kind, key_pos(o))
        caps.append(cap)
        add(doc, "Cap_" + name, cap, (0.12, 0.12, 0.13))
    frame, pilots = make_frame()
    membrane = make_membrane()
    retainer = make_retainer()
    switches, pcb, heads = make_reference()

    add(doc, "FramePad", frame, (0.22, 0.23, 0.25))
    add(doc, "FramePilotHoles", pilots, (0.9, 0.2, 0.2)).Visibility = False
    add(doc, "Membrane", membrane, (0.85, 0.45, 0.1))
    add(doc, "Retainer", retainer, (0.35, 0.55, 0.75))
    ref = doc.addObject("App::DocumentObjectGroup", "Reference")
    fc = add(doc, "FrontCase_ref", case, (0.22, 0.23, 0.25), 70)
    ref.addObject(fc)
    for i, s in enumerate(switches):
        ref.addObject(add(doc, "Switch%d" % i, s, (0.6, 0.6, 0.6)))
    ref.addObject(add(doc, "PCB_ref", pcb, (0.1, 0.4, 0.15)))
    ref.addObject(add(doc, "ScrewHeads", heads, (0.7, 0.7, 0.72)))
    doc.recompute()

    check(case, caps, frame, membrane, retainer, switches, pcb, heads)

    if os.environ.get("DPAD_STL"):
        import Mesh
        out = os.environ["DPAD_STL"]
        os.makedirs(out, exist_ok=True)
        for o in doc.Objects:
            if o.Name.startswith(("Cap_", "Membrane", "Retainer")):
                Mesh.export([o], os.path.join(out, o.Name + ".stl"))
    return doc


def render(doc, outdir):
    from FreeCAD import Rotation as Rot
    os.makedirs(outdir, exist_ok=True)
    gdoc = Gui.getDocument(doc.Name)
    Gui.ActiveDocument = gdoc
    v = gdoc.ActiveView
    v.setCameraType("Orthographic")
    v.setAxisCross(False)

    def shot(name, rot):
        v.setCameraOrientation(rot)
        for _ in range(3):
            Gui.updateGui()
            v.fitAll()
        v.saveImage(os.path.join(outdir, "dpad_" + name + ".png"), 1400, 1000, "White")

    def show(names):
        for o in doc.Objects:
            if o.TypeId == "Part::Feature":
                o.ViewObject.Visibility = o.Name in names

    printed = [o.Name for o in doc.Objects if o.Name.startswith(("Cap_", "Membrane", "Retainer"))]

    # section through Up / Enter / Down (x = CENTER) and through Left / Enter / Right
    sec_objs = []
    c = key_pos((0, 0))
    crop = Part.makeBox(2 * OUTLINE + 8, 2 * OUTLINE + 8, 40,
                        V(c.x - OUTLINE - 4, c.y - OUTLINE - 4, -10))
    for axis, cut_name in ((0, "secX"), (1, "secY")):
        if axis == 0:
            half = Part.makeBox(60, 200, 40, V(c.x, -100, -10))
        else:
            half = Part.makeBox(200, 60, 40, V(-100, c.y, -10))
        names = []
        for o in doc.Objects:
            if o.TypeId != "Part::Feature" or o.Name.startswith(("Frame", "Section")):
                continue
            s = o.Shape.cut(half).common(crop) if o.Name != "FramePilotHoles" else None
            if s is None or s.isNull() or s.Volume < 1e-6:
                continue
            n = "Section_%s_%s" % (cut_name, o.Name)
            so = add(doc, n, s, tuple(o.ViewObject.ShapeColor[:3]))
            names.append(n)
        f = doc.getObject("FramePad").Shape.cut(half).common(crop)
        names.append(add(doc, "Section_%s_Frame" % cut_name, f, (0.22, 0.23, 0.25)).Name)
        sec_objs.append(names)
    doc.recompute()

    show(sec_objs[0])
    shot("section_updown", Rot(V(0, 1, 0), 90).multiply(Rot(V(0, 0, 1), 90)))
    show(sec_objs[1])
    shot("section_leftright", Rot(V(1, 0, 0), -90))
    three_q = Rot(V(0, 1, 0), 30).multiply(Rot(V(1, 0, 0), -35))
    show(sec_objs[0])
    shot("section_3q", Rot(V(0, 1, 0), 60).multiply(Rot(V(1, 0, 0), -25)))
    show(printed)
    shot("parts_front", three_q)
    shot("parts_back", Rot(V(0, 1, 0), 180).multiply(three_q))
    show(["FramePad"])
    shot("frame_back", Rot(V(0, 1, 0), 180).multiply(three_q))

    for names in sec_objs:
        for n in names:
            doc.removeObject(n)
    for o in doc.Objects:
        if o.TypeId == "Part::Feature":
            o.ViewObject.Visibility = o.Name not in ("FramePilotHoles", "FrontCase_ref")
    doc.recompute()


if __name__ in ("__main__", "dpad_membrane"):
    doc = build()
    doc.save()
    if GUI and os.environ.get("DPAD_RENDER"):
        render(doc, os.environ["DPAD_RENDER"])
        doc.save()
    print("DPAD DONE", flush=True)
    if os.environ.get("DPAD_EXIT"):
        os._exit(0)
