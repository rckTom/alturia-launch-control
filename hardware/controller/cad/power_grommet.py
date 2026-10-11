"""Sealed power button, compact version: a round TPU grommet onto SW101.

Same job as power_boot.py, without the two side pins. The boot is held by
barbed fingers on its own sleeve, so the visible part is a Ø7.6 disc instead
of a 16 x 7.6 pad, and the clamping force sits right at the hole edge.

Run with the FreeCAD GUI (colours need it), e.g. headless:
    xvfb-run -a freecad hardware/controller/cad/power_grommet.py
It writes PowerGrommet.FCStd next to this file and, if GROMMET_RENDER is set
to a directory, preview PNGs into it. GROMMET_STL=<dir> also exports the
grommet. The check output only shows when run with freecadcmd.

Frame: FrontCase.FCStd coordinates, see power_boot.py. Actuator axis
x = -18.37, z = 5.98, outward is +y, wall y 79.5..82.0. Inside the wall only
z 3.5..8.5 is free (split lip below, panel fillet above), so nothing that
sticks out behind the wall may reach further than ±2.5 in z: the barbs sit
left and right only.

The grommet, outside to inside:
  flange  Ø7.6 disc in a round pocket, 0.6 proud. Its back face is the seal.
  web     WEB_T disc across the sleeve bore, flush with the flange front.
  sleeve  through the Ø6 hole, cut by four slits into
            fingers  left and right, with a barb lobe behind the wall. The
                     shoulder is PRELOAD short, so seated fingers stretch and
                     pull the flange onto the pocket floor at the hole edge.
            stops    top and bottom, each carrying a 45° lip inside the bore
  stem    from the web centre: 45° cone out to a collar, 45° cone back in to
          Ø2.4, onto the actuator (PREGAP above it). The collar's back cone
          lands on the lips after PREGAP + TRAVEL + OVERTRAVEL. The stop is
          TPU, so it is softer than a wall shoulder.

Printed front face down, like power_boot.py: the web is the first layers and
every overhang (hub cone, collar back, lips, barb lead-in) is 45° or less.
The barb shoulder is a 0.5 mm ledge.

Fitting: press in from outside with the case open, then pull the barbs
through from inside with tweezers (the fingers fold inward into the bore).
Then the PCB, which slides in past the stem.
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
OUT = os.path.join(HERE, "PowerGrommet.FCStd")
FRONTCASE = os.path.join(HERE, "FrontCase.FCStd")
ASSEMBLY = os.path.join(HERE, "ControllerAssembly.FCStd")
FC_OFFSET = V(-6.03913, 2.96004, 2.21679)   # FrontCase link placement in the assembly
CASE_FEATURE = "Fillet001"      # last FrontCase feature before its own power slot

# ---- case / switch (measured, same as power_boot.py) ----------------------
AX_X, AX_Z = -18.37, 5.98
WALL_OUT = 82.0
WALL_IN = 79.5
ACT_TIP = 77.55
TRAVEL = 0.25                   # SKRT series; check on the real part
FILLET_Z = 10.0                 # outer wall flat below this
BAND = 2.5                      # free inside the wall: AX_Z ± BAND

# ---- grommet --------------------------------------------------------------
POCKET = 0.4
POCKET_CLR = 0.15
FLANGE_R = 3.8
FLANGE_T = 1.0
FRONT_CHAMFER = 0.4
HOLE_R = 3.0
SLEEVE_OR = 2.95                # slip fit in the hole
SLEEVE_IR = 2.45                # = free web radius
WEB_T = 0.3                     # 2 layers at 0.15
HUB_R = 1.0
COLLAR_R = 1.9
COLLAR_T = 0.3
STEM_R = 1.2
TIP_R = 1.0
PREGAP = 0.1
OVERTRAVEL = 0.1                # switch sees little force before the stop
LIP_IR = 1.5                    # stop lip bore, 0.3 around the stem
LIP_T = 0.3                     # straight part of the lip after its cone
FINGER_HALF = 35.0              # finger sector, degrees either side of ±x
SLIT = 12.0                     # slit width, degrees (~0.55 mm at the sleeve)
PRELOAD = 0.1                   # finger stretch when seated
BARB_R = 3.45                   # 0.45 ledge behind the hole edge
BARB_L = 1.0
BARB_TIP_R = 2.85

FL_BACK = WALL_OUT - POCKET
FL_FRONT = FL_BACK + FLANGE_T
WEB_BACK = FL_FRONT - WEB_T
CONE_END = WEB_BACK - (COLLAR_R - HUB_R)
COLLAR_BACK = CONE_END - COLLAR_T
STEM_TOP = COLLAR_BACK - (COLLAR_R - STEM_R)
STOP_GAP = PREGAP + TRAVEL + OVERTRAVEL
LIP_TOP = COLLAR_BACK - COLLAR_R + SLEEVE_IR - STOP_GAP   # parallel 45° cones, STOP_GAP apart
LIP_CONE_END = LIP_TOP - (SLEEVE_IR - LIP_IR)
LIP_END = LIP_CONE_END - LIP_T
SHOULDER = WALL_IN + PRELOAD
STEM_TIP = ACT_TIP + PREGAP

# Built in a local frame: Z along the button axis (= FrontCase y), X = x
# from the axis, Y = -(z from the axis). Everything is symmetric in Y.
TO_FC = App.Placement(V(AX_X, 0, AX_Z), App.Rotation(V(1, 0, 0), -90))


def zcyl(r, w0, w1, a0=0.0, a1=360.0):
    lo, hi = min(w0, w1), max(w0, w1)
    c = Part.makeCylinder(r, hi - lo, V(0, 0, lo), V(0, 0, 1), a1 - a0)
    return c.rotate(V(0, 0, 0), V(0, 0, 1), a0) if a0 else c


def zcone(r0, r1, w0, w1, a0=0.0, a1=360.0):
    """Cone with radius r0 at w0 and r1 at w1."""
    if w0 > w1:
        r0, r1, w0, w1 = r1, r0, w1, w0
    c = Part.makeCone(r0, r1, w1 - w0, V(0, 0, w0), V(0, 0, 1), a1 - a0)
    return c.rotate(V(0, 0, 0), V(0, 0, 1), a0) if a0 else c


def fuse_all(shapes):
    out = shapes[0]
    for s in shapes[1:]:
        out = out.fuse(s)
    return out.removeSplitter()


def placed(s):
    s = s.copy()
    s.transformShape(TO_FC.toMatrix())
    return s


def fingers_sectors():
    """(a0, a1) of the two fingers and the two stop sectors."""
    f = [(-FINGER_HALF, FINGER_HALF), (180 - FINGER_HALF, 180 + FINGER_HALF)]
    s = [(FINGER_HALF + SLIT, 180 - FINGER_HALF - SLIT),
         (180 + FINGER_HALF + SLIT, 360 - FINGER_HALF - SLIT)]
    return f, s


def make_grommet():
    flange = zcyl(FLANGE_R, FL_BACK, FL_FRONT)
    try:
        flange = flange.makeChamfer(FRONT_CHAMFER, [
            e for e in flange.Edges
            if abs(e.BoundBox.ZMin - FL_FRONT) < 1e-6 and abs(e.BoundBox.ZMax - FL_FRONT) < 1e-6])
    except Part.OCCError:
        pass
    flange = flange.cut(zcyl(SLEEVE_IR, FL_BACK - 1, WEB_BACK))
    parts = [flange,
             zcyl(HUB_R, WEB_BACK - 0.01, FL_FRONT),
             zcone(HUB_R, COLLAR_R, WEB_BACK, CONE_END),
             zcyl(COLLAR_R, COLLAR_BACK, CONE_END + 0.01),
             zcone(COLLAR_R, STEM_R, COLLAR_BACK, STEM_TOP),
             zcyl(STEM_R, STEM_TIP + 0.3, STEM_TOP + 0.01),
             zcone(TIP_R, STEM_R, STEM_TIP, STEM_TIP + 0.3)]
    fingers, stops = fingers_sectors()
    for a0, a1 in fingers:
        tube = zcyl(SLEEVE_OR, SHOULDER, FL_BACK + 0.01, a0, a1)
        lobe = zcone(BARB_R, BARB_TIP_R, SHOULDER, SHOULDER - BARB_L, a0, a1)
        parts.append(tube.fuse(lobe).cut(zcyl(SLEEVE_IR, SHOULDER - BARB_L - 1, FL_BACK + 1)))
    # stop lip: a ring inside the bore whose top is a 45° cone, kept on the stop sectors only
    lip = zcyl(SLEEVE_IR + 0.01, LIP_END, LIP_TOP).cut(zcone(LIP_IR, SLEEVE_IR + 0.01, LIP_CONE_END, LIP_TOP)) \
        .cut(zcyl(LIP_IR, LIP_END - 1, LIP_CONE_END + 0.01))
    for a0, a1 in stops:
        tube = zcyl(SLEEVE_OR, LIP_END, FL_BACK + 0.01, a0, a1).cut(zcyl(SLEEVE_IR, LIP_END - 1, FL_BACK + 1))
        parts.append(tube.fuse(lip.common(zcyl(SLEEVE_OR, LIP_END - 1, LIP_TOP + 1, a0, a1))))
    return placed(fuse_all(parts))


def make_wall_cut():
    """Subtract from FrontCase: round pocket and the hole."""
    pocket = zcyl(FLANGE_R + POCKET_CLR, FL_BACK, WALL_OUT + 2)
    hole = zcyl(HOLE_R, WALL_IN - 1, FL_BACK + 0.01)
    return placed(pocket.fuse(hole).removeSplitter())


def web_stiffness(a=SLEEVE_IR, b=HUB_R, t=WEB_T, E=26.0, nu=0.45):
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
    """SW101, the PCB and nearby components from the assembly, in FrontCase coordinates."""
    d = App.openDocument(ASSEMBLY)
    link = d.getObject("Part")
    ctrl = link.LinkedObject.Document
    to_fc = App.Placement(FC_OFFSET * -1, App.Rotation()).multiply(link.Placement)
    zone = App.BoundBox(AX_X - 8, 74, AX_Z - 5, AX_X + 8, 83, AX_Z + 5)
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


def check(case, cut_case, grommet, switch, pcb, near):
    import sys
    print = lambda *a: sys.__stdout__.write(" ".join(map(str, a)) + "\n")

    def vol(a, b):
        return a.common(b).Volume

    lobes = placed(fuse_all([zcone(BARB_R, BARB_TIP_R, SHOULDER, SHOULDER - BARB_L, a0, a1)
                             for a0, a1 in fingers_sectors()[0]]))
    print("== interference (mm³, want ~0)")
    print("  grommet / case (cut) %.3f, of which barbs %.3f (preload, intended)"
          % (vol(grommet, cut_case), vol(lobes, cut_case)))
    print("  grommet / switch     %.3f (tip %.2f above actuator)" % (vol(grommet, switch), PREGAP))
    print("  grommet / pcb        %.3f" % vol(grommet, pcb))
    for label, s in near:
        v = vol(grommet, s)
        if v > 1e-4:
            print("  grommet / %-10s %.3f" % (label, v))
    inner = grommet.common(Part.makeBox(100, 30, 40, V(-60, WALL_IN - 30, -10)))
    print("== geometry")
    print("  flange z %.2f..%.2f (outer wall flat below %.1f), land %.2f"
          % (AX_Z - FLANGE_R, AX_Z + FLANGE_R, FILLET_Z, FLANGE_R - HOLE_R))
    print("  behind the wall: z %.2f..%.2f (free %.2f..%.2f), down to y %.2f, PCB edge y %.2f"
          % (inner.BoundBox.ZMin, inner.BoundBox.ZMax, AX_Z - BAND, AX_Z + BAND,
             inner.BoundBox.YMin, pcb.BoundBox.YMax))
    print("  collar lands on the lips after %.2f (switch makes at %.2f)" % (STOP_GAP, PREGAP + TRAVEL))
    print("  barbs fold in %.2f to pass the hole, room to the stem %.2f"
          % (BARB_R - HOLE_R, SLEEVE_IR - STEM_R))
    k = web_stiffness()
    print("== feel (95A, E 26 MPa, estimate)")
    print("  web %.2f N/mm: +%.2f N at click, +%.2f N at stop, switch 1.6 N"
          % (k, k * (PREGAP + TRAVEL), k * STOP_GAP))


def build():
    for d in list(App.listDocuments().values()):
        if d.Label == "PowerGrommet":
            App.closeDocument(d.Name)
    case = frontcase_shape()
    switch, pcb, near = reference_parts()
    doc = App.newDocument("PowerGrommet")
    doc.saveAs(OUT)

    grommet = make_grommet()
    cut = make_wall_cut()
    cut_case = case.cut(cut)
    add(doc, "Grommet", grommet, (0.85, 0.45, 0.1))
    add(doc, "WallCut", cut, (0.9, 0.2, 0.2), 50).Visibility = False
    ref = doc.addObject("App::DocumentObjectGroup", "Reference")
    ref.addObject(add(doc, "FrontCase_cut", cut_case, (0.22, 0.23, 0.25), 60))
    ref.addObject(add(doc, "SW101", switch, (0.6, 0.6, 0.6)))
    ref.addObject(add(doc, "PCB_ref", pcb, (0.1, 0.4, 0.15)))
    doc.recompute()

    check(case, cut_case, grommet, switch, pcb, near)

    if os.environ.get("GROMMET_STL"):
        import Mesh
        out = os.environ["GROMMET_STL"]
        os.makedirs(out, exist_ok=True)
        Mesh.export([doc.getObject("Grommet")], os.path.join(out, "PowerGrommet.stl"))
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
        v.saveImage(os.path.join(outdir, "grommet_" + name + ".png"), 1400, 1000, "White")

    crop = Part.makeBox(16, 12, 16, V(AX_X - 8, 73, AX_Z - 8))
    secs = {}
    for key, half in (("z", Part.makeBox(100, 100, 50, V(-60, 40, AX_Z))),     # through the fingers
                      ("x", Part.makeBox(50, 100, 50, V(AX_X, 40, -20)))):     # through the stop lips
        names = []
        for o in list(doc.Objects):
            if o.TypeId != "Part::Feature" or o.Name == "WallCut" or o.Name.startswith("Sec"):
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
    shot("section_top", Rot(V(0, 0, 1), 0))
    show(secs["x"])
    shot("section_side", Rot(V(0, 1, 0), V(0, 0, 1), V(1, 0, 0), "ZYX"))
    show(["Grommet"])
    shot("back", Rot(V(1, 0, 0), V(0, 0, 1), V(0, -1, 0), "ZYX").multiply(Rot(V(1, 0, 0), 25)).multiply(Rot(V(0, 1, 0), 30)))

    for names in secs.values():
        for n in names:
            doc.removeObject(n)
    for o in doc.Objects:
        if o.TypeId == "Part::Feature":
            o.ViewObject.Visibility = o.Name != "WallCut"
    doc.recompute()


if __name__ in ("__main__", "power_grommet"):
    doc = build()
    doc.save()
    if GUI and os.environ.get("GROMMET_RENDER"):
        render(doc, os.environ["GROMMET_RENDER"])
        doc.save()
    print("GROMMET DONE", flush=True)
    if os.environ.get("GROMMET_EXIT"):
        os._exit(0)
