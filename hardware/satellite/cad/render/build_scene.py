"""Build the photoreal Blender scene of the satellite, lid off.

    blender -b --factory-startup --python build_scene.py [-- --render [names]]
        [--scale=50] [--samples=64]

Inputs, all in this directory (export_glb.py writes the first two):
  *.glb            one per part, each in its own frame
  placements.json  each part's global placement in SatelliteAssembly (mm)
  engraving.png    frontpanel_engraving.svg engrave layer, 25 px/mm, black on
                   white, made by hand with Inkscape (--export-id=engrave
                   --export-id-only --export-area-page, 5495 x 2920)

SatelliteAssembly is modelled upside down, so the scene turns it over. The
lid has latches on both long sides and no hinge: "open" means lifted off
and set down beside the case, inside up.

Cameras: hero, front, detail show the assembled satellite; exploded pulls
the parts out of the case upward in assembly order, see EXPLODE. The lid
stays where it was set down.

Writes satellite.blend; with --render also renders the cameras to
render_<camera>.png. Blender units are metres (glTF), 1 mm = 0.001.
"""

import json
import math
import os
import sys
from pathlib import Path

import bmesh
import bpy
from mathutils import Matrix, Quaternion, Vector

HERE = Path(__file__).resolve().parent
MM = 0.001
P = json.loads((HERE / "placements.json").read_text())

# Case shell spans z 2.04..119.44 in the assembly; turned over, its floor
# (assembly z = 119.44) goes to z = 0.
UPRIGHT = Matrix.Translation((0, 0, 119.44 * MM)) @ Matrix.Rotation(math.pi, 4, 'X')
# Panel engraving in front_plate Body coordinates: front view is
# (45 - x, -(y + 38.5)) mm, 219.8 x 116.8 mm, visible face at z = 0.
PANEL_W, PANEL_H = 219.8, 116.8

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.unit_settings.length_unit = 'MILLIMETERS'

root = bpy.data.objects.new("Satellite", None)
scene.collection.objects.link(root)
root.matrix_world = UPRIGHT


def placement(name):
    m = Matrix(P[name])
    m.translation *= MM
    return m


def import_part(name, parent, file=None):
    """name's placement on file.glb (default name.glb) under a new empty."""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(HERE / f"{file or name}.glb"))
    new = [o for o in bpy.data.objects if o not in before]
    e = bpy.data.objects.new(name, None)
    scene.collection.objects.link(e)
    e.parent = parent
    e.matrix_parent_inverse = Matrix()
    e.matrix_basis = placement(name) if name in P else Matrix()
    for o in new:
        if o.parent is None:
            o.parent = e
    return e, new


parts = {}
for name in ("case", "screws", "screws40", "battery", "clamp1", "clamp2", "plate_Body026",
             "plate_satellite_1", "plate_Part", "plate_KeySwitch", "plate_Delock_22032",
             "plate_FuseHolder", "plate_FuseGrommet"):
    parts[name] = import_part(name, root)
# the panel's LED grommets: one model, a placement each
LEDS = sorted(k for k in P if k.startswith("led_"))
for name in LEDS:
    parts[name] = import_part(name, root, "led_grommet")

# The lid, lifted off: turned inside up and set down left of the case
lid_e, lid_objs = import_part("lid", None)
bpy.context.view_layer.update()
closed = UPRIGHT @ placement("lid")
lid_e.matrix_world = closed
bpy.context.view_layer.update()
lid_meshes = [o for o in lid_objs if o.type == 'MESH']


def bounds(objs):
    vs = [o.matrix_world @ Vector(c) for o in objs for c in o.bound_box]
    return (Vector([min(v[i] for v in vs) for i in range(3)]),
            Vector([max(v[i] for v in vs) for i in range(3)]))


