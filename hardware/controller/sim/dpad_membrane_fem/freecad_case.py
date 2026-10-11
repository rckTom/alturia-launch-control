"""seal.py's retainer plate case as a FreeCAD FEM analysis, to look at in the GUI.

    freecadcmd freecad_case.py                       # CASE=plate for the plain plate
    freecad work/plate_ribs/fc/plate_ribs.FCStd

Same as seal.plate(): the retainer STEP, PETG, the four screw-head seats fixed,
the lip force F'(LIP_SQUEEZE) as a pressure on the groove floor. The mesh is
seal.py's own work/<case>/mesh.inp (no gmsh/netgen for FreeCAD here), so the
answer should match work/<case>/job.dat. FreeCAD writes its own job, runs ccx
and loads the result into a pipeline; the boundary conditions show as symbols
on the solid.
"""
import json
import shutil
import os
import sys
import types

import FreeCAD
import ObjectsFem
import Part
from feminout import importInpMesh
from femtools import ccxtools

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.modules.setdefault("gmsh", types.ModuleType("gmsh"))    # web_force imports it, cad_constants doesn't need it
from web_force import CAD, WORK, cad_constants              # noqa: E402

E_PETG, NU_PETG = 1800.0, 0.38                              # as seal.py
HEAD_R = 1.75                                               # ISO 7380 M2 head, as seal.py
STEPS = {"plate": "retainer.step", "plate_t3": "retainer_t3.step",
         "plate_t34": "retainer_t34.step", "plate_ribs": "retainer_ribs.step"}


def flat_faces(shape, z):
    return ["Face%d" % (i + 1) for i, f in enumerate(shape.Faces)
            if abs(f.BoundBox.ZMin - z) < 1e-3 and abs(f.BoundBox.ZMax - z) < 1e-3]


def main():
    case = os.environ.get("CASE", "plate_ribs")
    c = cad_constants(CAD)
    z_top = c["MEM_BOT"] - (c["LIP_H"] - c["LIP_SQUEEZE"])          # groove floor
    z_seat = c["RET_BOT"] + c["HEAD_DEPTH"]
    cx, cy = c["CENTER"]
    screws = [FreeCAD.Vector(cx + sx * c["SCREW"], cy + sy * c["SCREW"], z_seat) for sx in (-1, 1) for sy in (-1, 1)]
    f_lip = json.load(open(os.path.join(HERE, "seal_results.json")))["lip"]["curve"][-1][1]
    pressure = f_lip / (c["GROOVE_OUT"] - c["GROOVE_IN"])

    out = os.path.join(WORK, case, "fc")
    os.makedirs(out, exist_ok=True)
    doc = FreeCAD.newDocument(case)

    # the head only bears on the inner part of the counterbore floor; imprint
    # its circle so the fixed faces hold the same nodes seal.py does
    shape = Part.read(os.path.join(WORK, STEPS[case]))
    heads = [Part.Face(Part.Wire(Part.makeCircle(HEAD_R, p))) for p in screws]
    shape = shape.generalFuse(heads)[0].Solids[0]
    ret = doc.addObject("Part::Feature", "Retainer")
    ret.Shape = shape

    analysis = ObjectsFem.makeAnalysis(doc, "Analysis")
    solver = analysis.addObject(ObjectsFem.makeSolverCalculiXCcxTools(doc))[0]
    solver.AnalysisType = "static"

    mat = analysis.addObject(ObjectsFem.makeMaterialSolid(doc, "PETG"))[0]
    m = mat.Material
    m.update({"Name": "PETG (printed)", "YoungsModulus": "%g MPa" % E_PETG,
              "PoissonRatio": "%g" % NU_PETG, "Density": "1270 kg/m^3"})
    mat.Material = m

    mesh = doc.addObject("Fem::FemMeshObject", "Mesh")
    mesh.FemMesh = importInpMesh.read(os.path.join(WORK, case, "mesh.inp"))
    analysis.addObject(mesh)

    seats = [n for n in flat_faces(shape, z_seat)
             if min(shape.getElement(n).CenterOfMass.distanceToPoint(p) for p in screws) < 1.0
             and shape.getElement(n).BoundBox.XLength <= 2 * HEAD_R + 1e-3]
    fixed = analysis.addObject(ObjectsFem.makeConstraintFixed(doc, "ScrewSeats"))[0]
    fixed.References = [(ret, seats)]

    floor = flat_faces(shape, z_top)
    load = analysis.addObject(ObjectsFem.makeConstraintPressure(doc, "LipOnGrooveFloor"))[0]
    load.References = [(ret, floor)]
    load.Pressure = "%.6g MPa" % pressure                   # into the face = -z, away from the frame
    load.Reversed = False
    area = sum(shape.getElement(n).Area for n in floor)
    print("%s: %d seat faces, %d floor faces, %.1f mm2 at %.4f MPa = %.2f N"
          % (case, len(seats), len(floor), area, pressure, area * pressure))
    doc.recompute()

    fea = ccxtools.FemToolsCcx(analysis, solver)
    fea.update_objects()
    fea.setup_working_dir(out, create=True)
    msg = fea.check_prerequisites()
    if msg:
        raise SystemExit(msg)
    fea.write_inp_file()
    fea.setup_ccx(shutil.which("ccx"))
    fea.setup_ccx = lambda *a, **k: None    # ccx_run() would redo it from the (stale) FEM preference
    # FreeCAD runs ccx on every core; SPOOLES at 16 threads once gave 14 garbage
    # nodes here, at 4 it matched seal.py. ccx takes this over OMP_NUM_THREADS.
    os.environ["CCX_NPROC_EQUATION_SOLVER"] = "4"
    if fea.ccx_run() != 0:
        raise SystemExit("ccx failed, see " + out)
    fea.load_results()
    doc.recompute()

    res = [o for o in doc.Objects if o.isDerivedFrom("Fem::FemResultObjectPython") or o.TypeId == "Fem::FemResultObjectPython"]
    if res:
        dz = [v.z for v in res[0].DisplacementVectors]
        print("U_z min %.4f max %.4f mm" % (min(dz), max(dz)))
    print("objects:", ", ".join("%s (%s)" % (o.Name, o.TypeId) for o in doc.Objects))
    path = os.path.join(out, case + ".FCStd")
    doc.saveAs(path)
    print("saved", path)


main()
