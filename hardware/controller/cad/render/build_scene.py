"""Build the photoreal Blender scene of the controller assembly.

    blender -b --factory-startup --python build_scene.py [-- --render [names]]
        [--eevee] [--scale=50] [--samples=64]

--eevee renders with EEVEE on the GPU instead of Cycles on the CPU: much
faster; the acrylic window is shaded as thin glass without refraction.

Cameras: hero, front, detail, top show the assembled controller; exploded
pulls the parts apart along z (front up) in assembly order, see EXPLODE.

Inputs, all in this directory:
  assembly.glb   ControllerAssembly.FCStd (export_glb.py); sometimes without
                 the PCB link, depending on what FreeCAD had loaded
  pcb.glb        the PCB link target, Controller.FCStd#Part, used then
  screen.png     a 400x240 frame from the controller firmware under radiosim
  engraving.png  frontcase_engraving.svg engrave layer, 40 px/mm

Writes controller.blend; with --render also renders the cameras to
render_<camera>.png. Blender units are metres (glTF), 1 mm = 0.001.
"""

import math
import os
import re
import sys
from pathlib import Path

import bmesh
import bpy
from mathutils import Matrix, Vector

HERE = Path(__file__).resolve().parent
MM = 0.001

# Controller#Part link placement in ControllerAssembly (mm)
PCB_AT = Vector((26.4609, 80.46, 4.90679)) * MM
# PCB outline in assembly coordinates (mm): parts outside are unplaced
BOARD = (-38.54, -75.54, 26.46, 80.46)
# Centre of the FrontCase flat top face; z is read from the geometry below
FRONT_AT = Vector((-6.03913, 2.96004, 0)) * MM
FACE_W, FACE_H = 69.92, 158.92
# Display active area (LS027B7DH01, 58.8 x 35.28 mm) on the glass; z below
SCREEN_C = Vector((-6.04, 45.2, 0)) * MM
SCREEN_W, SCREEN_H = 58.8, 35.28

# ---- scene ----------------------------------------------------------------

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.unit_settings.length_unit = 'MILLIMETERS'

root = bpy.data.objects.new("Controller", None)
scene.collection.objects.link(root)


def import_glb(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(path))
    return [o for o in bpy.data.objects if o not in before]


# KiCad StepUp names the board "<board>_PCB" and its copper "<board>_pad",
# and the board name follows whatever file KiCad exported from
def is_board(o):
    return re.search(r"_PCB(\.\d+)?$", o.name) is not None


def is_pads(o):
    return re.search(r"_pad(\.\d+)?$", o.name) is not None


asm = import_glb(HERE / "assembly.glb")
for o in asm:
    if o.parent is None:
        o.parent = root

pcb_link = next((o for o in asm if is_board(o)), None)
if pcb_link is not None:
    # this export kept the PCB link: everything under the link's node
    while pcb_link.parent is not None and pcb_link.parent.name != "Assembly":
        pcb_link = pcb_link.parent
    pcb = [pcb_link] + list(pcb_link.children_recursive)
else:
    pcb_root = bpy.data.objects.new("PCB", None)
    scene.collection.objects.link(pcb_root)
    pcb_root.parent = root
    pcb_root.location = PCB_AT
    pcb = import_glb(HERE / "pcb.glb")
    for o in pcb:
        if o.parent is None:
            o.parent = pcb_root
bpy.context.view_layer.update()


def on_board(p):
    return BOARD[0] * MM < p.x < BOARD[2] * MM and BOARD[1] * MM < p.y < BOARD[3] * MM


# Parts not yet placed in KiCad sit around the board: drop them, and the
# pads that go with them.
for o in pcb:
    if o.type != 'MESH':
        continue
    if is_pads(o):
        bm = bmesh.new()
        bm.from_mesh(o.data)
        mw = o.matrix_world
        bmesh.ops.delete(bm, geom=[f for f in bm.faces
                                   if not on_board(mw @ f.calc_center_median())],
                         context='FACES')
        bm.to_mesh(o.data)
        bm.free()
        continue
    c = sum((o.matrix_world @ Vector(b) for b in o.bound_box), Vector()) / 8
    if not on_board(c):
        bpy.data.objects.remove(o)


