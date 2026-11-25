#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# PROVENANCE: TRX64U-MultiCore tools/check_workflows.py (t239.3 d9ddadeb1; Alex Damhuis' code in both repositories), U20 B.
# Changed here: rule 2 names our refs (main, v* tags); rule 6 accepts only GitHub-hosted images and one listed exception (U20 F, ephemeral-a.yml's job
# `work`, whose runs-on is the run's random label: MultiCore's word test for `self-hosted` did not see it, U20 §6 F open point 4); rule 7 names our
# private repository and, through tools/check_public_tree.py's text rules (U20 A8), every internal host, address and path; rule 8 is new (the references to needs and steps outputs; actionlint is not in our images).
"""U20 B (MultiCore T239.1 D1, S9): the rules a workflow of .github/ must keep, read from the parsed YAML (and, for the names, the raw text).

  1. no pull_request_target, workflow_run or issue_comment trigger in a workflow that names a secret (secrets.X, secrets: inherit) or an environment
  2. environment `builds` only in a job called `prerelease` whose `if:` demands a push of main or a v* tag and whose checkout takes the run's own commit
  3. top-level `permissions: contents: read` (the only key)
  4. every `uses:` is a local path (./) or pinned by a full 40-hex commit SHA; docker:// needs a digest
  5. every container / service image carries @sha256:<64 hex>
  6. every `runs-on` is a GitHub-hosted image (ubuntu-*, windows-*, macos-*); never self-hosted; the one exception is listed in EXCEPTIONS
  7. no name of a private repository (the builder's) and no hit of tools/check_public_tree.py's text rules (internal hosts, private
     addresses, home and NAS paths, logins, personal mail addresses, claude.ai links) anywhere under .github/
  8. every needs.<job>.outputs.<name> names a job of the job's needs that declares that output (a called workflow's outputs are not checked), every
     steps.<id> names a step of the same job

  tools/check_workflows.py [DIR]      (default: .github of the checkout)
  tools/check_workflows.py --selftest"""
import os
import re
import sys
import tempfile

import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import check_public_tree  # noqa: E402  (rule 7: the text rules of the public-tree check, one source)
RISKY = ("pull_request_target", "workflow_run", "issue_comment")
PRIVATE_NAMES = re.compile(r"trx64mc-builder\b", re.I)
SHA40 = re.compile(r"^[0-9a-f]{40}$")
DIGEST = re.compile(r"@sha256:[0-9a-f]{64}\b")
HOSTED = re.compile(r"^(ubuntu|windows|macos)-[0-9a-z.-]+$")
# (workflow file, job): a runs-on that is no GitHub-hosted image, with why. U20 F-a: the job runs on the just-in-time runner of the run's server, whose
# only label is the random one job `start` made (no `self-hosted`, so a fork's queued job cannot take that runner).
EXCEPTIONS = {("ephemeral-a.yml", "work"): "${{ needs.start.outputs.label }}"}
NEEDS_OUT = re.compile(r"needs\.([A-Za-z0-9_-]+)\.outputs\.([A-Za-z0-9_-]+)")
STEPS_REF = re.compile(r"steps\.([A-Za-z0-9_-]+)\.")


def triggers(doc):
    on = doc.get("on", doc.get(True))  # YAML 1.1 reads the key `on` as True
    if isinstance(on, str):
        return [on]
    return list(on or [])


def walk(node):
    if isinstance(node, dict):
        for k, v in node.items():
            yield k, v
            yield from walk(v)
    elif isinstance(node, list):
        for v in node:
            yield from walk(v)


def strings(node):
    if isinstance(node, str):
        yield node
    elif isinstance(node, dict):
        for v in node.values():
            yield from strings(v)
    elif isinstance(node, list):
        for v in node:
            yield from strings(v)


