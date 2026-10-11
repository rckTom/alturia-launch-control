"""Sealed power button: one printed TPU boot through the top wall onto SW101.

Run with the FreeCAD GUI (colours need it), e.g. headless:
    xvfb-run -a freecad hardware/controller/cad/power_boot.py
It writes PowerBoot.FCStd next to this file and, if BOOT_RENDER is set to a
directory, preview PNGs into it. BOOT_STL=<dir> also exports the boot.

Frame: FrontCase.FCStd coordinates. The top wall is y 79.5..82.0 (2.5 mm), the
front face z = 13. SW101 (ALPS SKRTLAE010, side actuated, on the top PCB edge)
has its 2x2 actuator on the axis x = -18.37, z = 5.98, free tip at y = 77.55,
0.05 past the PCB edge. The outer wall is flat only up to z ~ 10 (front
fillet) and the case split/gasket lip sits below z ~ 3.5 on the inside, so
everything here lives in a band about 8 mm tall.

The boot, outside to inside:
  flange  stadium pad, sits in a shallow pocket in the wall, 0.6 proud.
          Its back face is the seal, pulled against the pocket floor by
  pins    two slotted, flat-sided barbed pins either side, through Ø2
          holes (the prongs close in z, the flats clear in x); the barb
          shoulder sits PIN_PRELOAD short of the inner face so the pins
          stretch and keep the flange pressed on.
  web     WEB_T thin disc across the counterbore, flush with the flange
          front. This is what bends; stiffness goes with WEB_T³.
  stem    hangs from the web centre, cone out to a collar, then Ø2.4
          through a Ø3.2 guide bore onto the actuator (PREGAP above it).
          The collar lands on the counterbore floor after PREGAP + TRAVEL +
          OVERTRAVEL, so a hard press goes into the wall, not the switch.

Printed front face down: web, flange front and stem centre all lie on the bed,
so the web is the first layers (no bridging, tight) and the stem, collar cone
(45°), pins and barbs grow upward without supports. The price is a flush
button face; it sits on a 0.6 mm proud pad so a thumb finds it.

Splash-proof (flat TPU land on a printed face), not submersible.
"""
import math
import os

import FreeCAD as App
import Part
from FreeCAD import Vector as V

try:
    import FreeCADGui as Gui
    GUI = App.GuiUp
except ImportError:
    GUI = False

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "PowerBoot.FCStd")
FRONTCASE = os.path.join(HERE, "FrontCase.FCStd")
ASSEMBLY = os.path.join(HERE, "ControllerAssembly.FCStd")
FC_OFFSET = V(-6.03913, 2.96004, 2.21679)   # FrontCase link placement in the assembly
CASE_FEATURE = "Fillet001"      # last FrontCase feature before its own power slot

# ---- case / switch (measured from FrontCase and the assembly) -------------
AX_X, AX_Z = -18.37, 5.98       # actuator axis
WALL_OUT = 82.0
WALL_IN = 79.5
ACT_TIP = 77.55                 # free actuator tip
TRAVEL = 0.25                   # SKRT series; check on the real part
FILLET_Z = 10.0                 # outer wall flat below this

# ---- boot -----------------------------------------------------------------
POCKET = 0.4                    # flange pocket depth in the wall
POCKET_CLR = 0.15               # per side around the flange
FLANGE_T = 1.0                  # 0.6 proud of the wall
FL_HX = 8.0                     # stadium half length (along x)
FL_HZ = 3.8                     # half height (along z), top at 9.78
FRONT_CHAMFER = 0.4
CB_R = 3.0                      # counterbore = free web radius; 0.8 land at top
WEB_T = 0.4                     # 2 layers at 0.2
HUB_R = 1.2                     # web is stiff inside this
COLLAR_R = 2.1
COLLAR_T = 0.4
STEM_R = 1.2
TIP_R = 1.0                     # on the 2x2 actuator
PREGAP = 0.1                    # stem tip above the free actuator
OVERTRAVEL = 0.2                # past full switch travel before the collar lands
GUIDE_R = 1.6
PIN_X = 6.0                     # pins at AX_X ± PIN_X
PIN_HOLE_R = 1.0
PIN_R = 0.95
PIN_PRELOAD = 0.15              # stretch when seated
BARB_R = 1.25                   # 0.25 ledge behind the wall, top and bottom
BARB_L = 1.2
BARB_TIP_R = 0.6
BARB_FLAT = 0.9                 # barb cut flat to ±this along x: the slot only
                                # lets the prongs close in z