def z_range(o):
    z = [(o.matrix_world @ Vector(b)).z for b in o.bound_box]
    return min(z), max(z)


# Heights follow the assembly: the flat top face is the front case's highest
# point, the screen 0.1 mm over the display glass (closer z-fights in EEVEE),
# the floor under the back.
FRONT_AT.z = z_range(bpy.data.objects["Body"])[1]
SCREEN_C.z = z_range(next(o for o in bpy.data.objects if o.type == 'MESH'
                          and o.name.startswith("Open CASCADE STEP translator")))[1] + 0.1 * MM
# glTF node names are FreeCAD labels: BackCase's link is BackShell (was Body003)
BACK = next(o for o in bpy.data.objects if o.type == 'MESH'
            and re.match(r"(BackShell|Body003)(\.\d+)?$", o.name))
CASE_BOTTOM = z_range(BACK)[0]

# ---- materials --------------------------------------------------------------


def new_mat(name):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    return m, m.node_tree.nodes, m.node_tree.links, m.node_tree.nodes["Principled BSDF"]


def principled(name, color, rough, metal=0.0, **kw):
    m, nodes, links, b = new_mat(name)
    b.inputs["Base Color"].default_value = (*color, 1)
    b.inputs["Roughness"].default_value = rough
    b.inputs["Metallic"].default_value = metal
    for k, v in kw.items():
        b.inputs[k].default_value = v
    return m


def case_plastic(name, color, engrave=None):
    """Matte printed plastic with 0.2 mm layer lines, optional laser marks."""
    m, nodes, links, b = new_mat(name)
    b.inputs["Base Color"].default_value = (*color, 1)
    b.inputs["Roughness"].default_value = 0.55
    b.inputs["Specular IOR Level"].default_value = 0.4

    tc = nodes.new("ShaderNodeTexCoord")
    sep = nodes.new("ShaderNodeSeparateXYZ")
    links.new(tc.outputs["Object"], sep.inputs[0])
    tc.object = root
    # layer lines: sine bands along z, 0.2 mm pitch
    mul = nodes.new("ShaderNodeMath")
    mul.operation = 'MULTIPLY'
    mul.inputs[1].default_value = 2 * math.pi / (0.2 * MM)
    links.new(sep.outputs["Z"], mul.inputs[0])
    sine = nodes.new("ShaderNodeMath")
    sine.operation = 'SINE'
    links.new(mul.outputs[0], sine.inputs[0])
    bump = nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.12
    bump.inputs["Distance"].default_value = 0.02 * MM
    links.new(sine.outputs[0], bump.inputs["Height"])
    links.new(bump.outputs["Normal"], b.inputs["Normal"])

    if engrave is not None:
        ref = engrave
        tc2 = nodes.new("ShaderNodeTexCoord")
        tc2.object = ref
        mp = nodes.new("ShaderNodeMapping")
        mp.inputs["Location"].default_value = (0.5, 0.5, 0)
        mp.inputs["Scale"].default_value = (1 / (FACE_W * MM), 1 / (FACE_H * MM), 1)
        links.new(tc2.outputs["Object"], mp.inputs["Vector"])
        img = nodes.new("ShaderNodeTexImage")
        img.image = bpy.data.images.load(str(HERE / "engraving.png"))
        img.image.colorspace_settings.name = 'Non-Color'
        img.extension = 'CLIP'
        img.interpolation = 'Cubic'
        links.new(mp.outputs[0], img.inputs[0])
        # only on the flat top face
        sep2 = nodes.new("ShaderNodeSeparateXYZ")
        links.new(tc2.outputs["Object"], sep2.inputs[0])
        top = nodes.new("ShaderNodeMath")
        top.operation = 'GREATER_THAN'
        top.inputs[1].default_value = -0.03 * MM
        links.new(sep2.outputs["Z"], top.inputs[0])
        inv = nodes.new("ShaderNodeMath")       # marks are black in the PNG
        inv.operation = 'SUBTRACT'
        inv.inputs[0].default_value = 1.0
        links.new(img.outputs["Color"], inv.inputs[1])
        mask = nodes.new("ShaderNodeMath")
        mask.operation = 'MULTIPLY'
        links.new(inv.outputs[0], mask.inputs[0])
        links.new(top.outputs[0], mask.inputs[1])

        col = nodes.new("ShaderNodeMix")
        col.data_type = 'RGBA'
        col.inputs["A"].default_value = (*color, 1)
        col.inputs["B"].default_value = (0.36, 0.36, 0.35, 1)  # frosted mark
        links.new(mask.outputs[0], col.inputs["Factor"])
        links.new(col.outputs["Result"], b.inputs["Base Color"])
        rough = nodes.new("ShaderNodeMapRange")
        rough.inputs["To Min"].default_value = 0.55
        rough.inputs["To Max"].default_value = 0.85
        links.new(mask.outputs[0], rough.inputs["Value"])
        links.new(rough.outputs["Result"], b.inputs["Roughness"])
        # marks sit a few hundredths of a mm into the surface
        height = nodes.new("ShaderNodeMath")
        height.operation = 'SUBTRACT'
        links.new(sine.outputs[0], height.inputs[0])
        links.new(mask.outputs[0], height.inputs[1])
        links.new(height.outputs[0], bump.inputs["Height"])
        bump.inputs["Distance"].default_value = 0.03 * MM
    return m


