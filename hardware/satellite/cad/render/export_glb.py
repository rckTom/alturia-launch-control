"""Export SatelliteAssembly to glTF with FreeCAD's colours, for build_scene.py.

    xvfb-run -a freecad export_glb.py

Needs the GUI for the colours. A link exported on its own loses its
placement, so every part goes to its own <name>.glb in its own frame and
placements.json holds its global placement (a 4x4 matrix in mm). The whole
front plate assembly in one file crashes Blender's glTF importer, so its
parts are exported one by one.

SatelliteAssembly is modelled upside down: the deep ParametricCase shell is
the case, opening down, and LidAssembly's shallow Box with the latches is
the lid underneath.

Every linked part file is opened on its own first: a part that FreeCAD
loads only through the assembly is loaded partially and can keep stale
shapes (ParametricCase did). Nothing is saved.
"""
import json
import os
import re
import zipfile
from pathlib import Path

import FreeCAD as App
import ImportGui

try:
    HERE = Path(__file__).resolve().parent
except NameError:   # some FreeCAD versions run scripts without __file__
    HERE = Path.cwd()
ASM = HERE.parent / "SatelliteAssembly.FCStd"


def linked_files(path, seen):
    """FCStd files linked from path, leaves first."""
    path = path.resolve()
    if path in seen:
        return
    seen[path] = None
    xml = zipfile.ZipFile(path).read("Document.xml").decode()
    for f in sorted(set(re.findall(r'file="([^"]+\.FCStd)"', xml))):
        yield from linked_files(path.parent / f, seen)
    yield path


for f in linked_files(ASM, {}):
    if f != ASM.resolve():
        App.openDocument(str(f)).recompute()
doc = App.openDocument(str(ASM))
doc.recompute()
g = doc.getObject
a1, a2 = g("Assembly001"), g("Assembly002")
plate = a1.LinkedObject.Document

parts = {
    "case": ([g("Body001")], g("Body001").Placement),
    "lid": ([a2], a2.Placement),            # Box + latches, as assembled
    # M3x10 hold the plate; the M3x40 (added 2026-11-05) lie along x
    "screws": ([o for o in doc.Objects if o.Name.startswith("Screw") and o.Visibility
                and not o.Label.startswith("M3x40")], App.Placement()),
    "screws40": ([o for o in doc.Objects if o.Name.startswith("Screw") and o.Visibility
                  and o.Label.startswith("M3x40")], App.Placement()),
    "battery": ([g("Body028")], g("Body028").Placement),
    "clamp1": ([g("Body029")], g("Body029").Placement),
    "clamp2": ([g("Body030")], g("Body030").Placement),
    "engraving_plane": (None, g("frontpanel_engraving").Placement),
}
for name in ("Body026", "satellite_1", "Part", "KeySwitch", "Delock_22032", "FuseHolder",
             "FuseGrommet"):
    o = plate.getObject(name)
    parts["plate_" + name] = ([o], a1.Placement.multiply(o.Placement))
# LED grommets (led_grommet links, 2026-11-05): one file, one placement each
leds = [o for o in plate.Objects if o.TypeId == "App::Link"
        and o.LinkedObject.Document.Name == "led_grommet"]
ImportGui.export([leds[0].LinkedObject], str(HERE / "led_grommet.glb"))
for o in leds:
    parts["led_" + o.Name] = (None, a1.Placement.multiply(o.Placement))

out = {}
for name, (objs, pl) in parts.items():
    if objs:
        ImportGui.export(objs, str(HERE / f"{name}.glb"))
    m = pl.toMatrix()
    out[name] = [[m.A[r * 4 + c] for c in range(4)] for r in range(4)]
ip = g("frontpanel_engraving")
out["engraving_size"] = [float(ip.XSize), float(ip.YSize)]
(HERE / "placements.json").write_text(json.dumps(out, indent=1))
os._exit(0)