def names_secret_or_env(doc, text):
    if re.search(r"\bsecrets\s*\.|\bsecrets\s*\[", text):
        return True
    for k, v in walk(doc.get("jobs") or {}):
        if k == "environment" or (k == "secrets" and v):
            return True
    return False


def images_of(job):
    c = job.get("container")
    if isinstance(c, str):
        yield c
    elif isinstance(c, dict) and "image" in c:
        yield str(c["image"])
    for s in (job.get("services") or {}).values():
        if isinstance(s, dict) and "image" in s:
            yield str(s["image"])
        elif isinstance(s, str):
            yield s


def env_name(job):
    e = job.get("environment")
    return e.get("name") if isinstance(e, dict) else e


def prerelease_guard(jid, job):
    bad = []
    cond = str(job.get("if", ""))
    if jid != "prerelease":
        bad.append("environment builds in job %s, only job prerelease may use it" % jid)
    if "github.event_name == 'push'" not in cond:
        bad.append("job %s: if: does not demand github.event_name == 'push'" % jid)
    for ref in ("refs/heads/main", "refs/tags/v"):
        if ref not in cond:
            bad.append("job %s: if: does not name %s" % (jid, ref))
    if "pull_request" in cond or "head_ref" in cond:
        bad.append("job %s: if: lets a pull request in" % jid)
    ok = False
    for st in job.get("steps") or []:
        if str(st.get("uses", "")).startswith("actions/checkout@"):
            ok = str((st.get("with") or {}).get("ref", "")).replace(" ", "") == "${{github.sha}}"
    if not ok:
        bad.append("job %s: the checkout does not take ref: ${{ github.sha }}" % jid)
    return bad


def runs_on_bad(name, jid, ro):
    if ro is None:
        return []
    if "self-hosted" in str(ro).lower():
        return ["%s: job %s runs-on self-hosted" % (name, jid)]
    labels = ro if isinstance(ro, list) else [ro]
    if (name, jid) in EXCEPTIONS:
        return [] if labels == [EXCEPTIONS[(name, jid)]] else ["%s: job %s: runs-on %r is not its listed exception" % (name, jid, ro)]
    return ["%s: job %s: runs-on %r is no GitHub-hosted image" % (name, jid, lab) for lab in labels if not HOSTED.match(str(lab))]


def refs_bad(name, jobs):
    bad = []
    for jid, job in jobs.items():
        if not isinstance(job, dict):
            continue
        needs = job.get("needs") or []
        needs = [needs] if isinstance(needs, str) else needs
        step_ids = {s.get("id") for s in job.get("steps") or [] if isinstance(s, dict)}
        for text in strings(job):
            for other, out in NEEDS_OUT.findall(text):
                if other not in needs:
                    bad.append("%s: job %s: needs.%s.outputs.%s, but %s is not in its needs" % (name, jid, other, out, other))
                elif isinstance(jobs.get(other), dict) and "uses" not in jobs[other] and out not in (jobs[other].get("outputs") or {}):
                    bad.append("%s: job %s: job %s declares no output %s" % (name, jid, other, out))
        for st in job.get("steps") or []:
            for text in strings(st):
                for sid in STEPS_REF.findall(text):
                    if sid not in step_ids:
                        bad.append("%s: job %s: steps.%s names no step of the job" % (name, jid, sid))
        for k, v in (job.get("outputs") or {}).items():
            for sid in STEPS_REF.findall(str(v)):
                if sid not in step_ids:
                    bad.append("%s: job %s: output %s reads steps.%s, no step of the job" % (name, jid, k, sid))
    return bad