engrave_ref = bpy.data.objects.new("EngraveRef", None)
scene.collection.objects.link(engrave_ref)
engrave_ref.parent = root
engrave_ref.location = FRONT_AT
engrave_ref.empty_display_size = 0.01

CASE = (0.011, 0.012, 0.013)
M = {
    "front": case_plastic("FrontCase", CASE, engrave_ref),
    "back": case_plastic("BackCase", CASE),
    "acrylic": principled("Acrylic", (1, 1, 1), 0.02, **{
        "Transmission Weight": 1.0, "IOR": 1.49}),
    "steel": principled("Stainless", (0.62, 0.61, 0.6), 0.28, 1.0,
                        **{"Anisotropic": 0.5}),
    "screw": principled("Steel", (0.5, 0.5, 0.5), 0.35, 1.0),
    "fire": principled("ActuatorFire", (0.6, 0.02, 0.015), 0.3, 0.7),
    "fill": principled("ActuatorFillVent", (0.03, 0.4, 0.06), 0.3, 0.7),
    "arm": principled("ActuatorArm", (0.02, 0.08, 0.55), 0.3, 0.7),
    "rubber": principled("Rubber", (0.012, 0.012, 0.012), 0.75),
    "brass": principled("Brass", (0.85, 0.62, 0.3), 0.3, 1.0),
    "gold": principled("Gold", (1.0, 0.77, 0.37), 0.22, 1.0),
    "keycap": principled("Keycap", (0.018, 0.018, 0.02), 0.45,
                         **{"Coat Weight": 0.15}),
    "inlay": principled("KeycapInlay", (0.8, 0.8, 0.78), 0.5),
    "lcd_frame": principled("LCDFrame", (0.01, 0.01, 0.011), 0.15),
    "enig": principled("ENIG", (1.0, 0.8, 0.45), 0.25, 1.0),
    "mask": principled("SolderMask", (0.03, 0.15, 0.06), 0.3,
                       **{"Coat Weight": 0.5}),
}


def screen_mat():
    """Memory LCD: reflective grey pixels behind glass."""
    m, nodes, links, b = new_mat("MemoryLCD")
    img = nodes.new("ShaderNodeTexImage")
    img.image = bpy.data.images.load(str(HERE / "screen.png"))
    img.interpolation = 'Closest'
    ramp = nodes.new("ShaderNodeMix")
    ramp.data_type = 'RGBA'
    ramp.inputs["A"].default_value = (0.012, 0.013, 0.014, 1)   # pixel on
    ramp.inputs["B"].default_value = (0.42, 0.43, 0.41, 1)      # reflector
    links.new(img.outputs["Color"], ramp.inputs["Factor"])
    links.new(ramp.outputs["Result"], b.inputs["Base Color"])
    b.inputs["Roughness"].default_value = 0.6
    b.inputs["Coat Weight"].default_value = 1.0
    b.inputs["Coat Roughness"].default_value = 0.03
    return m


def assign(o, mat):
    if o.data.users > 1:    # instances of one FreeCAD link share their mesh
        o.data = o.data.copy()
    o.data.materials.clear()
    o.data.materials.append(mat)


