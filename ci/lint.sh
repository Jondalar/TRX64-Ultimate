#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Stage `lint`: cheap checks that fail before a bench or Vivado runs, in
# trx64-sim-env. Every check runs; the stage fails if one did.
#
#   ci/lint.sh
#
#  - shell: bash -n and shellcheck (errors only) over ci/, tools/, vivado/ and the benches
#  - python: byte-compile and ruff's syntax/undefined-name rules (E9, F63, F7, F82)
#  - the RTL (designs/*/*.v): syntax only (yosys -defer), no elaboration; the VHDL and the
#    benches are analysed by the benches of stage sims
#  - the workflows: parse, every `needs:` names a job, every `uses:` a file
#  - ci/select.sh: its answers for the cases the workflows rely on
#  - ci/sims.txt: every bench's script exists
#  - software/trx64u (U18): the pinned SDK's reverse lint (rtl_check.py lint-src) on our libraries' sources and uci/
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
cd "$ROOT"

# hand-over directories of runs that never reached their end (failed stages)
[ ! -d /cache/runs ] || find /cache/runs -mindepth 1 -maxdepth 1 -mtime +2 -exec rm -rf {} + || true

fails=()
check() { # check <name> <command...>
  local name="$1"; shift
  if "$@"; then echo "ok   $name"; else echo "FAIL $name"; fails+=("$name"); fi
}
check "bash -n" bash -c 'for f in $(git ls-files -- "ci/*.sh" "tools/*.sh" "vivado/*.sh" "designs/*/sim/*/*.sh"); do bash -n "$f" || exit 1; done'
check "shellcheck" bash -c 'git ls-files -- "ci/*.sh" "tools/*.sh" "vivado/*.sh" "designs/*/sim/*/*.sh" | xargs shellcheck -S error'

check "python compile" bash -c 'git ls-files -- "*.py" | xargs python3 -m py_compile'
check "ruff" bash -c 'git ls-files -- "*.py" | xargs ruff check --no-cache --select E9,F63,F7,F82'

check "verilog syntax" bash -c '
  rc=0
  for f in $(git ls-files | grep -E "^designs/[^/]+/[^/]+\.v$"); do
    yosys -q -p "read_verilog -sv -defer -I cores/icepi -I cores/icepi/vicii-kawari/hdl $f" >/tmp/lint.log 2>&1 || { echo "$f"; tail -5 /tmp/lint.log; rc=1; }
  done; exit $rc'

check "select.sh" bash -c '
  t() { [ "$(env -i HOME="$HOME" PATH="$PATH" CI_MSG= "${@:2}" ci/select.sh $1)" = "$EXPECT" ]; }
  EXPECT=no  t ue2 GITHUB_REF_NAME=main GITHUB_EVENT_NAME=push &&
  EXPECT=no  t ue2 GITHUB_REF_NAME=main GITHUB_EVENT_NAME=workflow_dispatch UE2_INPUT=false &&
  EXPECT=yes t ue2 GITHUB_REF_NAME=main GITHUB_EVENT_NAME=workflow_dispatch UE2_INPUT=true &&
  EXPECT=yes t ue2 GITHUB_REF_NAME=main GITHUB_EVENT_NAME=workflow_call UE2_INPUT=true &&
  EXPECT=no  t ue2 GITHUB_REF_NAME=main GITHUB_EVENT_NAME=workflow_call UE2_INPUT= &&
  EXPECT=yes t ue2 GITHUB_REF_NAME=v1.0 GITHUB_REF_TYPE=tag GITHUB_EVENT_NAME=push &&
  EXPECT=yes t ue2 GITHUB_REF_NAME=release/1 GITHUB_EVENT_NAME=push &&
  EXPECT=yes t update GITHUB_REF_NAME=integ-m1 &&
  EXPECT=yes t update GITHUB_REF_NAME=main &&
  EXPECT=yes t update GITHUB_REF_NAME=v1.0 GITHUB_REF_TYPE=tag &&
  EXPECT=no  t update GITHUB_REF_NAME=t40 &&
  EXPECT=yes t update GITHUB_REF_NAME=t40 UPDATE_FORCE=true &&
  EXPECT=1   t sims-tier GITHUB_REF_NAME=t40 GITHUB_EVENT_NAME=push &&
  EXPECT=2   t sims-tier GITHUB_REF_NAME=main GITHUB_EVENT_NAME=push &&
  EXPECT=2   t sims-tier GITHUB_REF_NAME=integ-m1 GITHUB_EVENT_NAME=push &&
  EXPECT=2   t sims-tier GITHUB_REF_NAME=t40 GITHUB_EVENT_NAME=push "CI_MSG=x [sims]"'
check "workflow" python3 - <<'PY'
import glob, os, sys, yaml
ok = True
for p in glob.glob(".gitea/workflows/*.yml"):
    jobs = yaml.safe_load(open(p))["jobs"]
    for name, j in jobs.items():
        needs = j.get("needs", [])
        for n in [needs] if isinstance(needs, str) else needs:
            if n not in jobs:
                print(f"{p}: job {name} needs {n}, which does not exist"); ok = False
        if "uses" in j and not os.path.exists(j["uses"]):
            print(f"{p}: job {name} uses {j['uses']}, which does not exist"); ok = False
sys.exit(0 if ok else 1)
PY
check "sims.txt" python3 - <<'PY'
import os, re, sys
ok = True
names = set()
for n, line in enumerate(open("ci/sims.txt"), 1):
    if not line.strip() or line.startswith("#"):
        continue
    f = line.rstrip("\n").split("|", 3)
    if len(f) != 4 or f[0] not in "12" or not f[2].isdigit():
        print(f"ci/sims.txt:{n}: want tier|name|timeout|command"); ok = False; continue
    if f[1] in names:
        print(f"ci/sims.txt:{n}: {f[1]} twice"); ok = False
    names.add(f[1])
    for path in re.findall(r"designs/[\w./-]+", f[3]):
        if not os.path.exists(path):
            print(f"ci/sims.txt:{n}: {path} does not exist"); ok = False
sys.exit(0 if ok else 1)
PY

# U18 (MultiCore T239.3 P1-3, D30 condition 1): no literal I/O address in the sources of our three libraries and of our UCI library (uci/, linked into
# the app); every address comes from c64_basic's TOML A through the driver table. The pinned SDK's rtl_check.py, from our registry (fetch_sdk.sh
# --download). The one --allow is SHA-256's K[47], which lies in the I/O space by chance. rtl_check.py check (the TOML A against the RTL) is not here
# yet: the tool of SDK 0.4.6 holds every block of it but one property, cmd_if's command_start, whose decoded read drives the constant 0 and which it
# takes for no read: MultiCore's tool fault, fixed in a later SDK (U18 §5).
check "reverse lint (software/trx64u)" bash -c '
  T="$PWD/software/trx64u"
  [ -f "$T/sdk.pin" ] || exit 0
  GITEA_URL="${GITHUB_SERVER_URL:-http://gitea.example}" sh "$T/fetch_sdk.sh" --download >/tmp/lint-sdk.log 2>&1 || { cat /tmp/lint-sdk.log; exit 1; }
  S="$T/.sdk/trx64-sdk-$(sed -n "s/^sdk_version=//p" "$T/sdk.pin")"
  cd "$T" && python3 -I "$S/tools/rtl_check.py" lint-src base ring monitor uci --toml-a gateware/c64_basic.toml --maps "$S/maps" \
      --allow trx_sha256.c:0x106aa070u'

if [ ${#fails[@]} -gt 0 ]; then echo "FAILED: ${fails[*]}"; exit 1; fi
echo "=== lint done in $(lap)"
