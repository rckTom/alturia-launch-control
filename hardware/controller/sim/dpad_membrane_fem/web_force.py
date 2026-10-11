"""Force-stroke of one D-pad key's TPU web, CalculiX nonlinear static.

Geometry comes from ../../cad/dpad_membrane.py (its top-level constants), so
the sim follows the CAD. Quarter model of one key, centred on the hub:
  - sheet WEB_T thick, out to the well edge plus half a rib (the clamp zone)
  - hub + nub underneath, free (the switch force adds in parallel)
  - the Ø STEM_D stem is a rigid, sticking disc on the sheet top that is
    pushed down to STROKE
  - clamp zone: "clamped" = top and bottom face nodes fixed in x, y, z;
    "sliding" = z only, i.e. a frictionless sandwich. Reality sits between.
Material: compressible neo-Hooke from E and nu. With a pure displacement load
every force scales exactly with E, so one run per case is rescaled for the
E band in E_BAND.

Needs gmsh (python) and ccx on PATH, e.g. a conda-forge env:
    micromamba create -p ./fem -c conda-forge calculix gmsh python-gmsh
    ./fem/bin/python web_force.py            # writes results.json + work/
"""
import ast
import json
import math
import os
import re
import shutil
import subprocess
import sys

import gmsh
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
CAD = os.path.normpath(os.path.join(HERE, "../../cad/dpad_membrane.py"))
WORK = os.path.join(HERE, "work")

E_REF = 26.0        # MPa, NinjaTek Cheetah 95A datasheet tensile modulus
E_BAND = (10.0, 26.0)   # printed 95A TPUs quote ~9-26 MPa (PolyFlex 95 .. Cheetah)
NU = 0.45
STROKE = 0.6        # mm of stem travel simulated
INC = 0.05          # mm per increment
THICKNESSES = (0.3, 0.4, 0.5)
H_DIV = 2           # element size = t / H_DIV; t/3 changed F(0.35) by <2 % (see README)


def cad_constants(path):
    """Evaluate the plain numeric top-level assignments of the CAD script."""
    env = {}
    for node in ast.parse(open(path).read()).body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 \
                and isinstance(node.targets[0], ast.Name):
            try:
                v = eval(compile(ast.Expression(node.value), path, "eval"), {"__builtins__": {}}, env)
            except Exception:
                continue
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                env[node.targets[0].id] = float(v)
            elif isinstance(v, tuple) and all(isinstance(e, (int, float)) for e in v):
                env[node.targets[0].id] = tuple(map(float, v))
    return env


def rounded_square_sd(x, y, half, r):
    qx, qy = abs(x) - (half - r), abs(y) - (half - r)
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


def build_mesh(c, shape, t, path, h_div):
    well = c["WELL"]
    ext = 0.5 * (c["PITCH"] - 2 * well)          # half the rib between wells
    nub_len = (c["MEM_TOP"] - t - c["HUB_T"]) - (c["SW_TOP"] + c["PRELOAD"])
    gmsh.initialize()
    gmsh.option.setNumber("General.Terminal", 0)
    gmsh.model.add("web")
    occ = gmsh.model.occ
    if shape == "sq":
        half, r = well + ext, c["WELL_R"] + ext
        face = occ.addRectangle(-half, -half, 0, 2 * half, 2 * half, roundedRadius=r)
        sheet = [d for d in occ.extrude([(2, face)], 0, 0, t) if d[0] == 3]
    else:
        sheet = [(3, occ.addCylinder(0, 0, 0, 0, 0, t, well + ext))]
    hub = occ.addCylinder(0, 0, -c["HUB_T"], 0, 0, c["HUB_T"], c["HUB_R"])
    nub = occ.addCylinder(0, 0, -c["HUB_T"] - nub_len, 0, 0, nub_len, c["NUB_D"] / 2)
    body, _ = occ.fuse(sheet, [(3, hub), (3, nub)])
    quarter = occ.addBox(0, 0, -10, 20, 20, 20)
    body, _ = occ.intersect(body, [(3, quarter)])
    occ.synchronize()
    vols = [v for _, v in gmsh.model.getEntities(3)]
    gmsh.model.addPhysicalGroup(3, vols, name="WEB")
    h = t / h_div
    gmsh.option.setNumber("Mesh.MeshSizeMax", h)
    gmsh.option.setNumber("Mesh.MeshSizeMin", h / 3)
    gmsh.option.setNumber("Mesh.ElementOrder", 2)
    gmsh.option.setNumber("Mesh.SecondOrderLinear", 0)
    gmsh.option.setNumber("Mesh.SaveAll", 0)
    gmsh.model.mesh.generate(3)
    gmsh.write(path)
    tags, xyz, _ = gmsh.model.mesh.getNodes()
    gmsh.finalize()
    return tags.astype(int), xyz.reshape(-1, 3), ext