for o in list(bpy.data.objects):
    if o.type != 'MESH':
        continue
    n = o.name
    if n == "Body":
        assign(o, M["front"])
    elif o is BACK:
        assign(o, M["back"])
    elif re.match(r"M\d+x\d+-Screw", n):     # Fasteners workbench screws
        assign(o, M["screw"])
    elif n == "Body002":
        assign(o, M["acrylic"])
    elif n in ("Membrane", "MembraneRetainer", "Body004", "Body005"):
        assign(o, M["rubber"])      # keypad membrane and retainer
    elif n.startswith("Insert_M2"):     # heat-set inserts
        assign(o, M["brass"])
    elif n == "Body001":
        assign(o, M["gold"])
    elif n.startswith(("Housing", "Nut")):
        assign(o, M["steel"])
    elif n.startswith("Screws"):
        assign(o, M["screw"])
    elif n.startswith("Actuator"):
        # FILL/VENT upper row, ARM lower left, FIRE lower right
        c = o.matrix_world @ (sum((Vector(b) for b in o.bound_box), Vector()) / 8)
        upper = c.y > -46.5 * MM
        assign(o, M["fill"] if upper else M["arm"] if c.x < -6 * MM else M["fire"])
    elif n.startswith(("Gasket", "Insulator", "Grommet")):
        assign(o, M["rubber"])
    elif n.startswith("Terminals"):
        assign(o, M["brass"])
    elif n.startswith("KeyCapBody"):
        assign(o, M["keycap"])
    elif n.startswith("KeyCapInlay"):
        assign(o, M["inlay"])
    elif n.startswith("Open CASCADE STEP translator"):   # Sharp display
        assign(o, M["lcd_frame"])
    elif is_pads(o):
        assign(o, M["enig"])
    elif is_board(o):
        assign(o, M["mask"])
    # other PCB parts keep their KiCad colours
    if o.data.polygons:
        o.data.shade_smooth()   # glTF carries CAD normals; keep them

# the visible pixels, just above the display glass
bpy.ops.mesh.primitive_plane_add(size=1)
scr = bpy.context.object
scr.name = "Screen"
scr.scale = (SCREEN_W * MM, SCREEN_H * MM, 1)
scr.location = SCREEN_C
scr.parent = root
assign(scr, screen_mat())

# ---- exploded view ---------------------------------------------------------

bpy.context.view_layer.update()     # the screen's matrix_world is stale till then
asm_node = bpy.data.objects["Assembly"]
pcb_top = pcb_link if pcb_link is not None else pcb_root


def nodes(*names):
    """Assembly children by FreeCAD name (Blender may append .001)."""
    pat = re.compile(r"(%s)(\.\d+)?$" % "|".join(names))
    return [o for o in asm_node.children if pat.match(o.name)]


buttons = nodes(r"Delock_\d+")
WINDOW = nodes("Body002")
# Clear acrylic all but vanishes on its own against the backdrop: exploded,
# the window gets a pale blue tint and a little haze so it reads as a part
M["acrylic_exploded"] = principled("AcrylicExploded", (0.72, 0.86, 0.95), 0.08, **{
    "Transmission Weight": 0.8, "IOR": 1.49, "Coat Weight": 0.5})
# Bottom to top, the order the controller goes together face down. Each
# group rises just far enough to clear, by EXPLODE_GAP, every group before
# it that it overlaps in plan view; "with" keeps a group at another's
# height, "under" hangs it "gap" mm below another, "shift" moves it sideways
# (mm) and "gap" widens its own gap. Empties
# that belong to a part (engraving projector, screen) ride along.
EXPLODE_GAP = 14
EXPLODE = [
    # M2x12 from the back through the back case into the front case
    ("case screws", nodes(r"M2x12-Screw\d*"), {}),
    ("back", [BACK], {}),
    # M2x8 from under the PCB through PCB and retainer into the inserts in
    # the front case: they clamp the board too
    ("retainer screws", nodes(r"M2x8-Screw\d*"), {}),
    # with the SMA pigtail; wider, so the screws under it stay in view
    ("pcb", [pcb_top, scr] + nodes("Body001"), {"gap": 22}),
    ("grommet", nodes("PowerGrommet"), {"with": "pcb", "shift": (0, 22, 0)}),
    ("retainer", nodes("MembraneRetainer", "Body005"), {}),
    ("membrane", nodes("Membrane", "Body004"), {}),
    ("keycaps", nodes(r"Keycap\d*"), {}),
    ("nuts", [c for b in buttons for c in b.children if c.name.startswith("Nut")], {}),
    # the acrylic window goes into the front case from the back, over the
    # display
    ("window", WINDOW, {}),
    # wider, so the keycaps under it stay in view
    ("front", nodes("Body") + [engrave_ref], {"gap": 24}),
    # heat-set inserts, just under their bosses in the front case
    ("inserts", nodes(r"Insert_M2 v\d*"), {"under": "front", "gap": 5}),
    ("buttons", buttons, {}),
]


