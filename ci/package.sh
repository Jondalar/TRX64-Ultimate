#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Stage `package`: what the earlier stages made, as the generic package
# trx64-ultimate in the Gitea registry, version <branch>-<8 digit commit> and
# <branch>-latest (the same versions and file names as the single job used:
# <design>.bit, <design>.timing.rpt, <design>.utilization*.rpt; tools/load.sh
# reads <design>.bit), manifest.json and SHA256SUMS over all of it. Stage `cor`
# adds trx64u_<commit8>.cor to the same versions. The Gideon app (ultimate.app,
# ultimate.elf, the manifest's "version" and "app_commit") left with the
# submodule (U20 A5, D13): the Gitea branch gideon-app packs them.
#
# The 100T bitstream is not part of this: stage bit-ue2 added its own files (_100t)
# to the same package versions; that stage is commented out since 2026-10-16.
#
#   PACKAGE_TOKEN=... ci/package.sh
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

out="$ROOT/out"
rm -rf "$out"; mkdir -p "$out"

c64u="$(need_stage bit-c64u)"
cp "$c64u"/*.bit "$c64u"/*.rpt "$c64u"/*.gz "$c64u"/*.steps.txt "$out/"
cp "$c64u/wns.txt" "$out/wns.c64u.txt"

sha="$(git -C "$ROOT" rev-parse --short=8 HEAD)"
BITDIR="$c64u" python3 - "$out" "$sha" "$(git -C "$ROOT" rev-parse HEAD)" "$(branch_slug)" <<'PY'
import hashlib, json, os, sys
out, short, full, branch = sys.argv[1:]
def wns(name):
    p = os.path.join(out, name)
    return dict(l.split() for l in open(p)) if os.path.exists(p) else {}
files = {}
for f in sorted(os.listdir(out)):
    if f.endswith(".txt") and f.startswith("wns."):
        continue
    data = open(os.path.join(out, f), "rb").read()
    files[f] = {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
json.dump({"trx64_commit": full, "branch": branch,
           "gateware_key": open(os.path.join(os.environ["BITDIR"], "key")).read().strip(),
           "bitstream_from": open(os.path.join(os.environ["BITDIR"], "from")).read().strip(),
           "wns_ns": wns("wns.c64u.txt"),
           "files": files}, open(os.path.join(out, "manifest.json"), "w"), indent=2)
PY
rm -f "$out"/wns.*.txt
(cd "$out" && sha256sum $(ls | grep -v '^SHA256SUMS$') > SHA256SUMS)
ls -l "$out"
cat "$out/manifest.json"

publish_files "$out" reset

# the run's hand-over directory goes (bit-ue2, which reused it, is off since 2026-10-16)
rm -rf "${HANDOFF:?}"
echo "=== package done in $(lap)"
