"""Export ControllerAssembly to glTF with FreeCAD's colours, for build_scene.py.

    xvfb-run -a freecad export_glb.py

Needs the GUI for the colours. The PCB link (Controller.FCStd#Part) is
silently dropped from the assembly export, so it goes to its own file and
build_scene.py places it at the link's placement.

Every linked part file is opened on its own first: a part that FreeCAD
loads only through the assembly is loaded partially and can keep stale
shapes. Nothing is saved.
"""
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
ASM = HERE.parent / "ControllerAssembly.FCStd"


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
# parts hidden while editing (BackShell, 2026-11-05) still belong in the
# renders; this is not saved
for o in doc.getObject("Assembly").Group:
    if o.TypeId in ("App::Link", "Part::FeaturePython") and not o.Visibility:
        print("export_glb: showing hidden", o.Name, o.Label)
        o.Visibility = True
ImportGui.export([doc.getObject("Assembly")], str(HERE / "assembly.glb"))
ImportGui.export([doc.getObject("Part").LinkedObject], str(HERE / "pcb.glb"))
os._exit(0)
