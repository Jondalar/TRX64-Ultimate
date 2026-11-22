# SPDX-License-Identifier: GPL-3.0-or-later
"""The boundary of trx64u's libraries (U18, software/trx64u/BOUNDARY.md).

    python3 tools/boundary.py <coreapp output dir> <result.json>

<coreapp output dir> is host-app/target/u64ii/riscv/coreapp/output of a MultiCore tree whose core application was
built (tools/build_coreapp.sh). For every object of the files that move (LIBS), it lists the symbols the object uses
that no moving object defines (minus the SDK's libc.c), with the core application's object that defines each one;
the calls between our libraries; and the core application's objects that call into the moving code. Needs
riscv64-elf-nm.
"""
import subprocess, sys, os, re, collections, json
out = sys.argv[1]            # coreapp output dir
NM = "riscv64-elf-nm"
LIBS = {
 "trx64u": ["trx64u_ops","u64cfg"],
 "ring":   ["ring","ringfile","ringops"],
 "monitor":["monitor","monsrv"] + [o[:-2] for o in os.listdir(out) if o.startswith("tm_") and o.endswith(".o")],
}
SDK_LIBC = set("memcpy memmove memset memcmp strlen strcmp strncmp strcpy strncpy strcat strchr strrchr __udivsi3 __umodsi3 __divsi3 __modsi3 __ashldi3 __lshrdi3 __ashrdi3".split())
def nm(path, flags):
    r = subprocess.run([NM, *flags, path], capture_output=True, text=True)
    return r.stdout.splitlines()
def defined(o):
    s = set()
    for l in nm(o, ["--defined-only", "-g"]):
        p = l.split()
        if len(p) >= 3: s.add(p[2])
    return s
def undef(o):
    return {l.split()[-1] for l in nm(o, ["-u"]) if l.strip()}
alldefs = {}
for f in os.listdir(out):
    if f.endswith(".o"):
        for s in defined(os.path.join(out, f)):
            alldefs.setdefault(s, []).append(f[:-2])
mov = {m: lib for lib, ms in LIBS.items() for m in ms}
moved_defs = {}
for m, lib in mov.items():
    for s in defined(os.path.join(out, m + ".o")):
        moved_defs[s] = lib
res = collections.defaultdict(lambda: collections.defaultdict(set))   # lib -> provider -> {(sym, user)}
cross = collections.defaultdict(set)
for m, lib in mov.items():
    for s in undef(os.path.join(out, m + ".o")):
        if s in moved_defs:
            if moved_defs[s] != lib: cross[(lib, moved_defs[s])].add((s, m))
            continue
        if s in SDK_LIBC: continue
        prov = ",".join(sorted(p for p in alldefs.get(s, []) if p not in mov)) or "(libc/lib .a)"
        res[lib][prov].add((s, m))
# reverse: app core objects that use moved symbols
rev = collections.defaultdict(set)
for f in os.listdir(out):
    if f.endswith(".o") and f[:-2] not in mov:
        for s in undef(os.path.join(out, f)):
            if s in moved_defs: rev[f[:-2]].add((s, moved_defs[s]))
json.dump({"res": {l: {p: sorted(v) for p, v in d.items()} for l, d in res.items()},
           "cross": {f"{a}->{b}": sorted(v) for (a, b), v in cross.items()},
           "rev": {k: sorted(v) for k, v in rev.items()}}, open(sys.argv[2], "w"), indent=1)
for lib, d in res.items():
    n = sum(len(v) for v in d.values())
    print(f"== {lib}: {n} external symbols from {len(d)} providers")
print("cross:", {k: len(v) for k, v in cross.items()})
print("reverse users:", {k: len(v) for k, v in sorted(rev.items())})
