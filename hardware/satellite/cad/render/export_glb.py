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
"""
import json
import os
from pathlib import Path

import FreeCAD as App
import ImportGui

try:
    HERE = Path(__file__).resolve().parent
except NameError:   # some FreeCAD versions run scripts without __file__
    HERE = Path.cwd()
doc = App.openDocument(str(HERE.parent / "Case" / "SatelliteAssembly.FCStd"))
doc.recompute()
g = doc.getObject
a1, a2 = g("Assembly001"), g("Assembly002")
plate = a1.LinkedObject.Document

parts = {
    "case": ([g("Body001")], g("Body001").Placement),
    "lid": ([a2], a2.Placement),            # Box + latches, as assembled
    "screws": ([o for o in doc.Objects if o.Name.startswith("Screw")], App.Placement()),
    "engraving_plane": (None, g("frontpanel_engraving").Placement),
}
for name in ("Body026", "satellite_1", "Part", "KeySwitch", "Delock_22032", "FuseHolder"):
    o = plate.getObject(name)
    parts["plate_" + name] = ([o], a1.Placement.multiply(o.Placement))

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
