#!/usr/bin/env python3
"""Function discovery for Rayman Origins.exe (M2): every function start and extent the lifter will need, with coverage numbers.

x86-32 PE files have no `.pdata`, so nothing lists the functions. They are found in rounds:
  1. seeds: the entry point and the exports;
  2. recursive descent from each seed (descent.py); every direct call target found becomes a new seed;
  3. code pointers in .rdata/.data (vtables, callback tables) and code addresses used as values in instructions
     (`push offset f`), when they do not land inside a function already found;
  4. gap fill: bytes of .text still unclaimed that are not padding (`int3`/`nop`) or switch-table data start a function;
  5. functions that never return (no `ret`, every path ends in a call that does not return) are marked, and the
     rounds are repeated until nothing changes.
What it does: writes private/analysis/functions.json (starts, ends, sizes, calls, switch tables) and prints coverage.
What it does not do: name functions (M3) or prove each start is right: unclaimed bytes are reported, not hidden.

usage: .venv/bin/python -I tools/funcs/discover.py [--game <Steam folder>] [--out private/analysis]
"""
import argparse
import bisect
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from descent import Descent  # noqa: E402
from pe32 import DEFAULT_EXE, DEFAULT_GAME, Image  # noqa: E402

CODE, TABLE, PAD = 1, 2, 3
FILLERS = (b"\x8d\xa4\x24\x00\x00\x00\x00", b"\x8d\x9b\x00\x00\x00\x00", b"\x8d\x64\x24\x00",
           b"\x8d\x49\x00", b"\x8d\x40\x00", b"\x8b\xff", b"\x8b\xc0", b"\x8d\x36", b"\x8d\x3f")
TRACE = {int(x, 16) for x in os.environ.get("DISCOVER_TRACE", "").split(",") if x}
WEAK = ("pointer", "code-ref", "gap")   # origins that can be data: rejected when they do not decode as a function
STRENGTH = {"entry": 3, "export": 3, "call": 3, "vtable": 3, "reloc": 3, "extra": 3, "thunk": 3, "code-ref": 2, "gap": 2, "pointer": 1}
PROLOGUES = (b"\x55\x8b\xec", b"\x8b\xff\x55")   # push ebp; mov ebp,esp (and the hot-patch form)