def world_bounds(objs):
    vs = [m.matrix_world @ Vector(c) for o in objs
          for m in [o] + list(o.children_recursive) if m.type == 'MESH'
          for c in m.bound_box]
    return (Vector([min(v[i] for v in vs) for i in range(3)]),
            Vector([max(v[i] for v in vs) for i in range(3)]))


def explode_offsets():
    """Offsets per group, and the exploded bounds of each group's meshes.

    Overlap is tested mesh by mesh: a group's overall box (a long key
    switch, a row of screws) would push the next group needlessly high."""
    off, boxes = {}, {}
    for name, objs, opt in EXPLODE:
        s = Vector(opt.get("shift", (0, 0, 0))) * MM
        mine = [(lo + s, hi + s) for o in objs
                for m in [o] + list(o.children_recursive) if m.type == 'MESH'
                for lo, hi in [world_bounds([m])]]
        if "with" in opt:
            dz = off[opt["with"]].z
        elif "under" in opt:
            # tops this far below the lowest point of that group
            bottom = min(lo.z for lo, _ in boxes[opt["under"]])
            dz = bottom - opt.get("gap", EXPLODE_GAP) * MM - max(hi.z for _, hi in mine)
        else:
            gap = opt.get("gap", EXPLODE_GAP) * MM
            dz = 0.0
            for theirs in boxes.values():
                for lo, hi in mine:
                    for olo, ohi in theirs:
                        if lo.x < ohi.x and olo.x < hi.x and lo.y < ohi.y and olo.y < hi.y:
                            dz = max(dz, ohi.z + gap - lo.z)
        up = Vector((0, 0, dz))
        off[name] = s + up
        boxes[name] = [(lo + up, hi + up) for lo, hi in mine]
    box = {n: (Vector([min(b[0][i] for b in bs) for i in range(3)]),
               Vector([max(b[1][i] for b in bs) for i in range(3)])) for n, bs in boxes.items()}
    return off, box


ASSEMBLED = {o: o.matrix_world.copy() for _, objs, _ in EXPLODE for o in objs}
OFFSETS, EXPLODED_BOX = explode_offsets()


def set_exploded(on):
    # the backdrop wall turns to stand behind the exploded camera's view
    sweep.rotation_euler.z = EX_SWEEP_Z if on else SWEEP_Z
    for w in WINDOW:
        for m in [w] + list(w.children_recursive):
            if m.type == 'MESH':
                m.data.materials[0] = M["acrylic_exploded" if on else "acrylic"]
    # twice: the nuts are children of the buttons, which move too
    for _ in range(2):
        for name, objs, _ in EXPLODE:
            for o in objs:
                t = Matrix.Translation(OFFSETS[name] if on else Vector())
                o.matrix_world = t @ ASSEMBLED[o]
        bpy.context.view_layer.update()


# ---- studio -----------------------------------------------------------------

# seamless sweep: floor under the case, curving up behind the top end
bpy.ops.mesh.primitive_plane_add(size=1)
sweep = bpy.context.object
sweep.name = "Backdrop"
bm = bmesh.new()
R, L, Wd = 0.25, 0.6, 1.4
prof = [(-L, 0.0)] + [(R * math.sin(t), R - R * math.cos(t))
                      for t in [i / 16 * math.pi / 2 for i in range(17)]] + [(R, 0.8)]
rows = []
for y, z in prof:
    rows.append([bm.verts.new((x, y + 0.15, z + CASE_BOTTOM)) for x in (-Wd / 2, Wd / 2)])
for a, b2 in zip(rows, rows[1:]):
    bm.faces.new((a[0], a[1], b2[1], b2[0]))
