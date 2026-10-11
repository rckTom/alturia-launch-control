"""Look-and-feel mockup of the Alturia Launch Control handheld controller.

Run with the FreeCAD GUI (colours and links need it), e.g. headless:
    xvfb-run -a freecad hardware/controller/cad/controller_mockup.py
It writes controller_mockup.FCStd next to this file and, if MOCKUP_RENDER is
set to a directory, preview PNGs into it.

Frame: x right, y up (y = 0 is the top face), z out of the front (z = 0 is the
front face), seen from the front. Layout values below are "from the top"
(down = positive) to read like a drawing.

The case shells are plain Part solids meant to be redrawn in PartDesign once
the layout settles; the bought parts are App::Links to their own models
(Delock 22032 from the satellite); the Sharp breakout is copied in from
Adafruit_4694_Sharp27.FCStd so it can be coloured.
"""
import math
import os

import FreeCAD as App
import FreeCADGui as Gui
import Part
from FreeCAD import Vector as V, Rotation as Rot, Placement as Pl

HERE = os.path.dirname(os.path.abspath(__file__))
SAT_CAD = os.path.normpath(os.path.join(HERE, "../../satellite/cad"))
OUT = os.path.join(HERE, "controller_mockup.FCStd")
FONT = "/usr/share/fonts/noto/NotoSans-SemiBold.ttf"

# ---- case ---------------------------------------------------------------
W, H, D = 86.0, 192.0, 24.0   # outer size
T = 2.5                       # wall
R_PLAN = 10.0                 # corner radius seen from the front
R_EDGE = 3.0                  # front/back edge round
SEAM = -13.0                  # front shell / back cover split
ENGRAVE = 0.4

# ---- stack (z) ----------------------------------------------------------
GLASS_TOP = -T                 # glass against the inside of the front wall
DISP_PCB_FRONT = GLASS_TOP - 1.79   # see Adafruit_4694_Sharp27 (glass+tape)
HDR_BODY = 2.5
CTRL_PCB_FRONT = DISP_PCB_FRONT - 1.6 - HDR_BODY
CTRL_PCB_T = 1.6
TACT_H = 1.5                   # TS-1187A

# ---- layout, (x, down from top) -----------------------------------------
DISP_CENTER = (0.0, 38.0)      # Adafruit board centre
ACTIVE = (58.8, 35.28, 2.03)   # w, h, offset of the active area towards +y
WIN_MARGIN = 0.75
DPAD = (-18.0, 100.0)
DPAD_PITCH = 11.0
BACK = (22.0, 94.0)
LEDS = [("STAT", (16.0, 112.0), (0.1, 0.8, 0.2)),
        ("CHG", (28.0, 112.0), (0.9, 0.15, 0.1))]
GRILLE = (0.0, 73.0)
FILL = (-20.0, 140.0)
VENT = (20.0, 140.0)
ARM = (-20.0, 166.0)
FIRE = (20.0, 166.0)
FIRE_GUARD = (12.0, 15.0, 9.0)  # inner r, outer r, height above the face
CTRL_PCB = (78.0, 8.0, 124.0)   # width, top, bottom (down from top)
USB_DOWN = 118.0               # left wall; z follows the PCB
POWER_DOWN = 50.0               # side-actuated tact switch on the right PCB edge
SIDE_SW = (6.0, 3.5, 3.5)       # body along y, x, z (generic right-angle SMD tact)
SMA_TOP = (26.0, -14.0)         # top face: x, z
BATTERY = (60.0, 90.0, 6.0, 20.0)  # w, h, thickness, top (down) - 606090, 4 Ah

DELOCK_HOLE = 12.2
DELOCK_FACE = 1.4   # model z of the gasket underside = panel front


def P(x, down):
    return V(x, -down, 0)


def box(x0, x1, y0, y1, z0, z1):
    return Part.makeBox(x1 - x0, y1 - y0, z1 - z0, V(x0, y0, z0))


def cyl(r, c, z0, z1, axis=V(0, 0, 1)):
    return Part.makeCylinder(r, z1 - z0, c + axis * z0, axis)


def rounded_rect(w, h, r, z0, z1, cx=0.0, cy=-H / 2):
    b = box(cx - w / 2, cx + w / 2, cy - h / 2, cy + h / 2, z0, z1)
    edges = [e for e in b.Edges
             if abs(e.Vertexes[0].Point.z - e.Vertexes[1].Point.z) > 1e-6]
    return b.makeFillet(r, edges)