def node_sets(c, shape, t, tags, xyz):
    tol = 1e-4
    x, y, z = xyz[:, 0], xyz[:, 1], xyz[:, 2]
    r = np.hypot(x, y)
    well = c["WELL"]
    if shape == "sq":
        outside = np.array([rounded_square_sd(a, b, well, c["WELL_R"]) >= -tol for a, b in zip(x, y)])
    else:
        outside = r >= well - tol
    face = (abs(z - t) < tol) | (abs(z) < tol)
    return {
        "STEM": tags[(abs(z - t) < tol) & (r <= c["STEM_D"] / 2 + tol)],
        "CLAMP": tags[face & outside],
        "SYMX": tags[abs(x) < tol],
        "SYMY": tags[abs(y) < tol],
        "TOPC": tags[(abs(z - t) < tol) & (abs(x) < tol) & (abs(y) < tol)],
    }


def write_inp(path, mesh, sets, clamp):
    g = E_REF / (2 * (1 + NU))
    k = E_REF / (3 * (1 - 2 * NU))
    with open(path, "w") as f:
        f.write("*INCLUDE,INPUT=%s\n" % os.path.basename(mesh))
        for name, ids in sets.items():
            f.write("*NSET,NSET=%s\n" % name)
            for i in range(0, len(ids), 12):
                f.write(", ".join(map(str, ids[i:i + 12])) + "\n")
        f.write("*MATERIAL,NAME=TPU\n*HYPERELASTIC,NEO HOOKE\n%.6g,%.6g\n" % (g / 2, 2 / k))
        f.write("*SOLID SECTION,ELSET=WEB,MATERIAL=TPU\n")
        f.write("*BOUNDARY\nSYMX,1,1\nSYMY,2,2\nSTEM,1,2\n")
        f.write("CLAMP,1,3\n" if clamp == "clamped" else "CLAMP,3,3\n")
        f.write("*STEP,NLGEOM,INC=500\n*STATIC\n%.6g,1.,1e-6,%.6g\n" % (INC / STROKE, INC / STROKE))
        f.write("*BOUNDARY\nSTEM,3,3,%.4f\n" % -STROKE)
        f.write("*NODE PRINT,NSET=STEM,TOTALS=ONLY\nRF\n")
        f.write("*NODE PRINT,NSET=TOPC\nU\n")
        # every increment: FREQUENCY / TIME POINTS on the file output also thin out NODE PRINT
        f.write("*EL FILE\nE\n*END STEP\n")


def fix_gmsh_inp(path):
    """Keep only the volume elements and name their set WEB."""
    out, keep = [], True
    for line in open(path):
        if line.startswith("*ELEMENT"):
            keep = "C3D10" in line
            if keep:
                line = "*ELEMENT, type=C3D10, ELSET=WEB\n"
        elif line.startswith("*ELSET") or line.startswith("*NSET"):
            keep = False
        elif line.startswith("*") and not line.startswith("**") and not line.startswith("*NODE"):
            keep = False
        elif line.startswith("*NODE"):
            keep = True
        if keep:
            out.append(line)
    open(path, "w").writelines(out)


def parse_dat(path):
    txt = open(path).read()
    curve = [(0.0, 0.0)]
    for m in re.finditer(r"total force \(fx,fy,fz\) for set STEM and time\s+([-\dE.+]+)\s*\n\s*([-\dE.+]+)\s+([-\dE.+]+)\s+([-\dE.+]+)", txt):
        tm, fz = float(m.group(1)), float(m.group(4))
        curve.append((tm * STROKE, -4 * fz))          # quarter model
    return curve