class Discovery:
    def __init__(self, img):
        self.img = img
        self.d = Descent(img)
        self.funcs = {}                  # start -> Func
        self.starts = set()
        self.sorted_starts = []
        self.origin = {}                 # start -> how it was found
        self.owner = bytearray(img.text_end - img.text_start)
        self.rejected = set()            # weak candidates that turned out not to be code
        self.insn_start = bytearray(img.text_end - img.text_start)   # 1 where a decoded instruction begins

    def add_start(self, a, why):
        if a not in self.starts and a not in self.rejected and self.img.in_text(a):
            self.starts.add(a)
            bisect.insort(self.sorted_starts, a)
            self.origin.setdefault(a, why)
            return True
        return False

    def next_start(self, a):
        i = bisect.bisect_right(self.sorted_starts, a)
        return self.sorted_starts[i] if i < len(self.sorted_starts) else self.img.text_end

    def claimed(self, a):
        return self.owner[a - self.img.text_start] != 0

    def mark(self, lo, hi, kind):
        t0 = self.img.text_start
        lo, hi = max(lo, t0) - t0, min(hi, self.img.text_end) - t0
        if hi > lo:
            self.owner[lo:hi] = bytes([kind]) * (hi - lo)

    def rebuild_owner(self):
        """The coverage map from the functions as they are now (padding found earlier is kept)."""
        pad = [i for i, b in enumerate(self.owner) if b == PAD] if PAD in self.owner else []
        self.owner = bytearray(len(self.owner))
        self.insn_start = bytearray(len(self.owner))
        for i in pad:
            self.owner[i] = PAD
        t0 = self.img.text_start
        for f in self.funcs.values():
            for a, size in f.insns.items():
                self.mark(a, a + size, CODE)
                self.insn_start[a - t0] = 1
            for t in f.tables:
                for lo, hi in t.data_ranges():
                    self.mark(lo, hi, TABLE)

    def strength(self, a):
        k = STRENGTH.get(self.origin.get(a, "pointer").split(":")[0], 1)
        k += 1 if self.img.bytes_at(a, 3).startswith(PROLOGUES) else 0
        t0 = self.img.text_start
        if self.img.in_text(a - 3):
            pad = self.img.u8(a - 1) in (0xCC, 0x90) and self.owner[a - 1 - t0] != CODE   # not a displacement byte
            ret = self.img.u8(a - 1) == 0xC3 or self.img.u8(a - 3) == 0xC2                # right after `ret` / `ret n`
            if pad or (ret and self.owner[a - 1 - t0] == CODE):
                k += 2                   # at a function boundary: where the compiler puts functions
        return k

    def weaker(self, sp, s):
        """True when start sp (inside an instruction of function s) is less trustworthy than s, so sp goes."""
        return self.origin.get(sp, "").startswith(WEAK) and self.strength(sp) <= self.strength(s)

    def is_garbage(self, f):
        """A weak candidate is rejected when a path hits bytes that do not decode or it starts with a trap."""
        if self.origin.get(f.start, "") in ("entry", "export", "extra", "call", "thunk"):
            return False                 # vtable / orphan / weak starts come from data and are checked
        if f.bad or not f.insns:
            return True
        first = f.insns.get(f.start)
        if first is None or self.img.u8(f.start) == 0xCC and first == 1:
            return True
        return self.img.bytes_at(f.start, 2) == b"\x00\x00"     # `add [eax], al`: zero bytes, not code

    def reject(self, s, why="?"):
        if s in TRACE:
            print("TRACE reject %x (%s): %s" % (s, self.origin.get(s), why))
        self.rejected.add(s)
        self.starts.discard(s)
        i = bisect.bisect_left(self.sorted_starts, s)
        if i < len(self.sorted_starts) and self.sorted_starts[i] == s:
            self.sorted_starts.pop(i)
        self.funcs.pop(s, None)

    def explore_all(self):
        """Descends every start not explored yet; new call targets join the queue. Returns how many were explored."""
        n = 0
        queue = [s for s in self.sorted_starts if s not in self.funcs]
        while queue:
            s = queue.pop()
            if s in self.funcs:
                continue
            f = self.d.run(s, self.starts.__contains__, self.next_start(s))
            n += 1
            weak_spans = [sp for sp in f.spans if self.origin.get(sp, "") not in ("entry", "export", "extra") and
                          (self.strength(sp) < self.strength(s) or
                           (self.strength(sp) == self.strength(s) and self.origin.get(sp, "").startswith(WEAK)))]
            if weak_spans:
                for sp in weak_spans:    # a pointer that lands inside one of our instructions is data, not a start
                    self.reject(sp, "inside an instruction of %x" % s)
                queue.append(s)
                continue
            labels = [sp for sp in f.flows_into if self.origin.get(sp, "").startswith(WEAK)
                      and not self.img.bytes_at(sp, 3).startswith(PROLOGUES)]
            if labels:
                for sp in labels:        # a weak "start" our own code flows into is one of our labels
                    self.reject(sp, "label of %x" % s)
                queue.append(s)
                continue
            if f.spans and self.origin.get(s, "") not in ("entry", "export", "extra") and \
                    any(self.strength(sp) > self.strength(s) or
                        (self.strength(sp) == self.strength(s) and self.origin.get(s, "").startswith(WEAK))
                        for sp in f.spans):
                self.reject(s, "spans stronger %s" % [hex(x) for x in f.spans])   # s is the misaligned one
                continue
            if self.is_garbage(f):
                self.reject(s, "garbage (bad=%s insns=%d)" % (f.bad, len(f.insns)))
                continue
            self.funcs[s] = f
            for c in f.calls | f.tails:
                if self.add_start(c, "call"):
                    queue.append(c)
                    self.split_owner(c, queue)
        self.rebuild_owner()
        return n

    def split_owner(self, c, queue):
        """A new start inside an explored function's range shortens that function: explore it again."""
        i = bisect.bisect_left(self.sorted_starts, c)
        if i == 0:
            return
        prev = self.sorted_starts[i - 1]
        f = self.funcs.get(prev)
        if f is not None and f.end > c:
            del self.funcs[prev]
            queue.append(prev)

    def pointer_seeds(self):
        """Dword-aligned code pointers in .rdata/.data plus code addresses used as instruction values."""
        img, added = self.img, 0
        for sec in (".rdata", ".data"):
            lo, hi = img.sections[sec]
            for a in range(lo, hi - 3, 4):
                v = img.u32(a)
                if v is None or not img.in_text(v):
                    continue
                vtable_like = any(img.in_text(img.u32(a + k) or 0) for k in (-4, 4))
                aligned = not self.claimed(v) or self.insn_start[v - img.text_start]   # not mid-instruction
                if aligned and (vtable_like or img.bytes_at(v, 3).startswith(PROLOGUES)) and v not in self.starts:
                    # a pointer in a vtable / callback table (code-pointer neighbours) or to a classic prologue is a
                    # function even where another candidate decoded over it misaligned: strong, so that one goes
                    if self.add_start(v, "vtable"):
                        added += 1
                        self.split_owner(v, [])
                elif not self.claimed(v) and self.add_start(v, "pointer:" + sec):
                    added += 1
        for f in list(self.funcs.values()):
            for v in f.code_refs:
                if not self.claimed(v) and self.add_start(v, "code-ref"):
                    added += 1
        for at, v in img.reloc_targets.items():      # relocated pointers to code nobody covers: functions
            if img.in_text(v) and not self.claimed(v) and v not in self.starts and self.add_start(v, "reloc"):
                added += 1
        return added

    def gap_seeds(self):
        """Candidates in unclaimed .text: the first byte after each padding run (`int3`/`nop`) and every
        `push ebp; mov ebp,esp` prologue. Padding runs are marked as padding."""
        img, t0, added = self.img, self.img.text_start, 0
        i, n = 0, len(self.owner)
        mem = img.text_bytes()
        while i < n:
            if self.owner[i]:
                i += 1
                continue
            j = i
            while j < n and not self.owner[j]:
                j += 1
            k = i
            ka = (k + 3) & ~3                # tables are 4-aligned: the run may start in their alignment bytes
            if ka + 4 <= j and img.in_text(int.from_bytes(mem[ka:ka + 4], "little")) and \
                    img.in_text(int.from_bytes(mem[ka + 4:ka + 8], "little")):
                if ka > k:
                    self.mark(t0 + k, t0 + ka, PAD)   # the table's alignment bytes
                k = ka
            while k + 4 <= j and img.in_text(int.from_bytes(mem[k:k + 4], "little")):
                k += 4                       # leftover switch-table entries (code addresses) before the code
            if k > i:
                self.mark(t0 + i, t0 + k, TABLE)
            at_boundary = True               # a run starts right after code, a table or padding
            while k < j:
                filler = next((len(f) for f in FILLERS if mem[k:k + len(f)] == f and k + len(f) <= j), 0)
                if filler and at_boundary:    # MSVC's multi-byte nops that align a table or the next function
                    self.mark(t0 + k, t0 + k + filler, PAD)
                    k += filler
                    while k + 4 <= j and img.in_text(int.from_bytes(mem[k:k + 4], "little")):
                        self.mark(t0 + k, t0 + k + 4, TABLE)
                        k += 4
                    continue
                if mem[k] in (0xCC, 0x90):
                    p = k
                    while k < j and mem[k] in (0xCC, 0x90):
                        k += 1
                    self.mark(t0 + p, t0 + k, PAD)
                    at_boundary = True
                    continue
                if (at_boundary or mem[k:k + 3] in PROLOGUES) and self.add_start(t0 + k, "gap"):
                    added += 1
                at_boundary = False
                k += 1
            i = j
        return added

    def settle(self, max_passes=8):
        """Re-explores functions explored before a start inside their code was known (they decoded another function's
        code as their own). Returns the number re-explored per pass."""
        passes = []
        for _ in range(max_passes):
            stale = []
            for s, f in self.funcs.items():
                nxt = self.next_start(s)
                if any((a != s and a in self.starts) or a >= nxt for a in f.insns):
                    stale.append(s)
            if not stale:
                break
            for s in stale:
                del self.funcs[s]
            self.explore_all()
            passes.append(len(stale))
        return passes

    def demote_orphans(self):
        """A call target that no surviving function calls came from a candidate that was rejected: weak again."""
        refs = set()
        for f in self.funcs.values():
            refs |= f.calls | f.tails
        n = 0
        for st in self.starts:
            if self.origin.get(st) == "call" and st not in refs:
                self.origin[st] = "pointer:orphan"
                n += 1
        return n

    def mark_noreturn(self):
        """Functions whose every path ends in a trap or a call that does not return (Func.never_returns)."""
        before = len(self.d.noreturn)
        for s, f in self.funcs.items():
            if f.never_returns():
                self.d.noreturn.add(s)
        return len(self.d.noreturn) - before

    def reexplore_callers_of_noreturn(self):
        hit = [s for s, f in self.funcs.items() if f.calls & self.d.noreturn]
        for s in hit:
            del self.funcs[s]
        self.rebuild_owner()
        return len(hit)


