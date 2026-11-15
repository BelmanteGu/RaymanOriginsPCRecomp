#!/usr/bin/env python3
"""First read of the Steam install: PE facts of Rayman Origins.exe and the DLLs next to it.

What it does: prints sections, image base, relocations, entry point, debug (PDB) path, copy-protection wrapper section
names, every imported DLL with its functions, exports, and the third-party libraries recognised by their copyright
strings. It only reads the install; it writes nothing into it.
What it does not do: disassemble code (see x86_census.py) or read the game's data archives.

usage: .venv/bin/python -I tools/pe_info.py --game <Steam folder> [--exe "Rayman Origins.exe"] [--all-imports]
"""
import argparse
import os
import re
import sys

import pefile

WRAPPER_SECTIONS = (".bind", ".arch", ".themida", ".vmp0", ".vmp1", ".enigma1", ".winlice", ".securom", ".tages")
LIB_PATTERNS = [
    (rb"Lua 5\.[0-9.]+", "Lua"),
    (rb"inflate [0-9.]+ Copyright", "zlib"),
    (rb"deflate [0-9.]+ Copyright", "zlib (deflate)"),
    (rb"Thomas G\. Lane", "libjpeg (IJG)"),
    (rb"libpng version [0-9.]+", "libpng"),
    (rb"P\.J\. Plauger, licensed by Dinkumware", "MSVC C++ library (Dinkumware)"),
    (rb"Scaleform", "Scaleform GFx"),
    (rb"CRI Middleware|CRI ADX|CRIWARE", "CRI Middleware"),
    (rb"Bink|RAD Game Tools", "RAD (Bink)"),
    (rb"Havok", "Havok"),
    (rb"OpenSSL [0-9]", "OpenSSL"),
    (rb"Quazal", "Quazal Rendez-Vous (online)"),
]


def section_rows(pe):
    rows = []
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode(errors="replace")
        flags = ("R" if s.Characteristics & 0x40000000 else "-") + ("W" if s.Characteristics & 0x80000000 else "-") + \
                ("X" if s.Characteristics & 0x20000000 else "-")
        rows.append((name, pe.OPTIONAL_HEADER.ImageBase + s.VirtualAddress, s.Misc_VirtualSize, s.SizeOfRawData, flags))
    return rows


def pdb_path(pe):
    for d in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []):
        e = d.entry
        if e is not None and hasattr(e, "PdbFileName"):
            return e.PdbFileName.rstrip(b"\0").decode(errors="replace"), getattr(e, "Age", None)
    return None, None


def imports(pe):
    out = []
    for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        names = [(i.name.decode() if i.name else "#%d" % i.ordinal) for i in d.imports]
        out.append((d.dll.decode(), names, False))
    for d in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []):
        names = [(i.name.decode() if i.name else "#%d" % i.ordinal) for i in d.imports]
        out.append((d.dll.decode(), names, True))
    return out


def exports(pe):
    d = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
    if d is None:
        return None, []
    name = pe.get_string_at_rva(d.struct.Name)
    return (name.decode(errors="replace") if name else None), \
           [(e.name.decode() if e.name else "#%d" % e.ordinal) for e in d.symbols]


def libraries(data):
    found = []
    for pat, label in LIB_PATTERNS:
        m = re.search(pat, data)
        if m:
            found.append("%s: %s" % (label, m.group(0).decode(errors="replace")))
    return found


def describe(path, all_imports):
    pe = pefile.PE(path)
    oh = pe.OPTIONAL_HEADER
    machine = {0x14C: "i386", 0x8664: "x86-64", 0xAA64: "arm64"}.get(pe.FILE_HEADER.Machine, hex(pe.FILE_HEADER.Machine))
    print("== %s  (%.1f MB)" % (os.path.basename(path), os.path.getsize(path) / 1e6))
    print("machine %s, %s, linker %d.%d, built %s" % (
        machine, "PE32+" if oh.Magic == 0x20B else "PE32", oh.MajorLinkerVersion, oh.MinorLinkerVersion,
        __import__("time").strftime("%Y-%m-%d", __import__("time").gmtime(pe.FILE_HEADER.TimeDateStamp))))
    relocs_stripped = bool(pe.FILE_HEADER.Characteristics & 0x1)
    print("image base 0x%08x, size 0x%x, entry 0x%08x, relocations %s" % (
        oh.ImageBase, oh.SizeOfImage, oh.ImageBase + oh.AddressOfEntryPoint,
        "STRIPPED (must load at the image base)" if relocs_stripped else "present"))
    print("%-9s %-10s %10s %10s %s" % ("section", "address", "vsize", "raw", "RWX"))
    wrappers = []
    for name, va, vs, raw, fl in section_rows(pe):
        print("%-9s 0x%08x %10d %10d %s" % (name, va, vs, raw, fl))
        if name.lower() in WRAPPER_SECTIONS:
            wrappers.append(name)
    print("copy-protection wrapper sections: %s" % (", ".join(wrappers) if wrappers else "none"))
    pdb, age = pdb_path(pe)
    if pdb:
        print("PDB: %s (age %s) - not shipped" % (pdb, age))
    imps = imports(pe)
    total = sum(len(n) for _, n, _ in imps)
    print("imports: %d functions from %d DLLs" % (total, len(imps)))
    for dll, names, delay in sorted(imps, key=lambda x: -len(x[1])):
        tag = " (delay-load)" if delay else ""
        if all_imports:
            print("  %s%s [%d]: %s" % (dll, tag, len(names), ", ".join(names)))
        else:
            shown = ", ".join(names[:6]) + (", ..." if len(names) > 6 else "")
            print("  %-22s %4d  %s" % (dll + tag, len(names), shown))
    ename, enames = exports(pe)
    if enames:
        print("exports: %d (module name %s): %s" % (len(enames), ename, ", ".join(enames[:8]) + (" ..." if len(enames) > 8 else "")))
    with open(path, "rb") as fh:
        libs = libraries(fh.read())
    if libs:
        print("embedded libraries: " + "; ".join(libs))
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True, help="Steam install folder")
    ap.add_argument("--exe", default="Rayman Origins.exe")
    ap.add_argument("--all-imports", action="store_true", help="list every imported function")
    ap.add_argument("--dlls", action="store_true", help="also describe the DLLs shipped next to the executable")
    a = ap.parse_args()
    describe(os.path.join(a.game, a.exe), a.all_imports)
    if a.dlls:
        for f in sorted(os.listdir(a.game)):
            if f.lower().endswith(".dll"):
                describe(os.path.join(a.game, f), a.all_imports)
    return 0


if __name__ == "__main__":
    sys.exit(main())