def check_file(path):
    name, bad = os.path.basename(path), []
    text = open(path).read()
    doc = yaml.safe_load(text) or {}
    if not isinstance(doc, dict):
        return ["%s: not a mapping" % name]
    risky = [t for t in triggers(doc) if t in RISKY]
    if risky and names_secret_or_env(doc, text):
        bad.append("%s: trigger %s with a secret or an environment" % (name, ", ".join(risky)))
    if doc.get("permissions") != {"contents": "read"}:
        bad.append("%s: no top-level permissions: contents: read (alone)" % name)
    jobs = doc.get("jobs") or {}
    for jid, job in jobs.items():
        if not isinstance(job, dict):
            continue
        if env_name(job) == "builds":
            bad += ["%s: %s" % (name, b) for b in prerelease_guard(jid, job)]
        bad += runs_on_bad(name, jid, job.get("runs-on"))
        for img in images_of(job):
            if not DIGEST.search(img):
                bad.append("%s: job %s: image %s has no @sha256 digest" % (name, jid, img))
        uses = [job["uses"]] if "uses" in job else []
        uses += [s["uses"] for s in job.get("steps") or [] if "uses" in s]
        for u in map(str, uses):
            if u.startswith("./"):
                continue
            if u.startswith("docker://"):
                if not DIGEST.search(u):
                    bad.append("%s: job %s: %s has no digest" % (name, jid, u))
            elif not SHA40.match(u.partition("@")[2]):
                bad.append("%s: job %s: %s is not pinned by a full commit SHA" % (name, jid, u))
    bad += refs_bad(name, jobs)
    return bad


def check_dir(d):
    bad = []
    for base, _, files in os.walk(d):
        for f in sorted(files):
            p = os.path.join(base, f)
            if f.endswith((".yml", ".yaml")) and os.path.basename(base) == "workflows":
                bad += check_file(p)
            data = open(p, "rb").read()
            m = PRIVATE_NAMES.search(data.decode(errors="replace"))
            if m:
                bad.append("%s: names a private repository (%s)" % (os.path.relpath(p, d), m.group(0)))
            bad += ["%s: %s (tools/check_public_tree.py)" % (os.path.relpath(p, d), h) for h in check_public_tree.text_hits(data)]
    return bad


GOOD = """name: t
on:
  push:
    branches: [main]
permissions:
  contents: read
jobs:
  a:
    runs-on: ubuntu-latest
    container:
      image: ghcr.io/x/y@sha256:%s
    outputs:
      o: ${{ steps.s.outputs.o }}
    steps:
      - uses: actions/checkout@%s
      - id: s
        run: echo o=1 >> "$GITHUB_OUTPUT"
  b:
    needs: a
    runs-on: ubuntu-24.04
    steps:
      - run: echo ${{ needs.a.outputs.o }}
  prerelease:
    runs-on: ubuntu-latest
    environment: builds
    if: ${{ github.event_name == 'push' && (github.ref == 'refs/heads/main' || startsWith(github.ref, 'refs/tags/v')) }}
    steps:
      - uses: actions/checkout@%s
        with:
          ref: ${{ github.sha }}
""" % ("a" * 64, "b" * 40, "b" * 40)