bm.to_mesh(sweep.data)
bm.free()
sweep.data.shade_smooth()
SWEEP_Z = math.radians(-28)
sweep.rotation_euler.z = SWEEP_Z
bd = principled("Backdrop", (0.62, 0.62, 0.61), 0.6)
assign(sweep, bd)

world = bpy.data.worlds.new("Studio")
scene.world = world
world.use_nodes = True
wn = world.node_tree.nodes
env = wn.new("ShaderNodeTexEnvironment")
env.image = bpy.data.images.load(
    str(next(Path(bpy.utils.system_resource('DATAFILES')).glob(
        "studiolights/world/studio.exr"))))
world.node_tree.links.new(env.outputs["Color"], wn["Background"].inputs["Color"])
wn["Background"].inputs["Strength"].default_value = 0.12


def area_light(name, loc, target, power, size, color=(1, 1, 1)):
    ld = bpy.data.lights.new(name, 'AREA')
    ld.energy = power
    ld.size = size
    ld.color = color
    lo = bpy.data.objects.new(name, ld)
    scene.collection.objects.link(lo)
    lo.location = loc
    lo.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat('-Z', 'Y').to_euler()
    return lo


# mid-height of the case; camera heights are relative to the case
MID_Z = (CASE_BOTTOM + FRONT_AT.z) / 2
CENTER = Vector((-6 * MM, 3 * MM, MID_Z))
area_light("Key", (-0.35, -0.25, 0.45), CENTER, 14, 0.45, (1.0, 0.97, 0.93))
area_light("Fill", (0.4, -0.3, 0.2), CENTER, 3, 0.6, (0.92, 0.95, 1.0))
area_light("Rim", (0.15, 0.45, 0.25), CENTER, 10, 0.25)
# overhead fill would mirror in the display window in the front view
area_light("Top", (0.0, 0.0, 0.6), CENTER, 4, 0.3).visible_glossy = False

# ---- cameras ------------------------------------------------------------


def camera(name, direction, dist, lens, target=CENTER, focus=None, fstop=None, up='Y'):
    cd = bpy.data.cameras.new(name)
    cd.lens = lens
    cd.clip_start = 0.005
    co = bpy.data.objects.new(name, cd)
    scene.collection.objects.link(co)
    d = Vector(direction).normalized()
    co.location = Vector(target) + d * dist
    co.rotation_euler = (Vector(target) - co.location).to_track_quat('-Z', up).to_euler()
    if fstop:
        cd.dof.use_dof = True
        cd.dof.aperture_fstop = fstop
        cd.dof.focus_distance = (Vector(focus or target) - co.location).length
    return co


CAMS = {
    # name: (camera, resolution)
    "hero": (camera("hero", (0.42, -0.62, 0.66), 0.64, 85, fstop=8), (2400, 1600)),
    "front": (camera("front", (0, -0.05, 1), 0.62, 100, target=CENTER + Vector((0, 0, 0))),
              (1400, 2200)),
    "detail": (camera("detail", (0.18, -0.78, 0.5), 0.21, 100,
                      target=Vector((-4 * MM, -52 * MM, FRONT_AT.z - 3.2 * MM)),
                      focus=Vector((12.7 * MM, -66 * MM, FRONT_AT.z + 0.3 * MM)), fstop=5.6), (2400, 1600)),
    "top": (camera("top", (-0.55, 0.62, 0.5), 0.36, 85,
                   target=Vector((-6 * MM, 50 * MM, MID_Z)), fstop=8), (2400, 1600)),
}



def fitted_camera(name, direction, boxes, lens, res, margin=1.1):
    """Camera looking along -direction, pulled back until boxes fill the frame."""
    scene.render.resolution_x, scene.render.resolution_y = res
    corners = [Vector((x, y, z)) for lo, hi in boxes for x in (lo.x, hi.x)
               for y in (lo.y, hi.y) for z in (lo.z, hi.z)]
    lo = Vector([min(v[i] for v in corners) for i in range(3)])
    hi = Vector([max(v[i] for v in corners) for i in range(3)])
    co = camera(name, direction, 1.0, lens, target=(lo + hi) / 2)
    bpy.context.view_layer.update()
    loc, _ = co.camera_fit_coords(bpy.context.evaluated_depsgraph_get(),
                                  [c for v in corners for c in v])
    c = (lo + hi) / 2
    co.location = c + (loc - c) * margin
    return co