def text_solid(doc, s, size, x, down, depth=ENGRAVE, z=0.0):
    import Draft
    App.setActiveDocument(doc.Name)
    ss = Draft.make_shapestring(String=s, FontFile=FONT, Size=size)
    doc.recompute()
    sh = ss.Shape.copy()
    doc.removeObject(ss.Name)
    bb = sh.BoundBox
    sh.translate(V(x - (bb.XMin + bb.XMax) / 2, -down - size / 2 - bb.YMin, z))
    return sh.extrude(V(0, 0, -depth)).translate(V(0, 0, 0.01))


def add(doc, name, shape, color, transparency=0):
    o = doc.addObject("Part::Feature", name)
    o.Shape = shape
    o.ViewObject.ShapeColor = color
    o.ViewObject.Transparency = transparency
    return o


def link(doc, name, target, placement):
    o = doc.addObject("App::Link", name)
    o.setLink(target)
    o.Placement = placement
    return o


def build():
    for d in list(App.listDocuments().values()):
        if d.Label == "controller_mockup":
            App.closeDocument(d.Name)
    doc = App.newDocument("controller_mockup")
    doc.saveAs(OUT)  # external links need a saved owner for relative paths
    tmp = App.newDocument("mockup_tmp")

    delock_doc = App.openDocument(os.path.join(SAT_CAD, "Delock_22032.FCStd"))
    delock = delock_doc.getObject("Delock_22032")
    disp_doc = App.openDocument(os.path.join(HERE, "Adafruit_4694_Sharp27.FCStd"))

    # ---- shell ----------------------------------------------------------
    outer = rounded_rect(W, H, R_PLAN, -D, 0)
    flat = [e for e in outer.Edges
            if abs(e.BoundBox.ZMax - e.BoundBox.ZMin) < 1e-6]
    outer = outer.makeFillet(R_EDGE, flat)
    inner = rounded_rect(W - 2 * T, H - 2 * T, R_PLAN - T, -D + T, -T)
    shell = outer.cut(inner)

    bosses = []
    for sx in (-1, 1):
        for down in (9.0, H - 9.0):
            bosses.append(cyl(3.5, P(sx * (W / 2 - 7), down), -D + T, -T))
    for b in bosses:
        shell = shell.fuse(b)

    cuts = []
    # display window with a 45 degree bevel
    ax, ay = DISP_CENTER[0], DISP_CENTER[1] - ACTIVE[2]
    ww, wh = ACTIVE[0] + 2 * WIN_MARGIN, ACTIVE[1] + 2 * WIN_MARGIN
    win = box(-ww / 2, ww / 2, -wh / 2, wh / 2, -T - 1, 1)
    bevel = Part.makeLoft([
        Part.makePolygon([V(-ww / 2 - s, -wh / 2 - s, z), V(ww / 2 + s, -wh / 2 - s, z),
                          V(ww / 2 + s, wh / 2 + s, z), V(-ww / 2 - s, wh / 2 + s, z),
                          V(-ww / 2 - s, -wh / 2 - s, z)])
        for s, z in ((0, -1.2), (1.6, 0.4))], True)
    cuts += [win.translate(P(ax, ay)), bevel.translate(P(ax, ay))]

    # navigation caps (TS-1187A on the controller PCB)
    caps = []
    dx, dy = DPAD
    keys = [(name, (dx + ox * DPAD_PITCH, dy + oy * DPAD_PITCH), "sq", 1.2)
            for name, (ox, oy) in (("Up", (0, -1)), ("Down", (0, 1)),
                                   ("Left", (-1, 0)), ("Right", (1, 0)))]
    keys += [("Enter", (dx, dy), "rd", 1.2), ("Back", BACK, "rd", 1.2),
             ]
    for name, pos, shape, top in keys:
        c = P(*pos)
        bottom = CTRL_PCB_FRONT + TACT_H
        if shape == "sq":
            s = 7.6
            stem = rounded_rect(s, s, 1.5, bottom, top, c.x, c.y)
            flange = rounded_rect(s + 2, s + 2, 2.0, -T - 0.8, -T - 0.05, c.x, c.y)
            cuts.append(rounded_rect(s + 0.5, s + 0.5, 1.7, -T - 1, 1, c.x, c.y))
        else:
            stem = cyl(4.5, c, bottom, top)
            flange = cyl(5.5, c, -T - 0.8, -T - 0.05)
            cuts.append(cyl(4.75, c, -T - 1, 1))
        cap = stem.fuse(flange).removeSplitter()
        try:
            cap = cap.makeFillet(0.6, [e for e in cap.Edges
                                       if abs(e.BoundBox.ZMin - top) < 1e-6])
        except Exception:
            pass
        caps.append((name, cap))

    for _, (x, down), _ in LEDS:
        cuts.append(cyl(1.6, P(x, down), -T - 1, 1))
    for i in range(-3, 4):
        cuts.append(rounded_rect(1.2, 8, 0.59, -T - 1, 1, GRILLE[0] + 2.4 * i, -GRILLE[1]))

    # panel buttons
    for (x, down) in (FILL, VENT, ARM, FIRE):
        cuts.append(cyl(DELOCK_HOLE / 2, P(x, down), -T - 1, 1))
    # power: cap through the right wall onto a side-actuated switch
    pz = CTRL_PCB_FRONT + SIDE_SW[2] / 2
    pcb_edge = CTRL_PCB[0] / 2

    def along_x(w_y, h_z, r, x0, x1):
        prism = rounded_rect(h_z, w_y, r, x0, x1, 0, 0)  # built along z
        return prism.rotate(V(0, 0, 0), V(0, 1, 0), 90).translate(V(0, -POWER_DOWN, pz))

    cuts.append(along_x(8.4, 4.0, 1.9, W / 2 - T - 1, W / 2 + 1))
    pwr_cap = along_x(8.0, 3.6, 1.7, pcb_edge + 1.0, W / 2 + 1.0).fuse(
        along_x(10.0, 5.6, 2.0, pcb_edge + 0.3, W / 2 - T - 0.05)).removeSplitter()
    sma_c = V(SMA_TOP[0], 0, SMA_TOP[1])
    cuts.append(cyl(3.3, sma_c, -T - 1, 1, V(0, 1, 0)))

    # USB-C on the left wall, centred on the receptacle (3.2 mm tall, top mount)
    usb_z = CTRL_PCB_FRONT + 1.6
    usb = Part.makeBox(T + 2, 9.8, 4.0, V(-W / 2 - 1, -USB_DOWN - 4.9, usb_z - 2.0))
    cuts.append(usb)

    # engraving
    labels = [("FILL", 3.2, FILL[0], FILL[1] - 14.5), ("VENT", 3.2, VENT[0], VENT[1] - 14.5),
              ("ARM", 3.2, ARM[0], 185.0),
              ("FIRE", 3.2, FIRE[0], 185.0),
              ("BACK", 2.4, BACK[0], BACK[1] + 8.0),
              ("STAT", 2.0, LEDS[0][1][0], LEDS[0][1][1] + 4.5),
              ("CHG", 2.0, LEDS[1][1][0], LEDS[1][1][1] + 4.5),
              ("ALTURIA LAUNCH CONTROL", 2.6, 0.0, 9.0)]
    for s, size, x, down in labels:
        cuts.append(text_solid(tmp, s, size, x, down))

    for c in cuts:
        shell = shell.cut(c)

    guard = cyl(FIRE_GUARD[1], P(*FIRE), -0.5, FIRE_GUARD[2]).cut(
        cyl(FIRE_GUARD[0], P(*FIRE), -1, FIRE_GUARD[2] + 1))
    try:
        guard = guard.makeFillet(1.0, [e for e in guard.Edges
                                       if abs(e.BoundBox.ZMin - FIRE_GUARD[2]) < 1e-6])
    except Exception:
        pass

    split = box(-W, W, -2 * H, H, SEAM, 10)
    front = shell.common(split).fuse(guard).removeSplitter()
    back = shell.cut(split).removeSplitter()

    add(doc, "FrontShell", front, (0.22, 0.23, 0.25))
    add(doc, "BackCover", back, (0.30, 0.31, 0.33))
    for name, cap in caps:
        add(doc, "Key_" + name, cap, (0.12, 0.12, 0.13))
    add(doc, "Key_Power", pwr_cap, (0.12, 0.12, 0.13))
    for name, (x, down), col in LEDS:
        add(doc, "LightPipe_" + name, cyl(1.5, P(x, down), CTRL_PCB_FRONT + 0.8, 0.3), col, 40)

    # ---- electronics ----------------------------------------------------
    pcb = rounded_rect(CTRL_PCB[0], CTRL_PCB[2] - CTRL_PCB[1], 2.0,
                       CTRL_PCB_FRONT - CTRL_PCB_T, CTRL_PCB_FRONT,
                       0, -(CTRL_PCB[1] + CTRL_PCB[2]) / 2)
    for sx in (-1, 1):
        pcb = pcb.cut(cyl(4.5, P(sx * (W / 2 - 7), 9.0), -D, 0))
    add(doc, "ControllerPCB", pcb, (0.05, 0.35, 0.15))
    pb = CTRL_PCB_FRONT - CTRL_PCB_T
    add(doc, "ESP32_S3_WROOM_1", box(4, 22, -60, -34.5, pb - 3.1, pb), (0.6, 0.6, 0.62))
    add(doc, "Wio_SX1262", box(-24, -6, -58, -40, pb - 2.5, pb), (0.6, 0.6, 0.62))
    for _, (x, down), _, _ in keys:
        add(doc, "TS1187A_%d_%d" % (x, down),
            box(-2.55, 2.55, -2.55, 2.55, CTRL_PCB_FRONT, CTRL_PCB_FRONT + TACT_H)
            .translate(P(x, down)), (0.75, 0.75, 0.75))
    sw_l, sw_w, sw_h = SIDE_SW
    add(doc, "SideSwitch_Power",
        box(pcb_edge - sw_w + 0.2, pcb_edge + 0.2, -POWER_DOWN - sw_l / 2, -POWER_DOWN + sw_l / 2,
            CTRL_PCB_FRONT, CTRL_PCB_FRONT + sw_h), (0.75, 0.75, 0.75))
    add(doc, "USB_C", box(-W / 2 + T + 0.2, -W / 2 + T + 7.5, -USB_DOWN - 4.47, -USB_DOWN + 4.47,
                          usb_z - 1.6, usb_z + 1.6), (0.8, 0.8, 0.82))
    bw, bh, bt, btop = BATTERY
    bz1 = pb - 3.6
    add(doc, "LiPo", rounded_rect(bw, bh, 2.0, bz1 - bt, bz1, 0, -btop - bh / 2),
        (0.75, 0.75, 0.80))

    # Sharp display: board centre to DISP_CENTER, PCB front to DISP_PCB_FRONT
    dpl = V(DISP_CENTER[0], -DISP_CENTER[1], DISP_PCB_FRONT)
    colors = {"PCB": (0.05, 0.05, 0.08), "LCD_glass": (0.55, 0.58, 0.55),
              "Polarizer_border": (0.35, 0.37, 0.35), "Active_area": (0.70, 0.72, 0.66),
              "FPC": (0.85, 0.55, 0.15), "Rear_parts_keepout": (0.2, 0.2, 0.2),
              "Header_body": (0.1, 0.1, 0.1), "Header_pins": (0.85, 0.75, 0.3)}
    for o in disp_doc.Objects:
        if o.TypeId == "Part::Feature":
            add(doc, "Display_" + o.Name, o.Shape.copy().translate(dpl),
                colors.get(o.Name, (0.5, 0.5, 0.5)))

    # ---- Delock 22032 buttons -------------------------------------------
    for name, (x, down) in (("FILL", FILL), ("VENT", VENT), ("ARM", ARM), ("FIRE", FIRE)):
        link(doc, "Button_" + name, delock, Pl(P(x, down) + V(0, 0, DELOCK_FACE), Rot()))

    # SMA bulkhead + 868 MHz whip on the top face
    ant = cyl(4.0, sma_c, 0, 8, V(0, 1, 0)).fuse(
        cyl(6.5, sma_c, 8, 22, V(0, 1, 0))).fuse(
        cyl(5.0, sma_c, 22, 120, V(0, 1, 0))).fuse(
        Part.makeSphere(5.0, sma_c + V(0, 120, 0)))
    add(doc, "Antenna", ant.removeSplitter(), (0.08, 0.08, 0.08))
    add(doc, "SMA_bulkhead", cyl(3.2, sma_c, -9, 1, V(0, 1, 0)), (0.85, 0.7, 0.3))

    App.closeDocument(tmp.Name)
    doc.recompute()
    return doc


