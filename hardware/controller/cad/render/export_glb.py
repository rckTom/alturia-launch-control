"""Export ControllerAssembly to glTF with FreeCAD's colours, for build_scene.py.

    xvfb-run -a freecad export_glb.py

Needs the GUI for the colours. The PCB link (Controller.FCStd#Part) is
silently dropped from the assembly export, so it goes to its own file and
build_scene.py places it at the link's placement.
"""
import os
from pathlib import Path

import FreeCAD as App
import ImportGui

try:
    HERE = Path(__file__).resolve().parent
except NameError:   # some FreeCAD versions run scripts without __file__
    HERE = Path.cwd()
doc = App.openDocument(str(HERE.parent / "ControllerAssembly.FCStd"))
doc.recompute()
ImportGui.export([doc.getObject("Assembly")], str(HERE / "assembly.glb"))
ImportGui.export([doc.getObject("Part").LinkedObject], str(HERE / "pcb.glb"))
os._exit(0)
