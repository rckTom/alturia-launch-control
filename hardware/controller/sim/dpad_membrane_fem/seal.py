"""Does the retainer keep the membrane's seal squeezed?

(a) ridge.  The rejected first design, kept for comparison: a 0.4 wide, 0.15
    tall rigid ridge on the retainer pressed into the 0.4 mm TPU sheet, which
    sticks to the flat frame on top. Plane strain, force per mm of ridge.
(b) lip.    The current design: a slanted TPU lip (LIP_*) on the sheet's back,
    tip pushed LIP_SQUEEZE up by the groove floor (frictionless), sheet held by
    the frame on top and resting on the retainer outside the groove.
(c) plate.  The retainer exported from DpadMembrane.FCStd (work/retainer.step),
    linear PETG, the four screw-head seats held, the lip force F'(LIP_SQUEEZE)
    spread over the groove floor, pushing away from the frame. How far the
    floor moves away between screws is squeeze lost. The load is not relaxed
    as the plate bends, so the answer is conservative (too big).

Same env as web_force.py. Export the retainer first, from the repo root:
    freecadcmd -c "import FreeCAD as A; A.openDocument('hardware/controller/cad/DpadMembrane.FCStd').getObject('Retainer').Shape.exportStep('hardware/controller/sim/dpad_membrane_fem/work/retainer.step')"
"""
import json
import math
import os
import re
import shutil
import subprocess
import sys

import gmsh
import numpy as np

from web_force import CAD, WORK, E_REF, E_BAND, NU, cad_constants, fix_gmsh_inp

HERE = os.path.dirname(os.path.abspath(__file__))
E_PETG, NU_PETG = 1800.0, 0.38     # printed PETG, a little under the 2000 MPa bulk value
HALF_W = 2.0                       # half width of the plane-strain slice


def ccx(d, must_finish=True):
    p = subprocess.run([shutil.which("ccx") or "ccx", "-i", "job"], cwd=d,
                       capture_output=True, text=True, env=dict(os.environ, OMP_NUM_THREADS="4"))
    if "Job finished" not in p.stdout:
        if not must_finish:
            return False
        sys.stderr.write(p.stdout[-3000:])
        raise RuntimeError("ccx failed in " + d)
    return True


def write_sets(f, sets):
    for name, ids in sets.items():
        f.write("*NSET,NSET=%s\n" % name)
        for i in range(0, len(ids), 12):
            f.write(", ".join(map(str, ids[i:i + 12])) + "\n")


RIDGE_W, RIDGE_H = 0.4, 0.15      # the rejected crush ridge