def render(doc, outdir):
    Gui.ActiveDocument = Gui.getDocument(doc.Name)
    v = Gui.ActiveDocument.ActiveView
    v.setCameraType("Orthographic")
    v.setAxisCross(False)

    def shot(name, rot):
        v.setCameraOrientation(rot)
        for _ in range(3):
            Gui.updateGui()
            v.fitAll()
        v.saveImage(os.path.join(outdir, "mockup_" + name + ".png"), 1200, 1200, "White")

    # identity = looking down -z, i.e. at the front face
    front = Rot()
    three_q = Rot(V(0, 1, 0), 28).multiply(Rot(V(1, 0, 0), -22))
    shot("front", front)
    shot("threequarter", three_q)
    shot("right", Rot(V(0, 1, 0), 90))
    shot("top", Rot(V(1, 0, 0), -90))
    shot("back", Rot(V(0, 1, 0), 180))
    shell = doc.getObject("FrontShell").ViewObject
    shell.Visibility = False
    shot("inside", three_q)
    shell.Visibility = True


if __name__ in ("__main__", "controller_mockup"):
    import sys
    doc = build()
    doc.save()
    if os.environ.get("MOCKUP_RENDER"):
        render(doc, os.environ["MOCKUP_RENDER"])
    print("MOCKUP DONE", flush=True)
    if os.environ.get("MOCKUP_EXIT"):
        os._exit(0)