def selftest():
    faults = {
        "risky trigger + secret": lambda s: s.replace("on:\n  push:\n    branches: [main]", "on:\n  pull_request_target:").replace("      - id: s\n", "      - run: echo ${{ secrets.X }}\n      - id: s\n"),
        "risky trigger + environment": lambda s: s.replace("on:\n  push:\n    branches: [main]", "on:\n  workflow_run:\n    workflows: [x]"),
        "builds outside prerelease": lambda s: s.replace("  a:\n    runs-on: ubuntu-latest", "  a:\n    environment: builds\n    runs-on: ubuntu-latest"),
        "builds without push": lambda s: s.replace("github.event_name == 'push' && ", ""),
        "builds without tags": lambda s: s.replace(" || startsWith(github.ref, 'refs/tags/v')", ""),
        "builds checkout not own commit": lambda s: s.replace("ref: ${{ github.sha }}", "ref: ${{ github.head_ref }}"),
        "no permissions": lambda s: s.replace("permissions:\n  contents: read\n", ""),
        "permissions write": lambda s: s.replace("contents: read", "contents: write"),
        "permissions extra key": lambda s: s.replace("contents: read", "contents: read\n  packages: write"),
        "action by tag": lambda s: s.replace("actions/checkout@" + "b" * 40, "actions/checkout@v4", 1),
        "docker uses without digest": lambda s: s.replace("      - id: s\n", "      - uses: docker://alpine:3\n      - id: s\n"),
        "image without digest": lambda s: s.replace("@sha256:" + "a" * 64, ":latest"),
        "image short digest": lambda s: s.replace("a" * 64, "a" * 63),
        "self-hosted": lambda s: s.replace("  a:\n    runs-on: ubuntu-latest", "  a:\n    runs-on: [self-hosted, linux]"),
        "runs-on a custom label": lambda s: s.replace("  a:\n    runs-on: ubuntu-latest", "  a:\n    runs-on: [eph-1]"),
        "runs-on an expression": lambda s: s.replace("  a:\n    runs-on: ubuntu-latest", "  a:\n    runs-on: ${{ inputs.r }}"),
        "builder repository name": lambda s: s + "# trx64mc-builder\n",
        "internal host": lambda s: s + "# http://devhost.lan\n",
        "private address": lambda s: s.replace("echo o=1", "curl http://10.0.0.5/; echo o=1"),
        "needs output of a job not needed": lambda s: s.replace("    needs: a\n", "    needs: prerelease\n"),
        "needs output not declared": lambda s: s.replace("needs.a.outputs.o", "needs.a.outputs.p"),
        "steps ref to no step": lambda s: s.replace("${{ steps.s.outputs.o }}", "${{ steps.t.outputs.o }}"),
    }
    errs = []
    with tempfile.TemporaryDirectory() as td:
        wd = os.path.join(td, "workflows")
        os.makedirs(wd)
        w = os.path.join(wd, "t.yml")
        open(w, "w").write(GOOD)
        if check_dir(td):
            errs.append("the good workflow is refused: %s" % check_dir(td))
        for label, f in faults.items():
            src = f(GOOD)
            if src == GOOD:
                errs.append("%s: the plant changed nothing" % label)
                continue
            open(w, "w").write(src)
            if not check_dir(td):
                errs.append("%s: passed" % label)
        os.unlink(w)
        e = os.path.join(wd, "ephemeral-a.yml")
        ex = GOOD.replace("  b:\n    needs: a\n    runs-on: ubuntu-24.04", "  work:\n    needs: a\n    runs-on: ['${{ needs.a.outputs.o }}']")
        open(e, "w").write(ex.replace("needs.a.outputs.o }}']", "needs.start.outputs.label }}']").replace("    needs: a\n    runs-on: ['$", "    needs: [a, start]\n    runs-on: ['$")
                           + "  start:\n    runs-on: ubuntu-latest\n    outputs:\n      label: x\n    steps:\n      - run: x\n")
        if check_dir(td):
            errs.append("the listed exception is refused: %s" % check_dir(td))
        open(e, "w").write(ex.replace("needs.a.outputs.o }}']", "needs.a.outputs.o }}', self-hosted]"))
        if not check_dir(td):
            errs.append("the exception with self-hosted passed")
        os.unlink(e)
        open(w, "w").write(GOOD)
        open(os.path.join(td, "notes.md"), "w").write("see trx64mc-builder\n")
        if not check_dir(td):
            errs.append("private name in a non-workflow file passed")
    for e in errs:
        print("selftest FAIL:", e)
    if not errs:
        print("selftest ok (%d faults planted, one rule each; the clean file and the listed exception pass)" % (len(faults) + 2))
    return 1 if errs else 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    d = next((a for a in sys.argv[1:] if not a.startswith("-")), os.path.join(ROOT, ".github"))
    bad = check_dir(d)
    for b in bad:
        print("FAIL", b)
    print("workflows: %s" % ("%d findings" % len(bad) if bad else "ok"))
    sys.exit(1 if bad else 0)
