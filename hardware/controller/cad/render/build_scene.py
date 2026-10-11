"""Build the photoreal Blender scene of the controller assembly.

    blender -b --factory-startup --python build_scene.py [-- --render [names]]

Inputs, all in this directory:
  assembly.glb   ControllerAssembly.FCStd (export_glb.py); sometimes without
                 the PCB link, depending on what FreeCAD had loaded
  pcb.glb        the PCB link target, Controller.FCStd#Part, used then
  screen.png     a 400x240 frame from controller_ui under radiosim
  engraving.png  frontcase_engraving.svg engrave layer, 40 px/mm

Writes controller.blend; with --render also renders the cameras to
render_<camera>.png. Blender units are metres (glTF), 1 mm = 0.001.
"""

import math
import os
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
# FrontCase Body placement and its flat top face (z = 13 in the body)
FRONT_AT = Vector((-6.03913, 2.96004, 2.21679 + 13.0)) * MM
FACE_W, FACE_H = 69.92, 158.92
# Display active area (LS027B7DH01, 58.8 x 35.28 mm) on the glass, z = 12.29
SCREEN_C = Vector((-6.04, 45.2, 12.30)) * MM
SCREEN_W, SCREEN_H = 58.8, 35.28
CASE_BOTTOM = -6.78 * MM

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


asm = import_glb(HERE / "assembly.glb")
for o in asm:
    if o.parent is None:
        o.parent = root

pcb_link = next((o for o in asm if o.name.startswith("controller_PCB")), None)
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
    if o.name.startswith("controller_pad"):
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
    elif n == "Body003":
        assign(o, M["back"])
    elif n == "Body002":
        assign(o, M["acrylic"])
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
    elif n.startswith(("Gasket", "Insulator")):
        assign(o, M["rubber"])
    elif n.startswith("Terminals"):
        assign(o, M["brass"])
    elif n.startswith("KeyCapBody"):
        assign(o, M["keycap"])
    elif n.startswith("KeyCapInlay"):
        assign(o, M["inlay"])
    elif n.startswith("Open CASCADE STEP translator"):   # Sharp display
        assign(o, M["lcd_frame"])
    elif n.startswith("controller_pad"):
        assign(o, M["enig"])
    elif n.startswith("controller_PCB"):
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
sweep.rotation_euler.z = math.radians(-28)
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


CENTER = Vector((-6, 3, 4)) * MM
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
                      target=Vector((-4, -52, 12)) * MM,
                      focus=Vector((12.7, -66, 15.5)) * MM, fstop=5.6), (2400, 1600)),
    "top": (camera("top", (-0.55, 0.62, 0.5), 0.36, 85,
                   target=Vector((-6, 50, 4)) * MM, fstop=8), (2400, 1600)),
}

scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
# leave one core free for FreeCAD; saved in the .blend too
scene.render.threads_mode = 'FIXED'
scene.render.threads = max(1, os.cpu_count() - 1)
scene.cycles.samples = 256
scene.cycles.use_denoising = True
scene.cycles.denoiser = 'OPENIMAGEDENOISE'
scene.view_settings.view_transform = 'AgX'
scene.view_settings.look = 'AgX - Medium High Contrast'
scene.render.film_transparent = False
scene.camera = CAMS["hero"][0]
scene.render.resolution_x, scene.render.resolution_y = CAMS["hero"][1]

bpy.ops.wm.save_as_mainfile(filepath=str(HERE / "controller.blend"), compress=True)

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
        scene.camera = cam
        scene.render.resolution_x, scene.render.resolution_y = rx, ry
        scene.render.filepath = str(HERE / f"render_{n}.png")
        bpy.ops.render.render(write_still=True)
