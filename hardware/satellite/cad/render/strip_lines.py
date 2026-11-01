"""Drop point and line primitives from a .glb in place.

FreeCAD exports KeySwitch.FCStd's edges and vertices as glTF LINES/POINTS
primitives, and Blender 5.2's importer crashes on them.
"""
import json
import struct
import sys

for path in sys.argv[1:]:
    b = open(path, "rb").read()
    jlen, = struct.unpack("<I", b[12:16])
    j = json.loads(b[20:20 + jlen])
    for m in j["meshes"]:
        m["primitives"] = [p for p in m["primitives"] if p.get("mode", 4) == 4]
    js = json.dumps(j, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    rest = b[20 + jlen:]
    out = b[:8] + struct.pack("<I", 12 + 8 + len(js) + len(rest)) + \
        struct.pack("<I4s", len(js), b"JSON") + js + rest
    open(path, "wb").write(out)