SLOT_W = 0.7                    # closed: 2·BARB_R - SLOT_W = 1.8 in the Ø2 hole
SLOT_L = 1.8                    # from the barb tip into the pin

FL_BACK = WALL_OUT - POCKET
FL_FRONT = FL_BACK + FLANGE_T
WEB_BACK = FL_FRONT - WEB_T
CONE_END = WEB_BACK - (COLLAR_R - HUB_R)          # 45° cone
STOP = CONE_END - COLLAR_T                        # collar face toward the switch
CB_FLOOR = STOP - (PREGAP + TRAVEL + OVERTRAVEL)
STEM_TIP = ACT_TIP + PREGAP
SHOULDER = WALL_IN + PIN_PRELOAD


def ycyl(r, x, z, y0, y1):
    """Cylinder along y from y0 to y1."""
    lo, hi = min(y0, y1), max(y0, y1)
    return Part.makeCylinder(r, hi - lo, V(x, lo, z), V(0, 1, 0))


def ycone(r0, r1, x, z, y0, y1):
    """Cone along y, radius r0 at y0, r1 at y1."""
    if y0 > y1:
        r0, r1, y0, y1 = r1, r0, y1, y0
    return Part.makeCone(r0, r1, y1 - y0, V(x, y0, z), V(0, 1, 0))


def stadium(hx, hz, y0, y1):
    """Stadium (rounded ends along x) on the axis, extruded along y."""
    r = hz
    body = Part.makeBox(2 * (hx - r), y1 - y0, 2 * r, V(AX_X - hx + r, y0, AX_Z - r))
    ends = [ycyl(r, AX_X + s * (hx - r), AX_Z, y0, y1) for s in (-1, 1)]
    return body.fuse(ends).removeSplitter()


def fuse_all(shapes):
    out = shapes[0]
    for s in shapes[1:]:
        out = out.fuse(s)
    return out.removeSplitter()


def make_pin(x):
    shaft = ycyl(PIN_R, x, AX_Z, FL_BACK + 0.01, SHOULDER)
    barb = ycone(BARB_R, BARB_TIP_R, x, AX_Z, SHOULDER, SHOULDER - BARB_L)
    tip = SHOULDER - BARB_L
    barb = barb.common(Part.makeBox(2 * BARB_FLAT, BARB_L + 0.02, 2 * BARB_R + 1,
                                    V(x - BARB_FLAT, tip - 0.01, AX_Z - BARB_R - 0.5)))
    slot = Part.makeBox(2 * BARB_R + 1, SLOT_L, SLOT_W,
                        V(x - BARB_R - 0.5, tip, AX_Z - SLOT_W / 2))
    return shaft.fuse(barb).cut(slot).removeSplitter()


def make_boot():
    flange = stadium(FL_HX, FL_HZ, FL_BACK, FL_FRONT)
    try:
        flange = flange.makeChamfer(FRONT_CHAMFER, [
            e for e in flange.Edges
            if abs(e.BoundBox.YMin - FL_FRONT) < 1e-6 and abs(e.BoundBox.YMax - FL_FRONT) < 1e-6])
    except Part.OCCError:
        pass
    # hollow behind the web
    flange = flange.cut(ycyl(CB_R, AX_X, AX_Z, FL_BACK - 1, WEB_BACK))
    hub = ycyl(HUB_R, AX_X, AX_Z, WEB_BACK - 0.01, FL_FRONT)
    cone = ycone(HUB_R, COLLAR_R, AX_X, AX_Z, WEB_BACK, CONE_END)
    collar = ycyl(COLLAR_R, AX_X, AX_Z, STOP, CONE_END + 0.01)
    stem = ycyl(STEM_R, AX_X, AX_Z, STEM_TIP + 0.3, STOP + 0.01)
    tip = ycone(TIP_R, STEM_R, AX_X, AX_Z, STEM_TIP, STEM_TIP + 0.3)
    pins = [make_pin(AX_X + s * PIN_X) for s in (-1, 1)]
    return fuse_all([flange, hub, cone, collar, stem, tip] + pins)


def make_wall_cut():
    """Subtract from FrontCase: pocket, counterbore, guide bore, pin holes."""
    pocket = stadium(FL_HX + POCKET_CLR, FL_HZ + POCKET_CLR, FL_BACK, WALL_OUT + 2)
    cb = ycyl(CB_R, AX_X, AX_Z, CB_FLOOR, FL_BACK + 0.01)
    guide = ycyl(GUIDE_R, AX_X, AX_Z, WALL_IN - 1, CB_FLOOR + 0.01)
    holes = [ycyl(PIN_HOLE_R, AX_X + s * PIN_X, AX_Z, WALL_IN - 1, FL_BACK + 0.01) for s in (-1, 1)]
    return fuse_all([pocket, cb, guide] + holes)


