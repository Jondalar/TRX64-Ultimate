#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""U20 F-b (D25): the comparison of job `report` of .github/workflows/ephemeral-b.yml, as Markdown on stdout.

    ci/ephemeral/vivado_report.py ART_DIR [BUILDER_DIR]

ART_DIR      the artifact of job `work` (ci/ephemeral/vivado.sh): info.txt, times.txt, warm/, cold/
BUILDER_DIR  the builder's release fw-bit-c64u-<key> of the same key, its reports.tar.gz unpacked beside wns.txt and key;
             missing when the builder has not built this key (then only the two Hetzner builds are shown)
Environment (job `start`): T_CREATE, T_RUNNING, T_ONLINE (epoch seconds), T_JOB (job `work`'s first step), SERVER_TYPE,
SERVER_LOCATION, PRICE_HOURLY (EUR net, the API's price).

Compared per design: Vivado's own elapsed time per command (*.steps.txt, the lines ci/bitstream.sh keeps), their sum, WNS
(wns.txt), and for c64_basic the utilization (Slice LUTs, Slice Registers, Block RAM Tile, DSPs of *.utilization.rpt).
The bitstreams are not compared byte for byte: the .bit header carries the build's date and time, and placement may
differ between machines; equal criteria are timing met and the board.
"""
import math
import os
import re
import sys

DESIGNS = ("c64_basic", "ddr2_test", "hdmi_test")
UTIL = ("Slice LUTs", "Slice Registers", "Block RAM Tile", "DSPs")


def read(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return None


def secs(hms):
    h, m, s = hms.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def steps(d, design):
    """[(command, seconds)] in the order Vivado ran them; None if the file is missing."""
    t = read(os.path.join(d, design + ".steps.txt")) if d else None
    if t is None:
        return None
    out = []
    for line in t.splitlines():
        m = re.match(r"^([a-z_]+) ([0-9:.]+) ", line)
        if m:
            out.append((m.group(1), secs(m.group(2))))
    return out


def wns(d):
    t = read(os.path.join(d, "wns.txt")) if d else None
    return dict(line.split()[:2] for line in (t or "").splitlines() if len(line.split()) >= 2)


def util(d, design):
    t = read(os.path.join(d, design + ".utilization.rpt")) if d else None
    res = {}
    for name in UTIL:
        m = re.search(r"^\| " + re.escape(name) + r"\s*\|\s*([0-9.]+)", t or "", re.M)
        res[name] = m.group(1) if m else "-"
    return res


def kv(path):
    t = read(path) or ""
    return dict(line.split(None, 1) for line in t.splitlines() if len(line.split(None, 1)) == 2)


def mmss(s):
    if s is None:
        return "-"
    s = int(round(s))
    return "%d:%02d" % (s // 60, s % 60)


def main():
    art = sys.argv[1]
    bld = sys.argv[2] if len(sys.argv) > 2 and os.path.isdir(sys.argv[2]) else None
    info = kv(os.path.join(art, "info.txt"))
    times = {k: int(v) for k, v in kv(os.path.join(art, "times.txt")).items() if v.strip().isdigit()}
    cols = [("builder", bld), ("Hetzner warm", os.path.join(art, "warm")), ("Hetzner cold", os.path.join(art, "cold"))]
    cols = [(n, d) for n, d in cols if d and os.path.isdir(d)]
    env = os.environ.get
    t_create = int(env("T_CREATE") or 0)
    price = float(env("PRICE_HOURLY") or 0)

    p = print
    p("## U20 F-b: Vivado on a Hetzner server made from the snapshot")
    p()
    p("| | |")
    p("|---|---|")
    p("| server | %s in %s, EUR %s per hour net, %s cores (`nproc`) |" % (
        env("SERVER_TYPE", "?"), env("SERVER_LOCATION", "?"), env("PRICE_HOURLY", "?"), info.get("nproc", "?")))
    p("| Vivado threads | %s (general.maxThreads) |" % info.get("threads", "?"))
    p("| image | `%s` %s, cache %s, snapshot made %s |" % (
        info.get("image_ref", "?"), info.get("image_id", "?")[:19], info.get("cache", "?"), info.get("made", "?")))
    keys = {n: (read(os.path.join(d, "key")) or "-").strip() for n, d in cols}
    p("| gateware keys | %s |" % ", ".join("%s `%s`" % (n, k) for n, k in keys.items()))
    if len(set(keys.values())) > 1:
        p("| **note** | the keys differ: the builds are not of the same gateware |")
    p()

    if t_create:
        p("### Seconds after the create request")
        p()
        p("| server running | runner online | job start | warm build | cold build |")
        p("|---|---|---|---|---|")
        rel = lambda k: str(int(env(k)) - t_create) if env(k) else "-"
        span = lambda b: ("%d - %d" % (times[b + "_start"] - t_create, times[b + "_end"] - t_create)
                          if b + "_start" in times and b + "_end" in times else "-")
        p("| %s | %s | %s | %s | %s |" % (rel("T_RUNNING"), rel("T_ONLINE"), rel("T_JOB"), span("warm"), span("cold")))
        p()
        end = max(times.values()) if times else 0
        if end and price:
            hours = math.ceil((end - t_create + 60) / 3600.0)
            p("Server from the create request to the end of the builds: %s min (deleted about a minute later). Hetzner bills"
              " every started hour: %d h x EUR %.4f = **EUR %.4f** net." % (mmss(end - t_create), hours, price, hours * price))
            p()
        for b in ("warm", "cold"):
            if b + "_failed" in times:
                p("**The %s build failed**: see its log.txt in the artifact." % b)
                p()

    p("### Vivado per design (elapsed, the sum of Vivado's commands; WNS ns)")
    p()
    p("| design | " + " | ".join(n for n, _ in cols) + " |")
    p("|---|" + "---|" * len(cols))
    ws = {n: wns(d) for n, d in cols}
    for design in DESIGNS:
        cells = []
        for n, d in cols:
            st = steps(d, design)
            cells.append("%s, WNS %s" % (mmss(sum(s for _, s in st)) if st else "-", ws[n].get(design, "-")))
        p("| %s | %s |" % (design, " | ".join(cells)))
    total = []
    for n, d in cols:
        sts = [steps(d, x) for x in DESIGNS]
        total.append(mmss(sum(s for st in sts if st for _, s in st)))
    p("| **all** | %s |" % " | ".join(total))
    p()

    p("### c64_basic per command (elapsed)")
    p()
    p("| command | " + " | ".join(n for n, _ in cols) + " |")
    p("|---|" + "---|" * len(cols))
    per = {n: steps(d, "c64_basic") or [] for n, d in cols}
    rows = max((len(v) for v in per.values()), default=0)
    for i in range(rows):
        name = next((per[n][i][0] for n, _ in cols if i < len(per[n])), "?")
        p("| %s | %s |" % (name, " | ".join(mmss(per[n][i][1]) if i < len(per[n]) else "-" for n, _ in cols)))
    p()

    p("### c64_basic utilization")
    p()
    p("| | " + " | ".join(n for n, _ in cols) + " |")
    p("|---|" + "---|" * len(cols))
    us = {n: util(d, "c64_basic") for n, d in cols}
    for name in UTIL:
        p("| %s | %s |" % (name, " | ".join(us[n][name] for n, _ in cols)))


if __name__ == "__main__":
    main()