def ridge(c):
    t, w, hgt = c["WEB_T"], RIDGE_W, RIDGE_H
    d = os.path.join(WORK, "ridge")
    os.makedirs(d, exist_ok=True)
    gmsh.initialize()
    gmsh.option.setNumber("General.Terminal", 0)
    occ = gmsh.model.occ
    occ.addRectangle(0, 0, 0, HALF_W, t)
    occ.synchronize()
    gmsh.model.addPhysicalGroup(2, [1], name="SHEET")
    f = gmsh.model.mesh.field
    box = f.add("Box")
    for k, v in (("VIn", 0.02), ("VOut", 0.08), ("XMin", 0), ("XMax", w / 2 + 0.3),
                 ("YMin", -1), ("YMax", 1), ("Thickness", 0.3)):
        f.setNumber(box, k, v)
    f.setAsBackgroundMesh(box)
    gmsh.option.setNumber("Mesh.MeshSizeExtendFromBoundary", 0)
    gmsh.option.setNumber("Mesh.RecombineAll", 1)
    gmsh.option.setNumber("Mesh.Algorithm", 8)
    gmsh.option.setNumber("Mesh.ElementOrder", 2)
    gmsh.option.setNumber("Mesh.SecondOrderIncomplete", 1)
    gmsh.option.setNumber("Mesh.SaveAll", 0)
    gmsh.model.mesh.generate(2)
    mesh = os.path.join(d, "mesh.inp")
    gmsh.write(mesh)
    tags, xyz, _ = gmsh.model.mesh.getNodes()
    gmsh.finalize()
    tags, xyz = tags.astype(int), xyz.reshape(-1, 3)
    # gmsh writes 2D quads as CPS8; keep them and make them plane strain
    txt = open(mesh).read()
    txt = re.sub(r"\*ELEMENT, type=CPS8[^\n]*", "*ELEMENT, type=CPE8, ELSET=SHEET", txt)
    open(mesh, "w").write(txt)
    out, keep = [], True
    for line in open(mesh):
        if line.startswith("*ELEMENT"):
            keep = "CPE8" in line
        elif line.startswith("*") and not line.startswith(("**", "*NODE")):
            keep = False
        elif line.startswith("*NODE"):
            keep = True
        if keep:
            out.append(line)
    open(mesh, "w").writelines(out)

    tol = 1e-6
    x, y = xyz[:, 0], xyz[:, 1]
    sets = {"TOP": tags[abs(y - t) < tol], "SYM": tags[abs(x) < tol],
            "PUNCH": tags[(abs(y) < tol) & (x <= w / 2 + tol)]}
    g = E_REF / (2 * (1 + NU))
    k = E_REF / (3 * (1 - 2 * NU))
    with open(os.path.join(d, "job.inp"), "w") as fh:
        fh.write("*INCLUDE,INPUT=mesh.inp\n")
        write_sets(fh, sets)
        fh.write("*MATERIAL,NAME=TPU\n*HYPERELASTIC,NEO HOOKE\n%.6g,%.6g\n" % (g / 2, 2 / k))
        fh.write("*SOLID SECTION,ELSET=SHEET,MATERIAL=TPU\n1.\n")
        fh.write("*BOUNDARY\nTOP,1,2\nSYM,1,1\nPUNCH,1,1\n")
        fh.write("*STEP,NLGEOM,INC=1000\n*STATIC\n0.05,1.,1e-6,0.05\n")
        fh.write("*BOUNDARY\nPUNCH,2,2,%.4f\n" % hgt)
        fh.write("*NODE PRINT,NSET=PUNCH,TOTALS=ONLY\nRF\n*END STEP\n")
    finished = ccx(d, must_finish=False)
    txt = open(os.path.join(d, "job.dat")).read()
    curve = [(0.0, 0.0)]
    for m in re.finditer(r"set PUNCH and time\s+([-\dE.+]+)\s*\n\s*([-\dE.+]+)\s+([-\dE.+]+)", txt):
        curve.append((float(m.group(1)) * hgt, 2 * float(m.group(3))))   # both halves, N per mm
    if not finished:
        # the sharp ridge corner distorts the mesh near full indent; extend the last slope
        (d0, f0), (d1, f1) = curve[-2], curve[-1]
        print("ridge: converged to %.3f of %.3f mm, extrapolating" % (d1, hgt))
        curve.append((hgt, f1 + (f1 - f0) / (d1 - d0) * (hgt - d1)))
    return curve