lo, hi = bounds(lid_meshes)
c = (lo + hi) / 2
# closed it hangs inside-down over the case; flip it about its long axis
turn = Matrix.Rotation(math.radians(-14), 4, 'Z') @ Matrix.Rotation(math.pi, 4, 'X')
lid_e.matrix_world = Matrix.Translation(-c) @ closed
lid_e.matrix_world = turn @ lid_e.matrix_world
bpy.context.view_layer.update()
lo, hi = bounds(lid_meshes)
# at the viewer's left (+x; the front faces +y), a little behind the case
case_lo, case_hi = bounds([o for o in parts["case"][1] if o.type == 'MESH'])
lid_w = hi.x - lo.x
lid_c = Vector((case_hi.x + 0.035 + lid_w / 2, (case_lo.y + case_hi.y) / 2 - 0.07))
lid_e.matrix_world = Matrix.Translation(
    (lid_c.x - (lo.x + hi.x) / 2, lid_c.y - (lo.y + hi.y) / 2, -lo.z)) @ lid_e.matrix_world
bpy.context.view_layer.update()

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


def printed(name, color, ref, rough=0.5):
    """Printed plastic with 0.2 mm layer lines along ref's z."""
    m, nodes, links, b = new_mat(name)
    b.inputs["Base Color"].default_value = (*color, 1)
    b.inputs["Roughness"].default_value = rough
    b.inputs["Specular IOR Level"].default_value = 0.4
    tc = nodes.new("ShaderNodeTexCoord")
    tc.object = ref
    sep = nodes.new("ShaderNodeSeparateXYZ")
    links.new(tc.outputs["Object"], sep.inputs[0])
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
    return m