def parse_frd_strain(path):
    """Max principal Lagrange strain over all nodes of the last E block."""
    lines = open(path).read().splitlines()
    starts = [i for i, l in enumerate(lines) if l.strip().startswith("-4") and "STRAIN" in l]
    if not starts:
        return None
    i, best = starts[-1] + 1, 0.0
    while i < len(lines) and not lines[i].strip().startswith("-3"):
        l = lines[i]
        if l.startswith(" -1"):
            v = [float(l[13 + 12 * k:25 + 12 * k]) for k in range(6)]
            exx, eyy, ezz, exy, eyz, ezx = v
            m = np.array([[exx, exy, ezx], [exy, eyy, eyz], [ezx, eyz, ezz]])
            best = max(best, float(np.linalg.eigvalsh(m)[-1]))
        i += 1
    return best


def run_case(c, shape, t, clamp, h_div=H_DIV):
    name = "%s_t%.2f_%s_h%d" % (shape, t, clamp, h_div)
    d = os.path.join(WORK, name)
    os.makedirs(d, exist_ok=True)
    mesh = os.path.join(d, "mesh.inp")
    tags, xyz, _ = build_mesh(c, shape, t, mesh, h_div)
    fix_gmsh_inp(mesh)
    sets = node_sets(c, shape, t, tags, xyz)
    write_inp(os.path.join(d, "job.inp"), mesh, sets, clamp)
    env = dict(os.environ, OMP_NUM_THREADS="1", CCX_NPROC_EQUATION_SOLVER="1")
    p = subprocess.run([shutil.which("ccx") or "ccx", "-i", "job"], cwd=d, env=env,
                       capture_output=True, text=True)
    if "Job finished" not in p.stdout:
        sys.stderr.write(p.stdout[-2000:])
        raise RuntimeError("ccx failed for " + name)
    curve = parse_dat(os.path.join(d, "job.dat"))
    if len(curve) < int(round(STROKE / INC)):
        raise RuntimeError("%s: only %d force points" % (name, len(curve)))
    strain = parse_frd_strain(os.path.join(d, "job.frd"))
    os.remove(os.path.join(d, "job.frd"))
    return {"shape": shape, "t": t, "clamp": clamp, "h_div": h_div, "nodes": int(len(tags)),
            "curve": curve, "max_strain_at_stroke": strain}


def roark_round(c, t):
    """Linear plate theory, same as the hand estimate: N/mm."""
    nu, a, b = 0.45, c["WELL"], c["HUB_R"]
    D = E_REF * t ** 3 / (12 * (1 - nu ** 2))
    ec = 1 / (8 * math.pi * D)
    m = np.array([[2 * a, 1 / a], [2 * b, 1 / b]])
    B, C = np.linalg.solve(m, -ec * np.array([2 * a * math.log(a) + a, 2 * b * math.log(b) + b]))
    w = lambda r: B * r * r + C * math.log(r) + ec * r * r * math.log(r)
    return 1 / abs(w(b) - w(a))


def _run(args):
    return run_case(*args)


def main():
    from multiprocessing import Pool
    c = cad_constants(CAD)
    os.makedirs(WORK, exist_ok=True)
    jobs = [(c, shape, t, clamp) for t in THICKNESSES for shape in ("sq", "rd")
            for clamp in ("clamped", "sliding")]
    jobs.append((c, "rd", 0.4, "clamped", 3))      # mesh convergence check
    with Pool(min(len(jobs), os.cpu_count() or 4)) as pool:
        results = pool.map(_run, jobs)
    for r in results:
        cv = list(zip(*r["curve"]))
        print("%-3s t=%.2f %-8s h=t/%d nodes=%6d  F(0.35)=%.3f N  F(0.45)=%.3f N  eps_max(%.2f)=%.3f"
              % (r["shape"], r["t"], r["clamp"], r["h_div"], r["nodes"], np.interp(0.35, *cv),
                 np.interp(0.45, *cv), STROKE, r["max_strain_at_stroke"] or -1))
    for t in THICKNESSES:
        print("round t=%.2f, linear plate theory: %.3f N at 0.35" % (t, roark_round(c, t) * 0.35))
    meta = {k: c[k] for k in ("WELL", "WELL_R", "HUB_R", "STEM_D", "PRELOAD", "PITCH", "WEB_T")}
    meta.update(E_REF=E_REF, E_BAND=E_BAND, NU=NU, STROKE=STROKE)
    json.dump({"meta": meta, "results": results}, open(os.path.join(HERE, "results.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
