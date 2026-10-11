"""Convert a CalculiX job.frd to a ParaView .vtu (mesh + nodal results).

    python frd2vtk.py work/plate_ribs/job.frd        # writes work/plate_ribs/job.vtu
    paraview --script=view_plate.py                  # or just open the .vtu

Needs only numpy. Handles the solid elements these jobs use (C3D4, C3D10,
C3D8) and every nodal result block of the last step: DISP becomes the vector
U, STRESS the symmetric tensor S plus von Mises S_mises, FORC the vector RF,
anything else (ERROR, ...) a scalar or multi-component array by its name.
"""
import base64
import os
import sys

import numpy as np

# frd element type -> (VTK cell type, nodes). CalculiX and VTK agree on the
# node order of all three.
CELL = {1: (12, 8), 3: (10, 4), 6: (24, 10)}
RENAME = {"DISP": "U", "STRESS": "S", "FORC": "RF"}


def floats(line, n):
    return [float(line[13 + 12 * i:25 + 12 * i]) for i in range(n)]


def read_frd(path):
    nodes, cells, results = {}, [], {}
    with open(path) as f:
        lines = iter(f)
        for line in lines:
            key = line[:6].strip()
            if key == "2C":
                for line in lines:
                    if line.startswith(" -3"):
                        break
                    nodes[int(line[3:13])] = floats(line, 3)
            elif key == "3C":
                cur = None
                for line in lines:
                    if line.startswith(" -3"):
                        break
                    if line.startswith(" -1"):
                        typ = int(line[13:18])
                        if typ not in CELL:
                            raise SystemExit("frd element type %d not supported" % typ)
                        cur = [typ, []]
                        cells.append(cur)
                    else:
                        cur[1] += [int(line[3 + 10 * i:13 + 10 * i]) for i in range((len(line.rstrip()) - 3) // 10)]
            elif line.startswith(" -4"):
                name, comps = line[5:13].strip(), []
                for line in lines:
                    if not line.startswith(" -5"):
                        break
                    if line[5:13].strip() != "ALL":     # magnitude, listed but not stored
                        comps.append(line[5:13].strip())
                vals = {}
                while not line.startswith(" -3"):
                    vals[int(line[3:13])] = floats(line, len(comps))
                    line = next(lines)
                results[name] = (comps, vals)     # a later step overwrites an earlier one
    return nodes, cells, results


DTYPE = {"Float32": np.float32, "Float64": np.float64, "Int32": np.int32, "Int64": np.int64, "UInt8": np.uint8}


def b64(a):
    # VTK wants the byte-count header and the data base64-encoded separately
    raw = np.ascontiguousarray(a).tobytes()
    return (base64.b64encode(np.uint64(len(raw)).tobytes()) + base64.b64encode(raw)).decode()


def array(name, a, typ):
    nc = 1 if a.ndim == 1 else a.shape[1]
    return ('<DataArray type="%s" Name="%s" NumberOfComponents="%d" format="binary">%s</DataArray>\n'
            % (typ, name, nc, b64(a.astype(DTYPE[typ]))))


def write_vtu(path, nodes, cells, results):
    ids = np.array(sorted(nodes))
    index = np.full(ids.max() + 1, -1, np.int64)
    index[ids] = np.arange(len(ids))
    pts = np.array([nodes[i] for i in ids])
    conn = np.concatenate([index[np.array(c[1][:CELL[c[0]][1]])] for c in cells])
    offs = np.cumsum([CELL[c[0]][1] for c in cells])
    types = np.array([CELL[c[0]][0] for c in cells], np.uint8)

    pdata = []
    for name, (comps, vals) in results.items():
        a = np.zeros((len(ids), len(comps)))
        for n, v in vals.items():
            if n < len(index) and index[n] >= 0:
                a[index[n]] = v
        out = RENAME.get(name, name)
        if name == "STRESS":
            # frd: xx yy zz xy yz zx; VTK symmetric tensor: xx yy zz xy yz xz
            xx, yy, zz, xy, yz, zx = a.T
            pdata.append(array(out, a, "Float32"))
            mises = np.sqrt(0.5 * ((xx - yy) ** 2 + (yy - zz) ** 2 + (zz - xx) ** 2)
                            + 3 * (xy ** 2 + yz ** 2 + zx ** 2))
            pdata.append(array("S_mises", mises, "Float32"))
        else:
            pdata.append(array(out, a[:, 0] if a.shape[1] == 1 else a, "Float32"))

    with open(path, "w") as f:
        f.write('<?xml version="1.0"?>\n<VTKFile type="UnstructuredGrid" version="1.0" '
                'byte_order="LittleEndian" header_type="UInt64">\n<UnstructuredGrid>\n'
                '<Piece NumberOfPoints="%d" NumberOfCells="%d">\n' % (len(ids), len(cells)))
        f.write("<PointData>\n%s</PointData>\n" % "".join(pdata))
        f.write("<Points>\n%s</Points>\n" % array("Points", pts, "Float64"))
        f.write("<CellData>\n%s</CellData>\n" % array("frd_id", np.arange(1, len(cells) + 1, dtype=np.int32), "Int32"))
        f.write("<Cells>\n%s%s%s</Cells>\n" % (array("connectivity", conn, "Int64"),
                                                array("offsets", offs, "Int64"),
                                                array("types", types, "UInt8")))
        f.write("</Piece>\n</UnstructuredGrid>\n</VTKFile>\n")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    for frd in sys.argv[1:]:
        nodes, cells, results = read_frd(frd)
        out = os.path.splitext(frd)[0] + ".vtu"
        write_vtu(out, nodes, cells, results)
        print("%s: %d nodes, %d cells, %s" % (out, len(nodes), len(cells), ", ".join(results) or "mesh only"))


if __name__ == "__main__":
    main()