def coverage(disc):
    c = {CODE: 0, TABLE: 0, PAD: 0, 0: 0}
    for b in disc.owner:
        c[b] += 1
    return {"code": c[CODE], "table": c[TABLE], "padding": c[PAD], "unclaimed": c[0], "total": len(disc.owner)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--out", default="private/analysis")
    ap.add_argument("--exe", default=DEFAULT_EXE, help="module in the game folder (Rayman Origins.exe, cryptopp.dll)")
    ap.add_argument("--max-rounds", type=int, default=8)
    a = ap.parse_args()
    t_start = time.time()
    img = Image(os.path.join(a.game, a.exe))
    disc = Discovery(img)
    disc.add_start(img.entry, "entry")
    for va in img.exports:
        disc.add_start(va, "export")
    mem = img.text_bytes()                 # runs of `jmp dword ptr [abs]` thunks (6 bytes apart): one function each
    i = mem.find(b"\xff\x25")
    while i != -1:
        if mem[i + 6:i + 8] == b"\xff\x25" or mem[i - 6:i - 4] == b"\xff\x25":
            if img.section_of(int.from_bytes(mem[i + 2:i + 6], "little")) in (".rdata", ".data"):
                disc.add_start(img.text_start + i, "thunk")
        i = mem.find(b"\xff\x25", i + 1)
    extra = os.path.join(a.out, "extra_starts.txt")   # starts reported by the runtime / difftest (indirect targets)
    if os.path.exists(extra):
        for line in open(extra):
            if line.strip() and not line.startswith("#"):
                disc.add_start(int(line.split()[0], 16), "extra")
    print("round  explored  +pointer  +gap  +noreturn  functions  unclaimed")
    for r in range(a.max_rounds):
        disc.rejected.clear()        # a candidate rejected against a wrong neighbour gets another chance
        n = disc.explore_all()
        p = disc.pointer_seeds()
        n += disc.explore_all()
        g = 0
        for _ in range(400):         # runs of small functions (import thunks) fill one function per pass
            gi = disc.gap_seeds()
            if not gi:
                break
            g += gi
            n += disc.explore_all()
        nr = disc.mark_noreturn()
        if nr:
            disc.reexplore_callers_of_noreturn()
            n += disc.explore_all()
        cov = coverage(disc)
        print("%5d  %8d  %8d  %4d  %9d  %9d  %9d" % (r + 1, n, p, g, nr, len(disc.funcs), cov["unclaimed"]))
        if not (p or g or nr):
            break

    passes = disc.settle()
    for _ in range(8):               # code only the stale paths reached is orphaned: fill those gaps, settle again
        disc.rejected.clear()        # a rejection made against a neighbour that has changed since does not stand
        disc.demote_orphans()
        for st in [x for x in disc.sorted_starts if disc.origin.get(x) == "gap" and x not in disc.funcs]:
            disc.funcs.pop(st, None)
        g = disc.pointer_seeds()     # (and pointers whose target was freed by a rejected candidate)
        disc.explore_all()
        for _ in range(400):
            gi = disc.gap_seeds()
            if not gi:
                break
            g += gi
            disc.explore_all()
        settled = disc.settle()
        passes += settled
        if not g and not settled:    # stable: no new seed and nothing re-explored
            break
    print("settle: re-explored per pass %s" % passes)
    cov = coverage(disc)
    funcs = sorted(disc.funcs.values(), key=lambda f: f.start)
    origins = {}
    for f in funcs:
        k = disc.origin.get(f.start, "?").split(":")[0]
        origins[k] = origins.get(k, 0) + 1
    tables = [t for f in funcs for t in f.tables]
    seen = bytearray(img.text_end - img.text_start)
    for f in funcs:
        for ad, size in f.insns.items():
            for k in range(ad - img.text_start, ad - img.text_start + size):
                if seen[k] < 255:
                    seen[k] += 1
    shared = sum(1 for b in seen if b > 1)
    stats = {
        "functions": len(funcs), "instructions": sum(len(f.insns) for f in funcs), "rejected": len(disc.rejected),
        "by_origin": origins, "returning": sum(f.returns for f in funcs), "noreturn": len(disc.d.noreturn),
        "with_overrun": sum(1 for f in funcs if f.overruns), "undecodable_paths": sum(1 for f in funcs if f.bad),
        "switch_tables": len(tables), "switch_tables_unbounded": sum(1 for t in tables if not t.bounded),
        "indirect_jumps_not_tables": sum(f.indirect_tail for f in funcs), "coverage_bytes": cov,
        "code_bytes_in_2plus_functions": shared, "settle_passes": passes,
        "seconds": round(time.time() - t_start, 1),
    }
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "functions.json"), "w") as fh:
        json.dump({"stats": stats, "noreturn": sorted(disc.d.noreturn), "functions": [
            {"start": f.start, "end": f.end, "insns": len(f.insns), "returns": f.returns,
             "origin": disc.origin.get(f.start, "?"), "calls": sorted(f.calls), "tails": sorted(f.tails),
             "tables": [{"jump": t.jump_at, "table": t.table, "count": t.count, "index": t.index_table,
                         "bounded": t.bounded, "targets": t.targets} for t in f.tables],
             "overruns": f.overruns, "bad": f.bad} for f in funcs]}, fh)
    with open(os.path.join(a.out, "text_owner.bin"), "wb") as fh:
        fh.write(bytes(disc.owner))
    print()
    for k, v in stats.items():
        if k == "coverage_bytes":
            tot = v["total"]
            for kk in ("code", "table", "padding", "unclaimed"):
                print("  .text %-10s %9d bytes  %5.2f%%" % (kk, v[kk], 100.0 * v[kk] / tot))
        else:
            print("%-26s %s" % (k, v))
    return 0


if __name__ == "__main__":
    sys.exit(main())
