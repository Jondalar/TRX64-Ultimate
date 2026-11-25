#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# PROVENANCE: TRX64U-MultiCore ci/dispatch.py (t239.3 d9ddadeb1; Alex Damhuis' code in both repositories), U20 B.
# Changed here: our three lanes (tree, bits:c64_basic, core:trx64u; no SDK lane, no host lane), and the plan is made here (MultiCore's ci/plan.sh
# decides per core what changed; we have one bitstream and one core, so the plan is the base commit and the count of changed files).
"""U20 B (MultiCore T239.5 D9): the dispatcher of the matrix - what this push has to do. It runs in the first job of .github/workflows/matrix.yml and
decides the lanes and their steps (T239.5 D8). Every lane starts on every push; a lane whose inputs did not change is served from its content-keyed
caches (the bitstream release of its key, the bench markers). The benches of the bitstream lane run on demand only (tier > 0, ci/select.sh sims-demand).

  ci/dispatch.py --plan-out FILE --tier N
      writes plan.json (version 1: sha, branch, base commit, changed files) to FILE; prints `key=value` lines for $GITHUB_OUTPUT: lint_groups, bits,
      cores, sims (JSON lists) and sims_tier; appends the matrix of this run (lanes from top to bottom, the steps from left to right) to
      $GITHUB_STEP_SUMMARY when that is set.
    env  BASE_SHA    the commit this push is compared with (the push's `before`, the pull request's base); not a commit of the checkout = no base
         CI_BRANCH   the ref of the run (else GITHUB_REF_NAME, else the checkout's branch)
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
STEPS = ["Lint", "Unit tests", "What has changed?", "Comparison", "What must be in it", "Build", "Complete and fits", "Tests: mock bitstream -> service",
         "Tests: service -> UI", "Tests: service -> API", "Package", "Emulator tests", "Deliver"]
BITS = ["c64_basic"]      # one bitstream lane: the entry c64u-<key> of ci/keys.sh (every designs/*/build.tcl; c64_basic is the one the core packs)
CORES = ["trx64u"]        # one core lane: software/trx64u, packed with the bitstream into trx64u.cor


def git(*args):
    return subprocess.run(["git", "-C", ROOT] + list(args), capture_output=True, text=True)


def plan(base_env, branch):
    sha = git("rev-parse", "HEAD").stdout.strip()
    base = base_env if base_env and set(base_env) != {"0"} and git("cat-file", "-e", base_env + "^{commit}").returncode == 0 else None
    changed = None
    if base:
        r = git("diff", "--name-only", base, "HEAD")
        if r.returncode != 0:
            raise SystemExit("dispatch: git diff %s..HEAD failed: %s" % (base[:8], r.stderr.strip()[-200:]))
        changed = len([ln for ln in r.stdout.splitlines() if ln.strip()])
    return {"version": 1, "sha": sha, "branch": branch, "base": {"commit": base}, "changed": changed}


def matrix(sims):
    """The rows of this run: (lane, {step number: what runs there})."""
    rows = [("Every push (tree)", {1: "tree checks", 3: "dispatcher"})]
    for b in BITS:
        rows.append(("Bitstream " + b, {1: "lint", 2: "sims" if b in sims else "sims on demand", 3: "key", 6: "Vivado (builder)", 7: "report",
                                        11: "keep", 13: "to the lanes"}))
    for c in CORES:
        rows.append((c, {1: "lint (reverse lint)", 3: "changed", 6: "build (SDK release, libraries)",
                         11: "package (link, symbol check, .cor, ops and heap checks: 5 and 7 inside)", 12: "emulator (its own app)", 13: "deliver"}))
    return rows


def main(argv):
    if len(argv) != 4 or argv[0] != "--plan-out" or argv[2] != "--tier":
        print(__doc__, file=sys.stderr)
        return 2
    out, tier = argv[1], int(argv[3])
    branch = os.environ.get("CI_BRANCH") or os.environ.get("GITHUB_REF_NAME") or git("rev-parse", "--abbrev-ref", "HEAD").stdout.strip()
    p = plan(os.environ.get("BASE_SHA", ""), branch)
    with open(out, "w") as f:
        json.dump(p, f, indent=2)
    sims = list(BITS) if tier > 0 else []
    res = {"lint_groups": ["tree"] + ["bits:" + b for b in BITS] + ["core:" + c for c in CORES], "bits": BITS, "cores": CORES, "sims": sims}
    for k, v in res.items():
        print("%s=%s" % (k, json.dumps(v, separators=(",", ":"))))
    print("sims_tier=%d" % tier)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        lines = ["### The matrix of this push (U20 B, MultiCore T239.5)", "",
                 "Changed: %s files, branch %s, base %s" % (p["changed"] if p["changed"] is not None else "?", branch, (p["base"]["commit"] or "none")[:12]), "",
                 "| lane | " + " | ".join("%d %s" % (i + 1, s) for i, s in enumerate(STEPS)) + " |", "|---" * (len(STEPS) + 1) + "|"]
        for lane, cells in matrix(sims):
            lines.append("| %s | " % lane + " | ".join(cells.get(i + 1, "") for i in range(len(STEPS))) + " |")
        with open(summary, "a") as fh:
            fh.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