def web_stiffness(a=CB_R, b=HUB_R, t=WEB_T, E=26.0, nu=0.45):
    """Annular plate, clamped at a, rigid guided hub at b, Kirchhoff + shear.
    E ~ 26 MPa for 95A TPU (small strain). N/mm."""
    D = E * t ** 3 / (12 * (1 - nu ** 2))
    f = lambda r: (r / 2 * math.log(r) - r / 4) / (2 * math.pi * D)
    det = a / 2 / b - b / 2 / a
    c1 = (-f(a) / b + f(b) / a) / det
    c2 = (-f(b) * a / 2 + f(a) * b / 2) / det
    n = 2000
    G = E / (2 * (1 + nu))
    d = 0.0
    for i in range(n):
        r = b + (a - b) * (i + 0.5) / n
        d += (abs(f(r) + c1 * r / 2 + c2 / r) + 1 / (2 * math.pi * r) / (5 / 6 * G * t)) * (a - b) / n
    return 1 / d


def frontcase_shape():
    """The wall without FrontCase's own 10x4 power slot (Pocket006 + Chamfer002,
    2026-10-07): WallCut replaces it, so check against the feature before it."""
    d = App.openDocument(FRONTCASE)
    base = d.getObject(CASE_FEATURE)
    s = (base or d.getObject("Body")).Shape.copy()
    App.closeDocument(d.Name)
    return s


def reference_parts():
    """SW101 and the PCB from the assembly, moved into FrontCase coordinates,
    plus every component whose box comes near the boot."""
    d = App.openDocument(ASSEMBLY)
    link = d.getObject("Part")
    ctrl = link.LinkedObject.Document
    to_fc = App.Placement(FC_OFFSET * -1, App.Rotation()).multiply(link.Placement)
    zone = App.BoundBox(AX_X - FL_HX - 2, 74, AX_Z - 5, AX_X + FL_HX + 2, 83, AX_Z + 5)
    switch, pcb, near = None, None, []
    for o in ctrl.Objects:
        if o.TypeId != "Part::Feature" or o.Shape.isNull():
            continue
        s = o.Shape.copy()
        s.Placement = to_fc.multiply(o.getGlobalPlacement())
        if o.Label == "controller_PCB":
            pcb = s
        elif not s.BoundBox.isValid() or not s.BoundBox.intersect(zone):
            continue
        elif any(p.Label == "SKRTLA" for p in o.InList):
            switch = s
        else:
            near.append((o.Label, s))
    App.closeDocument(d.Name)
    for doc in list(App.listDocuments().values()):
        if doc.FileName.endswith("Controller.FCStd"):
            App.closeDocument(doc.Name)
    return switch, pcb, near


def add(doc, name, shape, color, transparency=0):
    o = doc.addObject("Part::Feature", name)
    o.Shape = shape
    if GUI:
        o.ViewObject.ShapeColor = color
        o.ViewObject.Transparency = transparency
    return o


def check(case, cut_case, boot, switch, pcb, near):
    import sys
    print = lambda *a: sys.__stdout__.write(" ".join(map(str, a)) + "\n")

    def vol(a, b):
        return a.common(b).Volume

    print("== interference (mm³, want ~0)")
    pins = fuse_all([make_pin(AX_X + s * PIN_X) for s in (-1, 1)])
    print("  boot / case (cut)  %.3f, of which barbs %.3f (pin preload, intended)"
          % (vol(boot, cut_case), vol(pins, cut_case)))
    print("  boot / switch      %.3f (tip %.2f above actuator)" % (vol(boot, switch), PREGAP))
    print("  boot / pcb         %.3f" % vol(boot, pcb))
    for label, s in near:
        v = vol(boot, s)
        if v > 1e-4:
            print("  boot / %-12s %.3f" % (label, v))
    print("  cut removes        %.1f mm³ of FrontCase" % case.common(make_wall_cut()).Volume)
    print("== geometry")
    print("  flange z %.2f..%.2f (outer wall flat below %.1f), land above bore %.2f"
          % (AX_Z - FL_HZ, AX_Z + FL_HZ, FILLET_Z, FL_HZ - CB_R))
    print("  counterbore depth %.2f, guide bore length %.2f, stem %.2f long"
          % (FL_BACK - CB_FLOOR, CB_FLOOR - WALL_IN, STOP - STEM_TIP))
    print("  stop after %.2f mm stem travel (switch makes at %.2f)"
          % (STOP - CB_FLOOR, PREGAP + TRAVEL))
    print("  barb inside wall face %.2f, PCB edge at y %.2f"
          % (WALL_IN - (SHOULDER - BARB_L), pcb.BoundBox.YMax))
    k = web_stiffness()
    print("== feel (95A, E 26 MPa, estimate)")
    print("  web %.2f N/mm: +%.2f N at click, +%.2f N at stop, switch 1.6 N"
          % (k, k * (PREGAP + TRAVEL), k * (STOP - CB_FLOOR)))


