#!/usr/bin/env python3
"""The repository rules that a program can check (docs/RULES.md): every tracked file has at most 700 lines, and no
tracked file is game content (executables, DLLs, UbiArt archives, shaders, captures, keys, anything under private/). Run it before a
commit and in every pull request. Exit code 1 when a rule is broken.

usage: python3 tools/check_repo.py            (checks the files `git ls-files` lists, plus the staged ones)
"""
import os
import subprocess
import sys

MAX_LINES = 700
FORBIDDEN_SUFFIX = (".dll", ".exe", ".pdb", ".rdc", ".keys", ".jks", ".keystore", ".apk", ".so",
                    # UbiArt / Rayman Origins data formats
                    ".ipk", ".bik", ".fx", ".fxo", ".dds", ".tga", ".ckd", ".isg", ".isc", ".act", ".spv")
FORBIDDEN_NAME = ("signature",)
BINARY_OK = (".jpg", ".jpeg", ".png", ".gif", ".webp")  # screenshots: not counted in lines


def git_files():
    root = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip()
    out = subprocess.check_output(["git", "-C", root, "ls-files", "--cached"], text=True).splitlines()
    return root, sorted(set(out))


def main():
    root, files = git_files()
    bad = 0
    for f in files:
        low = f.lower()
        base = os.path.basename(low)
        if low.startswith("private/") or low.endswith(FORBIDDEN_SUFFIX) or base in FORBIDDEN_NAME:
            print("GAME CONTENT / KEY tracked:", f)
            bad += 1
            continue
        if low.endswith(BINARY_OK):
            continue
        path = os.path.join(root, f)
        if not os.path.isfile(path):
            continue
        with open(path, "rb") as fh:
            n = sum(1 for _ in fh)
        if n > MAX_LINES:
            print("TOO LONG (%d lines, max %d): %s" % (n, MAX_LINES, f))
            bad += 1
    print("checked %d files: %s" % (len(files), "OK" if not bad else "%d problem(s)" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
