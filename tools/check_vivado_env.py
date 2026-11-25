#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# PROVENANCE: TRX64U-MultiCore tools/check_vivado_env.py (t239.3 d9ddadeb1; Alex Damhuis' code in both repositories), U20 A7.
# Changed here: an option with an empty value in ci/vivado.env (our default build) must be absent or empty in a workflow, and a workflow that sets a
# build option ci/vivado.env does not name is a difference too; the default workflow is our Gitea bitstream stage.
"""U20 A7: ci/vivado.env is the one place for the bitstream build options (ci/keys.sh hashes them, the builder exports them). While the Gitea
pipeline builds the bitstream itself (.gitea/workflows/bitstream.yml), its env must give Vivado the same options, else the Gitea cache and the
builder's releases hold different bitstreams under one key. This check reads the env of every job and step of the workflow and is red when
  - an option with a value in ci/vivado.env is missing from the workflow or has another value there,
  - an option that is empty in ci/vivado.env (the default build) has a non-empty value in the workflow,
  - the workflow sets a build option (TRX_*, T79_*, T16_*) that ci/vivado.env does not name.
A workflow file that does not exist passes (that copy is gone).

  tools/check_vivado_env.py [--env FILE] [--workflow FILE]...   (default: .gitea/workflows/bitstream.yml)
  tools/check_vivado_env.py --selftest"""
import os
import re
import sys
import tempfile

import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OPTION = re.compile(r"^(TRX|T79|T16)_[A-Z0-9_]+$")


def read_env(path):
    out = {}
    for ln in open(path):
        ln = ln.strip()
        if ln and not ln.startswith("#"):
            k, _, v = ln.partition("=")
            out[k] = v
    return out


def workflow_envs(path):
    """Every env mapping of every job and step of the workflow, merged: key -> set of values (as strings)."""
    seen = {}
    for job in ((yaml.safe_load(open(path)) or {}).get("jobs") or {}).values():
        for env in [job.get("env")] + [s.get("env") for s in job.get("steps") or []]:
            for k, v in (env or {}).items():
                seen.setdefault(k, set()).add("" if v is None else str(v))
    return seen


def check(env_file, workflows):
    if isinstance(workflows, str):
        workflows = [workflows]
    want, bad = read_env(env_file), []
    for workflow in workflows:
        if not os.path.exists(workflow):
            continue
        name, have = os.path.basename(workflow), workflow_envs(workflow)
        for k, v in want.items():
            if v and have.get(k) != {v}:
                bad.append("%s: %s: %s in the workflow, %s in ci/vivado.env" % (name, k, sorted(have[k]) if k in have else "missing", v))
            elif not v and k in have and have[k] != {""}:
                bad.append("%s: %s: %s in the workflow, empty (the default build) in ci/vivado.env" % (name, k, sorted(have[k])))
        for k in sorted(have):
            if OPTION.match(k) and k not in want:
                bad.append("%s: %s: set in the workflow, not named in ci/vivado.env" % (name, k))
    return bad


def selftest():
    errs = []
    with tempfile.TemporaryDirectory() as td:
        e, w = os.path.join(td, "v.env"), os.path.join(td, "w.yml")

        def wf(env):
            body = "".join("          %s: \"%s\"\n" % kv for kv in env.items())
            open(w, "w").write("jobs:\n  j:\n    steps:\n      - run: x\n" + ("        env:\n" + body if env else ""))
        open(e, "w").write("# c\nTRX_A=x\nTRX_B=\n")
        cases = [({"TRX_A": "x"}, False, "value equal, empty option absent"),
                 ({"TRX_A": "x", "TRX_B": ""}, False, "empty option empty"),
                 ({"TRX_A": "y"}, True, "changed value"),
                 ({}, True, "missing value"),
                 ({"TRX_A": "x", "TRX_B": "1"}, True, "empty option set"),
                 ({"TRX_A": "x", "T16_C": "0"}, True, "option not in ci/vivado.env"),
                 ({"TRX_A": "x", "PART": "50t"}, False, "a variable that is no build option")]
        for env, red, label in cases:
            wf(env)
            if bool(check(e, w)) != red:
                errs.append("%s: %s" % (label, "passed" if red else "refused: %s" % check(e, w)))
        if check(e, os.path.join(td, "none.yml")):
            errs.append("absent workflow refused")
    print("selftest FAILED: " + "; ".join(errs) if errs else "selftest ok")
    return 1 if errs else 0


if __name__ == "__main__":
    a = sys.argv[1:]
    if a == ["--selftest"]:
        sys.exit(selftest())
    opt = dict(zip(a[::2], a[1::2]))
    wfs = [v for k, v in zip(a[::2], a[1::2]) if k == "--workflow"] or [os.path.join(ROOT, ".gitea/workflows/bitstream.yml")]
    bad = check(opt.get("--env", os.path.join(ROOT, "ci/vivado.env")), wfs)
    for b in bad:
        print("VIVADO-ENV: " + b)
    print("vivado env copies: %s" % ("%d differences" % len(bad) if bad else "agree"))
    sys.exit(1 if bad else 0)
