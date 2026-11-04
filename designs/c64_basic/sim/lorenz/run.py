#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Lorenz 6510/CIA tests on the Verilator model of c64_core (build.sh),
several at a time; tests/lorenz.py is the same on the board.

    run.py [--disk N ...] [--image X.d64] [--only NAME ...] [--cycles N] [--wait N] [-j N] [--bin tb]

Every test runs on its own from the state saved after a cold start (made on
first use as build/<bin>.state). Results go to stdout and build/results_<bin>.txt.
"""
import argparse
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import d64  # noqa: E402

LORENZ_DIR = os.environ.get("LORENZ_DIR", os.path.expanduser(
    "~/Development/C64/Tools/C64ReverseEngineeringMCP/samples/vice-testprogs/lorenz-2.15"))
SKIP = {"START", "FINISH", "TEMPLATE", "NEXTDISK"}
BUILD = os.path.join(HERE, "build")
RUNDIR = os.path.join(BUILD, "run")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disk", nargs="*", type=int, default=[1, 2, 3, 4])
    ap.add_argument("--image", nargs="*", default=None, help="d64 files instead of DiskN.d64")
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--cycles", type=int, default=30_000_000)
    ap.add_argument("--wait", type=int, default=0,
                    help="cycles to run before RUN: moves the start against the KERNAL's timer IRQ")
    ap.add_argument("-j", type=int, default=1)
    ap.add_argument("--bin", default="tb_lorenz")
    args = ap.parse_args()

    tb = os.path.join(BUILD, args.bin)
    state = os.path.join(BUILD, args.bin + ".state")
    if not os.path.exists(state) or os.path.getmtime(state) < os.path.getmtime(tb):
        subprocess.run([tb, "boot", state], cwd=RUNDIR, check=True)

    images = args.image or [os.path.join(LORENZ_DIR, f"Disk{n}.d64") for n in args.disk]
    prgdir = os.path.join(BUILD, "prg")
    os.makedirs(prgdir, exist_ok=True)
    # The last file on a disk carries the name of the next disk's first test
    # but is NEXTDISK ("please insert disk"): a later file of the same name wins.
    found = {}
    for path in images:
        img = open(path, "rb").read()
        tag = os.path.splitext(os.path.basename(path))[0]
        for name, t, s in d64.entries(img):
            if name.upper() not in SKIP:
                found[name.upper()] = (tag, name, d64.chain(img, t, s))
    only = {o.upper() for o in args.only} if args.only else None
    tests = []
    for key, (tag, name, prg) in found.items():
        if only and key not in only:
            continue
        f = os.path.join(prgdir, f"{tag}_{name}.prg")
        open(f, "wb").write(prg)
        tests.append((tag, name, f))

    def one(t):
        tag, name, f = t
        p = subprocess.run(["nice", "-n", "15", tb, "run", state, f, str(args.cycles), str(args.wait)], cwd=RUNDIR,
                           capture_output=True, text=True)
        out = p.stdout.splitlines()
        result = {0: "ok", 1: "FAIL", 2: "timeout"}.get(p.returncode, f"error {p.returncode}")
        cyc = out[0].split()[-1] if out and out[0].split()[-1].isdigit() else "?"
        return tag, name, result, cyc, out[1:]

    report = open(os.path.join(BUILD, f"results_{args.bin}.txt"), "w")
    counts = {}
    with ThreadPoolExecutor(args.j) as ex:
        for tag, name, result, cyc, screen in ex.map(one, tests):
            counts[result] = counts.get(result, 0) + 1
            line = f"{tag} {name:10s} {result:8s} {cyc:>10s} cycles"
            print(line, flush=True)
            report.write(line + "\n")
            if result != "ok":
                for l in screen[-10:]:
                    print(l)
                    report.write(l + "\n")
    summary = ", ".join(f"{k} {v}" for k, v in sorted(counts.items()))
    print(summary)
    report.write(summary + "\n")
    return 0 if set(counts) <= {"ok"} else 1


if __name__ == "__main__":
    sys.exit(main())