def lip(c, name="lip", **over):
    """Plane strain: slanted lip on the sheet underside squeezed onto the groove floor."""
    g_ = dict(c, **over)
    t, off, w, h = g_["WEB_T"], g_["LIP_OFF"], g_["LIP_W"], g_["LIP_H"]
    slant, sq = g_["LIP_SLANT"], g_["LIP_SQUEEZE"]
    g0, g1 = g_["GROOVE_IN"], g_["GROOVE_OUT"]
    d = os.path.join(WORK, name)
    os.makedirs(d, exist_ok=True)
    gmsh.initialize()
    gmsh.option.setNumber("General.Terminal", 0)
    occ = gmsh.model.occ
    sheet = occ.addRectangle(-1.0, 0, 0, g1 + 1.5, t)
    pts = [occ.addPoint(off - w / 2, 0, 0), occ.addPoint(off - w / 2 + slant, -h, 0),
           occ.addPoint(off + w / 2 + slant, -h, 0), occ.addPoint(off + w / 2, 0, 0)]   # CCW
    lines = [occ.addLine(pts[i], pts[(i + 1) % 4]) for i in range(4)]
    lipf = occ.addPlaneSurface([occ.addCurveLoop(lines)])
    out, _ = occ.fuse([(2, sheet)], [(2, lipf)])
    occ.synchronize()
    gmsh.model.addPhysicalGroup(2, [s for _, s in gmsh.model.getEntities(2)], name="LIP")
    gmsh.option.setNumber("Mesh.MeshSizeMax", min(0.05, w / 8))
    gmsh.option.setNumber("Mesh.RecombineAll", 1)
    gmsh.option.setNumber("Mesh.Algorithm", 8)
    gmsh.option.setNumber("Mesh.ElementOrder", 2)
    gmsh.option.setNumber("Mesh.SecondOrderIncomplete", 1)
    gmsh.option.setNumber("Mesh.SaveAll", 0)
    gmsh.model.mesh.generate(2)
    mesh = os.path.join(d, "mesh.inp")
    gmsh.write(mesh)
    tags, xyz, _ = gmsh.model.mesh.getNodes()
    gmsh.finalize()
    tags, xyz = tags.astype(int), xyz.reshape(-1, 3)
    out, keep = [], True
    for line in open(mesh):
        if line.startswith("*ELEMENT"):
            keep = "CPS8" in line
            line = "*ELEMENT, type=CPE8, ELSET=LIP\n" if keep else line
        elif line.startswith("*") and not line.startswith(("**", "*NODE")):
            keep = False
        elif line.startswith("*NODE"):
            keep = True
        if keep:
            out.append(line)
    open(mesh, "w").writelines(out)
    tol = 1e-6
    x, y = xyz[:, 0], xyz[:, 1]
    sets = {"TOP": tags[abs(y - t) < tol],
            "REST": tags[(abs(y) < tol) & ((x < g0 - tol) | (x > g1 + tol))],
            "TIP": tags[abs(y + h) < tol]}
    gm = E_REF / (2 * (1 + NU))
    k = E_REF / (3 * (1 - 2 * NU))
    with open(os.path.join(d, "job.inp"), "w") as fh:
        fh.write("*INCLUDE,INPUT=mesh.inp\n")
        write_sets(fh, sets)
        fh.write("*MATERIAL,NAME=TPU\n*HYPERELASTIC,NEO HOOKE\n%.6g,%.6g\n" % (gm / 2, 2 / k))
        fh.write("*SOLID SECTION,ELSET=LIP,MATERIAL=TPU\n1.\n")
        fh.write("*BOUNDARY\nTOP,1,2\nREST,2,2\n")
        fh.write("*STEP,NLGEOM,INC=1000\n*STATIC\n0.05,1.,1e-7,0.05\n")
        fh.write("*BOUNDARY\nTIP,2,2,%.4f\n" % sq)
        fh.write("*NODE PRINT,NSET=TIP,TOTALS=ONLY\nRF\n*NODE PRINT,NSET=TIP\nU\n*END STEP\n")
    finished = ccx(d, must_finish=False)
    txt = open(os.path.join(d, "job.dat")).read()
    curve = [(0.0, 0.0)]
    for m in re.finditer(r"set TIP and time\s+([-\dE.+]+)\s*\n\s*([-\dE.+]+)\s+([-\dE.+]+)", txt):
        curve.append((float(m.group(1)) * sq, float(m.group(3))))
    last = txt.split("displacements (vx,vy,vz) for set TIP")[-1].split("total force")[0]
    tip_x = [float(l.split()[1]) for l in last.splitlines() if len(l.split()) == 4]
    tip_x0 = off + slant
    return {"curve": curve, "finished": finished,
            "tip_x_range": [tip_x0 - w / 2 + min(tip_x), tip_x0 + w / 2 + max(tip_x)],
            "groove": [g0, g1]}


