"""PE32 image helper for the function-discovery tools: the executable loaded as the process would see it.

What it does: maps the sections at their virtual addresses (image base 0x00400000), reads bytes and dwords by virtual
address, tells which section an address is in, and names the import address table (IAT) slots ("KERNEL32.dll!ExitProcess")
and the exports. Reads the file only.
What it does not do: disassemble (discover.py) or apply relocations (the lifted image always loads at its preferred base, so relocations are only used as pointer evidence).
"""
import os
import struct

import pefile

DEFAULT_GAME = os.path.expanduser(os.environ.get("RAYMAN_GAME", "~/.local/share/Steam/steamapps/common/ValveTestApp207490"))
DEFAULT_EXE = "Rayman Origins.exe"


class Image:
    def __init__(self, path):
        pe = pefile.PE(path)
        self.path = path
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.mem = bytearray(self.size)
        self.sections = {}
        for s in pe.sections:
            name = s.Name.rstrip(b"\0").decode()
            data = s.get_data()[: s.Misc_VirtualSize]
            self.mem[s.VirtualAddress: s.VirtualAddress + len(data)] = data
            self.sections[name] = (self.base + s.VirtualAddress, self.base + s.VirtualAddress + s.Misc_VirtualSize)
        self.text_start, self.text_end = self.sections[".text"]
        self.iat = {}
        for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            for i in d.imports:
                name = i.name.decode() if i.name else "#%d" % i.ordinal
                self.iat[i.address] = "%s!%s" % (d.dll.decode(), name)
        for d in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []):
            for i in d.imports:
                name = i.name.decode() if i.name else "#%d" % i.ordinal
                self.iat[i.address] = "%s!%s" % (d.dll.decode(), name)
        # base relocations (HIGHLOW): the addresses that hold absolute pointers, and what they point at. Every one is
        # a real pointer (the linker wrote it), so a target in .text that no function covers is a function start.
        self.reloc_targets = {}
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
        for blk in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []):
            for e in blk.entries:
                if e.type == 3:
                    at = self.base + e.rva
                    self.reloc_targets[at] = struct.unpack_from("<I", self.mem, e.rva)[0]
        self.exports = {}
        exp = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
        if exp is not None:
            for e in exp.symbols:
                if e.name:
                    self.exports[self.base + e.address] = e.name.decode()

    def in_text(self, va):
        return self.text_start <= va < self.text_end

    def section_of(self, va):
        for name, (lo, hi) in self.sections.items():
            if lo <= va < hi:
                return name
        return None

    def u8(self, va):
        return self.mem[va - self.base]

    def u32(self, va):
        o = va - self.base
        if o < 0 or o + 4 > self.size:
            return None
        return struct.unpack_from("<I", self.mem, o)[0]

    def bytes_at(self, va, n):
        o = va - self.base
        return bytes(self.mem[o: o + n])

    def text_bytes(self):
        return bytes(self.mem[self.text_start - self.base: self.text_end - self.base])
