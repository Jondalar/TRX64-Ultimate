#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.4: merge per-SID register logs (cores/trx_sid/model/gen_logs.py format
# "<idle> W <reg> <val>" / "<idle> R <reg>") into one CPU op list, one op per
# C64 cycle: "<cycle> <sid> W|R <reg> <val>". An op that is due while the bus
# is taken waits, and so does the rest of its log (the lower SID wins).
# POT reads ($19/$1A) are left out: the wrapper owns POT, not the engine.
#   mklog.py <out> <log0> [<log1> ... <log7>] [--max <cycles>]
import sys

args = sys.argv[1:]
mx = None
if "--max" in args:
    i = args.index("--max")
    mx = int(args[i + 1])
    del args[i:i + 2]
out, logs = args[0], args[1:]


def load(p):
    q, c = [], 0
    for line in open(p):
        t = line.split("#")[0].split()
        if len(t) < 2:
            continue
        c += int(t[0])
        reg = int(t[2], 16) & 31
        if t[1] == "W":
            q.append([c, "W", reg, int(t[3], 16) & 255])
        elif reg not in (0x19, 0x1A):
            q.append([c, "R", reg, 0])
        c += 1
    return q


q = [load(p) for p in logs]
idx = [0] * len(q)
cyc = 0
with open(out, "w") as f:
    while True:
        done = all(idx[s] >= len(q[s]) for s in range(len(q)))
        if done or (mx is not None and cyc >= mx):
            break
        who = -1
        for s in range(len(q)):
            if idx[s] < len(q[s]) and q[s][idx[s]][0] <= cyc:
                who = s
                break
        if who < 0:
            # nothing due: jump to the earliest due time
            cyc = min(q[s][idx[s]][0] for s in range(len(q)) if idx[s] < len(q[s]))
            continue
        for s in range(len(q)):
            if s != who and idx[s] < len(q[s]) and q[s][idx[s]][0] <= cyc:
                for o in q[s][idx[s]:]:
                    o[0] += 1
        o = q[who][idx[who]]
        idx[who] += 1
        f.write("%d %d %s %02x %02x\n" % (cyc, who, o[1], o[2], o[3]))
        cyc += 1
    f.write("%d\n" % (cyc + 4))   # total cycles to run