def plate(c, f_per_mm, step=None, name="plate"):
    d = os.path.join(WORK, name)
    os.makedirs(d, exist_ok=True)
    step = step or os.path.join(WORK, "retainer.step")
    z_top = c["MEM_BOT"] - (c["LIP_H"] - c["LIP_SQUEEZE"])     # groove floor
    z_seat = c["RET_BOT"] + c["HEAD_DEPTH"]
    cx, cy = c["CENTER"]
    screws = [(cx + sx * c["SCREW"], cy + sy * c["SCREW"]) for sx in (-1, 1) for sy in (-1, 1)]

    gmsh.initialize()
    gmsh.option.setNumber("General.Terminal", 0)
    gmsh.model.occ.importShapes(step)
    gmsh.model.occ.synchronize()
    vols = [v for _, v in gmsh.model.getEntities(3)]
    gmsh.model.addPhysicalGroup(3, vols, name="RET")
    ridge_faces = []
    for _, s in gmsh.model.getEntities(2):
        bb = gmsh.model.getBoundingBox(2, s)
        if abs(bb[2] - z_top) < 1e-3 and abs(bb[5] - z_top) < 1e-3:
            ridge_faces.append(s)
    gmsh.option.setNumber("Mesh.MeshSizeMax", 0.8)
    gmsh.option.setNumber("Mesh.MeshSizeMin", 0.1)
    gmsh.option.setNumber("Mesh.MeshSizeFromCurvature", 12)
    gmsh.option.setNumber("Mesh.ElementOrder", 2)
    gmsh.option.setNumber("Mesh.SaveAll", 0)
    gmsh.model.mesh.generate(3)
    mesh = os.path.join(d, "mesh.inp")
    gmsh.write(mesh)
    tags, xyz, _ = gmsh.model.mesh.getNodes()
    tags, xyz = tags.astype(int), xyz.reshape(-1, 3)
    coord = dict(zip(tags, xyz))
    # consistent loads of 6-node triangles under uniform pressure: corners 0, mid-sides A/3
    loads, area, floor = {}, 0.0, set()
    for s in ridge_faces:
        types, _, conn = gmsh.model.mesh.getElements(2, s)
        for ty, cn in zip(types, conn):
            if ty != 9:
                continue
            for tri in cn.reshape(-1, 6).astype(int):
                floor.update(tri.tolist())
                p0, p1, p2 = (coord[n] for n in tri[:3])
                a = 0.5 * np.linalg.norm(np.cross(p1 - p0, p2 - p0))
                area += a
                for n in tri[3:]:
                    loads[n] = loads.get(n, 0.0) + a / 3
    gmsh.finalize()
    fix_gmsh_inp(mesh)
    txt = open(mesh).read().replace("ELSET=WEB", "ELSET=RET")
    open(mesh, "w").write(txt)

    groove_w = c["GROOVE_OUT"] - c["GROOVE_IN"]
    ridge_len = area / groove_w
    pressure = f_per_mm / groove_w
    x, y, z = xyz[:, 0], xyz[:, 1], xyz[:, 2]
    seat = np.zeros(len(tags), bool)
    for sx, sy in screws:
        seat |= (abs(z - z_seat) < 1e-3) & (np.hypot(x - sx, y - sy) <= 1.75 + 1e-3)
    ridge_nodes = np.array(sorted(floor))       # groove floor only, not wall nodes at that height
    with open(os.path.join(d, "job.inp"), "w") as fh:
        fh.write("*INCLUDE,INPUT=mesh.inp\n")
        write_sets(fh, {"SEAT": tags[seat], "RIDGE": ridge_nodes})
        fh.write("*MATERIAL,NAME=PETG\n*ELASTIC\n%.6g,%.6g\n" % (E_PETG, NU_PETG))
        fh.write("*SOLID SECTION,ELSET=RET,MATERIAL=PETG\n")
        fh.write("*BOUNDARY\nSEAT,1,3\n*STEP\n*STATIC\n*CLOAD\n")
        for n, a in loads.items():
            fh.write("%d,3,%.6e\n" % (n, -pressure * a))
        fh.write("*NODE PRINT,NSET=RIDGE\nU\n*NODE PRINT,NSET=SEAT,TOTALS=ONLY\nRF\n")
        fh.write("*NODE FILE\nU,RF\n*EL FILE\nS\n*END STEP\n")     # job.frd, for frd2vtk.py
    ccx(d)
    txt = open(os.path.join(d, "job.dat")).read()
    uz = {}
    blk = txt.split("displacements (vx,vy,vz) for set RIDGE")[1].split("forces")[0]
    for line in blk.splitlines():
        p = line.split()
        if len(p) == 4:
            uz[int(p[0])] = float(p[3])
    pts = np.array([[coord[n][0], coord[n][1], u] for n, u in uz.items()])
    # the lip force is ~linear in squeeze and the plate is linear, so the
    # relaxed gap is d*s/(s+d) for an unrelaxed d at squeeze s
    sq = c["LIP_SQUEEZE"]
    dz = -pts[:, 2]
    relaxed = dz * sq / (sq + np.maximum(dz, 0))
    return {"relaxed_max": float(relaxed.max()), "relaxed_mean": float(relaxed.mean()),
            "squeeze_left_min": float(sq - relaxed.max()),"ridge_length_mm": ridge_len, "total_load_N": pressure * area,
            "uz_min": float(pts[:, 2].min()), "uz_mean": float(pts[:, 2].mean()),
            "worst_xy": pts[pts[:, 2].argmin(), :2].tolist(), "nodes": int(len(tags)),
            "uz_points": pts.tolist()}


