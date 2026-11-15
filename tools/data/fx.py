#!/usr/bin/env python3
"""Reads a compiled Direct3D 9 effect (fx_2_0 binary, magic FEFF0901) and prints its parameters, techniques, passes,
pass states and embedded shaders, to know what the runtime's ID3DXEffect has to support.

What it does: walks the layout D3DX writes (and Wine's d3dx9 effect.c reads): header, parameter definitions with
their types and default values, techniques -> passes -> states (operation, index, value), then the object table
(strings, shader bytecode). For each state it prints the operation number; shaders are shown with their profile.
What it does not do: run or translate the shaders.

usage: .venv/bin/python -I tools/data/fx.py <file.fx.ckd>
"""
import struct
import sys

CLASS = {0: "scalar", 1: "vector", 2: "matrix_rows", 3: "matrix_cols", 4: "object", 5: "struct"}
TYPE = {0: "void", 1: "bool", 2: "int", 3: "float", 4: "string", 5: "texture", 6: "texture1d", 7: "texture2d",
        8: "texture3d", 9: "texturecube", 10: "sampler", 11: "sampler1d", 12: "sampler2d", 13: "sampler3d",
        14: "samplercube", 15: "pixelshader", 16: "vertexshader"}


class Fx:
    def __init__(self, data):
        tag, off = struct.unpack_from("<II", data, 0)
        assert tag == 0xFEFF0901, "not fx_2_0"
        self.base = data[8:]
        self.p = off
        self.d = data

    def u32(self):
        v = struct.unpack_from("<I", self.base, self.p)[0]
        self.p += 4
        return v

    def string(self, off):
        n = struct.unpack_from("<I", self.base, off)[0]
        return self.base[off + 4: off + 4 + n].split(b"\0")[0].decode("latin-1")

    def typedef(self, off):
        t, cls, name, sem, elems = struct.unpack_from("<5I", self.base, off)
        info = {"type": TYPE.get(t, t), "class": CLASS.get(cls, cls), "name": self.string(name) if name else "",
                "semantic": self.string(sem) if sem else "", "elements": elems}
        if cls in (0, 1, 2, 3):
            info["cols"], info["rows"] = struct.unpack_from("<II", self.base, off + 20)
        return info

    def annotations(self, n):
        for _ in range(n):
            self.u32(); self.u32()

    def state(self):
        op, index, td, val = self.u32(), self.u32(), self.u32(), self.u32()
        t = self.typedef(td)
        v = struct.unpack_from("<I", self.base, val)[0]
        return op, index, t["type"], v


def main():
    data = open(sys.argv[1], "rb").read()
    fx = Fx(data)
    nparams, ntech, _unk, nobjects = fx.u32(), fx.u32(), fx.u32(), fx.u32()
    print("parameters %d, techniques %d, objects %d" % (nparams, ntech, nobjects))
    for _ in range(nparams):
        td, val, flags, nann = fx.u32(), fx.u32(), fx.u32(), fx.u32()
        t = fx.typedef(td)
        extra = ""
        if t["type"] in ("sampler", "sampler2d", "samplercube"):
            nst = struct.unpack_from("<I", fx.base, val)[0]
            sts = []
            for k in range(nst):
                op, idx, std, sval = struct.unpack_from("<4I", fx.base, val + 4 + 16 * k)
                sts.append((op, idx, struct.unpack_from("<I", fx.base, sval)[0]))
            extra = " sampler states %s" % sts
        print("  param %-28s %-12s %-12s %s x%d flags %x%s" % (t["name"], t["type"], t["class"],
              "%dx%d" % (t.get("rows", 0), t.get("cols", 0)), t["elements"], flags, extra))
        fx.annotations(nann)
    for _ in range(ntech):
        name, nann, npass = fx.u32(), fx.u32(), fx.u32()
        print("  technique %s (%d passes)" % (fx.string(name), npass))
        fx.annotations(nann)
        for _ in range(npass):
            pname, pann, nst = fx.u32(), fx.u32(), fx.u32()
            fx.annotations(pann)
            states = [fx.state() for _ in range(nst)]
            print("    pass %s: %s" % (fx.string(pname), states))
    nstrings, nres = fx.u32(), fx.u32()
    print("strings %d, resources %d" % (nstrings, nres))
    for _ in range(nstrings):
        oid, size = fx.u32(), fx.u32()
        fx.p += (size + 3) & ~3
    for _ in range(nres):
        tech, idx, elem, st, usage, size = (fx.u32() for _ in range(6))
        blob = fx.base[fx.p: fx.p + size]
        fx.p += (size + 3) & ~3
        ver = struct.unpack_from("<I", blob, 0)[0] if size >= 4 else 0
        print("  resource tech %d pass %d elem %d state %d usage %d: %d bytes, version %08x" %
              (tech, idx, elem, st, usage, size, ver))


if __name__ == "__main__":
    main()