def build():
    for d in list(App.listDocuments().values()):
        if d.Label == "PowerBoot":
            App.closeDocument(d.Name)
    case = frontcase_shape()
    switch, pcb, near = reference_parts()
    doc = App.newDocument("PowerBoot")
    doc.saveAs(OUT)

    boot = make_boot()
    cut = make_wall_cut()
    cut_case = case.cut(cut)
    add(doc, "Boot", boot, (0.85, 0.45, 0.1))
    add(doc, "WallCut", cut, (0.9, 0.2, 0.2), 50).Visibility = False
    ref = doc.addObject("App::DocumentObjectGroup", "Reference")
    ref.addObject(add(doc, "FrontCase_cut", cut_case, (0.22, 0.23, 0.25), 60))
    ref.addObject(add(doc, "SW101", switch, (0.6, 0.6, 0.6)))
    ref.addObject(add(doc, "PCB_ref", pcb, (0.1, 0.4, 0.15)))
    doc.recompute()

    check(case, cut_case, boot, switch, pcb, near)

    if os.environ.get("BOOT_STL"):
        import Mesh
        out = os.environ["BOOT_STL"]
        os.makedirs(out, exist_ok=True)
        Mesh.export([doc.getObject("Boot")], os.path.join(out, "PowerBoot.stl"))
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
        v.saveImage(os.path.join(outdir, "boot_" + name + ".png"), 1400, 1000, "White")

    crop = Part.makeBox(2 * FL_HX + 8, 12, 16, V(AX_X - FL_HX - 4, 73, AX_Z - 8))
    secs = {}
    for key, half in (("z", Part.makeBox(100, 100, 50, V(-60, 40, AX_Z))),     # cut at the axis height
                      ("x", Part.makeBox(50, 100, 50, V(AX_X, 40, -20)))):     # cut through the stem
        names = []
        for o in list(doc.Objects):
            if o.TypeId != "Part::Feature" or o.Name in ("WallCut",) or o.Name.startswith("Sec"):
                continue
            s = o.Shape.common(crop).cut(half)
            if s.isNull() or s.Volume < 1e-6:
                continue
            names.append(add(doc, "Sec%s_%s" % (key, o.Name), s, tuple(o.ViewObject.ShapeColor[:3])).Name)
        secs[key] = names
    doc.recompute()

    def show(names):
        for o in doc.Objects:
            if o.TypeId == "Part::Feature":
                o.ViewObject.Visibility = o.Name in names

    show(secs["z"])
    shot("section_top", Rot(V(0, 0, 1), 0))                                  # look down -z
    show(secs["x"])
    shot("section_side", Rot(V(0, 1, 0), V(0, 0, 1), V(1, 0, 0), "ZYX"))      # from +x, z up
    show(["Boot"])
    three_q = Rot(V(0, 0, 1), 25).multiply(Rot(V(1, 0, 0), 60))
    shot("boot_front", Rot(V(0, 0, 1), 160).multiply(Rot(V(1, 0, 0), 70)))
    shot("boot_back", Rot(V(0, 0, 1), 180).multiply(three_q))
    show(["FrontCase_cut"])
    shot("wall_outside", Rot(V(-1, 0, 0), V(0, 0, 1), V(0, 1, 0), "ZYX"))    # from +y, z up

    for names in secs.values():
        for n in names:
            doc.removeObject(n)
    for o in doc.Objects:
        if o.TypeId == "Part::Feature":
            o.ViewObject.Visibility = o.Name != "WallCut"
    doc.recompute()


if __name__ in ("__main__", "power_boot"):
    doc = build()
    doc.save()
    if GUI and os.environ.get("BOOT_RENDER"):
        render(doc, os.environ["BOOT_RENDER"])
        doc.save()
    print("BOOT DONE", flush=True)
    if os.environ.get("BOOT_EXIT"):
        os._exit(0)