def report(label, pl, c):
    print("%s: seal loop %.0f mm, load %.1f N (%.1f N per screw), groove floor moves away by "
          "max %.3f / mean %.3f mm unrelaxed, %.3f / %.3f relaxed -> squeeze left >= %.2f of %.2f mm"
          % (label, pl["ridge_length_mm"], pl["total_load_N"], pl["total_load_N"] / 4, -pl["uz_min"],
             -pl["uz_mean"], pl["relaxed_max"], pl["relaxed_mean"], pl["squeeze_left_min"], c["LIP_SQUEEZE"]))


def main():
    c = cad_constants(CAD)
    rc = ridge(c)
    print("ridge (rejected): F'(%.2f mm) = %.2f N/mm" % (RIDGE_H, rc[-1][1]))
    lp = lip(c)
    if not lp["finished"]:
        raise RuntimeError("lip sim stopped at %.3f mm" % lp["curve"][-1][0])
    f_lip = lp["curve"][-1][1]
    print("lip: F'(%.2f mm) = %.3f N/mm at E=%.0f MPa (x%.2f for E=%.0f); tip spans %.2f..%.2f, groove %.2f..%.2f"
          % (c["LIP_SQUEEZE"], f_lip, E_REF, E_BAND[0] / E_REF, E_BAND[0], *lp["tip_x_range"], *lp["groove"]))
    pl = plate(c, f_lip)
    report("plate", pl, c)
    variants = {}
    for tag, label in (("t3", "RET_T 3.0"), ("t34", "RET_T 3.4"), ("ribs", "RET_T 2.4 + 1.0 ribs")):
        step = os.path.join(WORK, "retainer_%s.step" % tag)
        if os.path.exists(step):
            variants[tag] = plate(c, f_lip, step, "plate_" + tag)
            report("plate " + label, variants[tag], c)
    json.dump({"ridge_curve": rc, "lip": lp, "plate": pl, "variants": variants, "E_PETG": E_PETG}, open(os.path.join(HERE, "seal_results.json"), "w"))


if __name__ == "__main__":
    main()