def engraved_panel(name, color, ref):
    """Front plate with light laser marks from engraving.png on its z=0 face."""
    m, nodes, links, b = new_mat(name)
    b.inputs["Roughness"].default_value = 0.5
    b.inputs["Specular IOR Level"].default_value = 0.4
    tc = nodes.new("ShaderNodeTexCoord")
    tc.object = ref
    mp = nodes.new("ShaderNodeMapping")
    # u = (154.9 - x) / 219.8, v = (96.9 + y) / 116.8 with x, y in mm
    mp.inputs["Location"].default_value = (154.9 / PANEL_W, 96.9 / PANEL_H, 0)
    mp.inputs["Scale"].default_value = (-1 / (PANEL_W * MM), 1 / (PANEL_H * MM), 1)
    links.new(tc.outputs["Object"], mp.inputs["Vector"])
    img = nodes.new("ShaderNodeTexImage")
    img.image = bpy.data.images.load(str(HERE / "engraving.png"))
    img.image.colorspace_settings.name = 'Non-Color'
    img.extension = 'CLIP'
    img.interpolation = 'Cubic'
    links.new(mp.outputs[0], img.inputs[0])
    sep = nodes.new("ShaderNodeSeparateXYZ")
    links.new(tc.outputs["Object"], sep.inputs[0])
    face = nodes.new("ShaderNodeMath")
    face.operation = 'LESS_THAN'
    face.inputs[1].default_value = 0.03 * MM
    links.new(sep.outputs["Z"], face.inputs[0])
    inv = nodes.new("ShaderNodeMath")       # marks are black in the PNG
    inv.operation = 'SUBTRACT'
    inv.inputs[0].default_value = 1.0
    links.new(img.outputs["Color"], inv.inputs[1])
    mask = nodes.new("ShaderNodeMath")
    mask.operation = 'MULTIPLY'
    links.new(inv.outputs[0], mask.inputs[0])
    links.new(face.outputs[0], mask.inputs[1])
    col = nodes.new("ShaderNodeMix")
    col.data_type = 'RGBA'
    col.inputs["A"].default_value = (*color, 1)
    col.inputs["B"].default_value = (0.36, 0.36, 0.35, 1)
    links.new(mask.outputs[0], col.inputs["Factor"])
    links.new(col.outputs["Result"], b.inputs["Base Color"])
    rough = nodes.new("ShaderNodeMapRange")
    rough.inputs["To Min"].default_value = 0.5
    rough.inputs["To Max"].default_value = 0.85
    links.new(mask.outputs[0], rough.inputs["Value"])
    links.new(rough.outputs["Result"], b.inputs["Roughness"])
    bump = nodes.new("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.4
    bump.inputs["Distance"].default_value = 0.03 * MM
    bump.invert = True
    links.new(mask.outputs[0], bump.inputs["Height"])
    links.new(bump.outputs["Normal"], b.inputs["Normal"])
    return m


ORANGE = (0.95, 0.22, 0.012)
DARK = (0.011, 0.012, 0.013)
plate_ref = parts["plate_Body026"][0]
M = {
    "case": printed("CaseOrange", ORANGE, root, 0.45),
    "lid": printed("LidOrange", ORANGE, lid_e, 0.45),
    "latch": printed("Latch", (0.02, 0.021, 0.023), lid_e, 0.5),
    "panel": engraved_panel("FrontPlate", DARK, plate_ref),
    "steel": principled("Stainless", (0.62, 0.61, 0.6), 0.28, 1.0,
                        **{"Anisotropic": 0.5}),
    "screw": principled("BlackOxide", (0.04, 0.04, 0.045), 0.35, 1.0),
    "rubber": principled("Rubber", (0.012, 0.012, 0.012), 0.75),
    "brass": principled("Brass", (0.85, 0.62, 0.3), 0.3, 1.0),
    "actuator": principled("Actuator", (0.02, 0.08, 0.55), 0.3, 0.7),
    "fuse": principled("FuseHolder", (0.015, 0.015, 0.016), 0.45),
    "battery": principled("BatteryABS", (0.035, 0.036, 0.04), 0.4),
    "clamp": printed("BatteryClamp", (0.02, 0.021, 0.023), root, 0.5),
}


def assign(o, mat):
    if o.data.users > 1:
        o.data = o.data.copy()
    o.data.materials.clear()
    o.data.materials.append(mat)


for o in bpy.data.objects:
    if o.type != 'MESH':
        continue
    n = o.name
    if o in lid_objs:
        assign(o, M["lid"] if n.startswith("Box") else M["latch"])
    elif n.startswith("Body001") and o in parts["case"][1]:
        assign(o, M["case"])
    elif o in parts["plate_Body026"][1]:
        assign(o, M["panel"])
    elif o in parts["screws"][1] or o in parts["screws40"][1]:
        assign(o, M["screw"])
    elif any(o in parts[k][1] for k in LEDS):
        assign(o, M["rubber"])
    elif o in parts["plate_FuseHolder"][1]:
        assign(o, M["fuse"])
    elif o in parts["plate_FuseGrommet"][1]:
        assign(o, M["rubber"])
    elif o in parts["battery"][1]:
        assign(o, M["battery"])
    elif o in parts["clamp1"][1] or o in parts["clamp2"][1]:
        assign(o, M["clamp"])
    elif o in parts["plate_Delock_22032"][1]:
        if n.startswith(("Housing", "Nut")):
            assign(o, M["steel"])
        elif n.startswith("Screws"):
            assign(o, M["screw"])
        elif n.startswith("Actuator"):
            assign(o, M["actuator"])
        elif n.startswith(("Gasket", "Insulator")):
            assign(o, M["rubber"])
        elif n.startswith("Terminals"):
            assign(o, M["brass"])
    else:
        # KiCad / vendor colours; make the grey ones read as metal
        for s in o.material_slots:
            m = s.material
            if m is None or not m.use_nodes:
                continue
            b = m.node_tree.nodes.get("Principled BSDF")
            if b is None:
                continue
            r, g, bl = b.inputs["Base Color"].default_value[:3]
            if max(r, g, bl) - min(r, g, bl) < 0.04 and r > 0.4:
                b.inputs["Metallic"].default_value = 1.0
                b.inputs["Roughness"].default_value = 0.3
    if o.data.polygons:
        o.data.shade_smooth()

# ---- exploded view ---------------------------------------------------------


def world_bounds(objs):
    vs = [m.matrix_world @ Vector(c) for o in objs
          for m in [o] + list(o.children_recursive) if m.type == 'MESH'
          for c in m.bound_box]
    return (Vector([min(v[i] for v in vs) for i in range(3)]),
            Vector([max(v[i] for v in vs) for i in range(3)]))


def outward(name, of, dist):
    """Sideways shift (mm) moving part name away from part of, along x or y."""
    d = sum(world_bounds([parts[name][0]]), Vector()) - sum(world_bounds([parts[of][0]]), Vector())
    return (math.copysign(dist, d.x), 0, 0) if abs(d.x) > abs(d.y) else (0, math.copysign(dist, d.y), 0)


bpy.context.view_layer.update()
# Bottom to top, the order the satellite goes together. Each group rises
# just far enough to clear, by EXPLODE_GAP, every group before it that it
# overlaps in plan view; "with" keeps a group at another's height and
# "shift" moves it sideways (mm).
EXPLODE_GAP = 30
EXPLODE = [
    ("case", ["case"], {}),
    ("battery", ["battery"], {}),
    # the clamps hold the battery's edges: out sideways, at its height
    ("clamp1", ["clamp1"], {"with": "battery", "shift": outward("clamp1", "battery", 45)}),
    ("clamp2", ["clamp2"], {"with": "battery", "shift": outward("clamp2", "battery", 45)}),
    ("pcb", ["plate_satellite_1"], {}),
    ("plate", ["plate_Body026"], {}),
    # fuse and LED grommets go in from the front
    ("grommet", ["plate_FuseGrommet"] + LEDS, {}),
    ("panel parts", ["plate_Part", "plate_KeySwitch", "plate_Delock_22032",
                     "plate_FuseHolder"], {}),
    ("screws", ["screws"], {}),
    # M3x40 latch pivots through the case lugs (the latches are drawn on the
    # set-aside lid): pulled out along their axis, heads at +x
    ("latch pins", ["screws40"], {"with": "case", "shift": (40, 0, 0)}),
]
EXPLODE = [(n, [parts[k][0] for k in keys], opt) for n, keys, opt in EXPLODE]


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
    for name, objs, _ in EXPLODE:
        for o in objs:
            t = Matrix.Translation(OFFSETS[name] if on else Vector())
            o.matrix_world = t @ ASSEMBLED[o]
    bpy.context.view_layer.update()


# ---- studio -----------------------------------------------------------------

# between the case and the lid beside it
CENTER = (case_lo + case_hi) / 2 + Vector((0.11, -0.03, -0.01))

bpy.ops.mesh.primitive_plane_add(size=1)
sweep = bpy.context.object
sweep.name = "Backdrop"
bm = bmesh.new()
R, L, Wd = 0.6, 1.5, 3.5
prof = [(-L, 0.0)] + [(R * math.sin(t), R - R * math.cos(t))
                      for t in [i / 16 * math.pi / 2 for i in range(17)]] + [(R, 2.0)]
rows = []
for y, z in prof:
    rows.append([bm.verts.new((x, -y - 0.3, z)) for x in (-Wd / 2, Wd / 2)])
for a, b2 in zip(rows, rows[1:]):
    bm.faces.new((a[1], a[0], b2[0], b2[1]))
bm.to_mesh(sweep.data)
bm.free()
sweep.data.shade_smooth()
SWEEP_Z = math.radians(20)
sweep.rotation_euler.z = SWEEP_Z
sweep.location.xy = CENTER.xy
assign(sweep, principled("Backdrop", (0.62, 0.62, 0.61), 0.6))

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
    lo_ = bpy.data.objects.new(name, ld)
    scene.collection.objects.link(lo_)
    lo_.location = loc
    lo_.rotation_euler = (Vector(target) - Vector(loc)).to_track_quat('-Z', 'Y').to_euler()
    return lo_


# the front of the satellite (pyro jacks at the bottom of the panel) faces +y
area_light("Key", CENTER + Vector((0.8, 0.7, 1.1)), CENTER, 40, 1.0, (1.0, 0.97, 0.93))
area_light("Fill", CENTER + Vector((-1.0, 0.8, 0.5)), CENTER, 8, 1.4, (0.92, 0.95, 1.0))
area_light("Rim", CENTER + Vector((-0.3, -1.1, 0.7)), CENTER, 25, 0.6)
area_light("Top", CENTER + Vector((0.0, 0.0, 1.5)), CENTER, 10, 0.8).visible_glossy = False

# ---- cameras ------------------------------------------------------------


def camera(name, direction, dist, lens, target=CENTER, focus=None, fstop=None, up='Z'):
    cd = bpy.data.cameras.new(name)
    cd.lens = lens
    cd.clip_start = 0.01
    co = bpy.data.objects.new(name, cd)
    scene.collection.objects.link(co)
    d = Vector(direction).normalized()
    co.location = Vector(target) + d * dist
    q = (Vector(target) - co.location).to_track_quat('-Z', up.lstrip('-'))
    if up.startswith('-'):      # image top toward -axis
        q = q @ Quaternion((0, 0, 1), math.pi)
    co.rotation_euler = q.to_euler()
    if fstop:
        cd.dof.use_dof = True
        cd.dof.aperture_fstop = fstop
        cd.dof.focus_distance = (Vector(focus or target) - co.location).length
    return co


panel_c = Vector(((case_lo.x + case_hi.x) / 2, (case_lo.y + case_hi.y) / 2, case_hi.z - 0.015))
CAMS = {
    "hero": (camera("hero", (-0.42, 0.85, 0.55), 1.15, 70, fstop=11), (2400, 1600)),
    "front": (camera("front", (0, 0.04, 1), 0.95, 85, target=panel_c, up='Y'),
              (2200, 1400)),
    "detail": (camera("detail", (-0.3, 0.75, 0.62), 0.42, 100,
                      target=panel_c + Vector((-0.05, 0.01, 0)),
                      focus=panel_c + Vector((-0.071, 0.034, 0.01)), fstop=5.6), (2400, 1600)),
}



def fitted_camera(name, direction, boxes, lens, res, margin=1.1):
    """Camera looking along -direction, pulled back until boxes fill the frame."""
    scene.render.resolution_x, scene.render.resolution_y = res
    corners = [Vector((x, y, z)) for lo, hi in boxes for x in (lo.x, hi.x)
               for y in (lo.y, hi.y) for z in (lo.z, hi.z)]
    lo = Vector([min(v[i] for v in corners) for i in range(3)])
    hi = Vector([max(v[i] for v in corners) for i in range(3)])
    co = camera(name, direction, 1.0, lens, target=(lo + hi) / 2, up='Y')
    bpy.context.view_layer.update()
    loc, _ = co.camera_fit_coords(bpy.context.evaluated_depsgraph_get(),
                                  [c for v in corners for c in v])
    c = (lo + hi) / 2
    co.location = c + (loc - c) * margin
    return co


# the stack and the lid lying beside it, from the front left
lid_lo, lid_hi = bounds(lid_meshes)
ex_boxes = list(EXPLODED_BOX.values()) + [(lid_lo, lid_hi)]
EX_DIR = Vector((-0.5, 0.82, 0.55))
EX_RES = (2000, 2400)
CAMS["exploded"] = (fitted_camera("exploded", EX_DIR, ex_boxes, 70, EX_RES), EX_RES)
EXPLODED_CAMS = {"exploded"}
# local -y is the wall: put it opposite the camera
EX_SWEEP_Z = math.atan2(EX_DIR.y, EX_DIR.x) - math.pi / 2

scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
# leave one core free for FreeCAD; saved in the .blend too
scene.render.threads_mode = 'FIXED'
scene.render.threads = max(1, os.cpu_count() - 1)
scene.cycles.samples = 128
scene.cycles.use_denoising = True
scene.cycles.denoiser = 'OPENIMAGEDENOISE'
scene.view_settings.view_transform = 'AgX'
scene.view_settings.look = 'AgX - Medium High Contrast'
scene.camera = CAMS["hero"][0]
scene.render.resolution_x, scene.render.resolution_y = CAMS["hero"][1]

bpy.ops.wm.save_as_mainfile(filepath=str(HERE / "satellite.blend"), compress=True)

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if "--render" in argv:
    names = [a for a in argv if not a.startswith("--")] or list(CAMS)
    for k in argv:
        if k.startswith("--scale="):
            scene.render.resolution_percentage = int(k.split("=")[1])
        if k.startswith("--samples="):
            scene.cycles.samples = int(k.split("=")[1])
    for n in names:
        cam, (rx, ry) = CAMS[n]
        set_exploded(n in EXPLODED_CAMS)
        scene.camera = cam
        scene.render.resolution_x, scene.render.resolution_y = rx, ry
        scene.render.filepath = str(HERE / f"render_{n}.png")
        bpy.ops.render.render(write_still=True)