# from the right-hand side, a little from the bottom end: the long axis runs
# across the frame and the low angle looks into the gaps
EX_DIR = Vector((0.86, -0.36, 0.36))
EX_RES = (2400, 2000)
CAMS["exploded"] = (fitted_camera("exploded", EX_DIR, list(EXPLODED_BOX.values()), 85,
                                   EX_RES), EX_RES)
# local +y is the wall: put it opposite the camera
EX_SWEEP_Z = math.atan2(EX_DIR.y, EX_DIR.x) + math.pi / 2
EXPLODED_CAMS = {"exploded"}

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
EEVEE = "--eevee" in argv

# leave one core free for FreeCAD; saved in the .blend too
scene.render.threads_mode = 'FIXED'
scene.render.threads = max(1, os.cpu_count() - 1)
scene.cycles.device = 'CPU'
scene.cycles.samples = 256
scene.cycles.use_denoising = True
scene.cycles.denoiser = 'OPENIMAGEDENOISE'
if EEVEE:
    scene.render.engine = 'BLENDER_EEVEE'
    ee = scene.eevee
    ee.taa_render_samples = 128
    ee.use_raytracing = True            # reflections
    ee.ray_tracing_options.resolution_scale = '1'
    ee.use_shadows = True
    ee.shadow_ray_count = 3
    ee.shadow_step_count = 12
    ee.use_fast_gi = True
    # EEVEE's screen-space refraction ghosts the screen behind the 1 mm
    # window, which barely refracts anyway: shade it as thin glass instead
    nt = M["acrylic"].node_tree
    fres = nt.nodes.new("ShaderNodeFresnel")
    fres.inputs["IOR"].default_value = 1.49
    clear = nt.nodes.new("ShaderNodeBsdfTransparent")
    gloss = nt.nodes.new("ShaderNodeBsdfGlossy")
    gloss.inputs["Roughness"].default_value = 0.02
    # gloss on the top face only: seen from inside, the bottom face would
    # be all total internal reflection, a black mirror of the case interior
    geo = nt.nodes.new("ShaderNodeNewGeometry")
    front = nt.nodes.new("ShaderNodeMath")
    front.operation = 'SUBTRACT'
    front.inputs[0].default_value = 1.0
    nt.links.new(geo.outputs["Backfacing"], front.inputs[1])
    fac = nt.nodes.new("ShaderNodeMath")
    fac.operation = 'MULTIPLY'
    nt.links.new(fres.outputs[0], fac.inputs[0])
    nt.links.new(front.outputs[0], fac.inputs[1])
    mix = nt.nodes.new("ShaderNodeMixShader")
    nt.links.new(fac.outputs[0], mix.inputs[0])
    nt.links.new(clear.outputs[0], mix.inputs[1])
    nt.links.new(gloss.outputs[0], mix.inputs[2])
    nt.links.new(mix.outputs[0], nt.nodes["Material Output"].inputs["Surface"])
else:
    scene.render.engine = 'CYCLES'
scene.view_settings.view_transform = 'AgX'
scene.view_settings.look = 'AgX - Medium High Contrast'
scene.render.film_transparent = False
scene.camera = CAMS["hero"][0]
scene.render.resolution_x, scene.render.resolution_y = CAMS["hero"][1]

bpy.ops.wm.save_as_mainfile(filepath=str(HERE / "controller.blend"), compress=True)

if "--render" in argv:
    names = [a for a in argv if not a.startswith("--")] or list(CAMS)
    for k in argv:
        if k.startswith("--scale="):
            scene.render.resolution_percentage = int(k.split("=")[1])
        if k.startswith("--samples="):
            scene.cycles.samples = int(k.split("=")[1])
            scene.eevee.taa_render_samples = int(k.split("=")[1])
    for n in names:
        cam, (rx, ry) = CAMS[n]
        set_exploded(n in EXPLODED_CAMS)
        scene.camera = cam
        scene.render.resolution_x, scene.render.resolution_y = rx, ry
        scene.render.filepath = str(HERE / f"render_{n}.png")
        bpy.ops.render.render(write_still=True)
