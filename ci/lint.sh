#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Stage `lint`: cheap checks that fail before a bench or Vivado runs, in trx64-sim-env (Gitea) or MultiCore's trx64u-sim image (GitHub).
# Every check runs; the stage fails if one did.
#
#   ci/lint.sh [GROUP]     no GROUP: every check (the Gitea stage); GROUP: the checks of one lane of the GitHub matrix
#                          (U20 B, MultiCore T239.5 D7): tree, bits:c64_basic, core:trx64u
#
# tree:
#  - shell: bash -n and shellcheck (errors only) over ci/, tools/, vivado/ and the benches
#  - python: byte-compile and ruff's syntax/undefined-name rules (E9, F63, F7, F82)
#  - the workflows: parse, every `needs:` names a job, every `uses:` a file (.gitea and .github); tools/check_workflows.py over .github
#    (pinned actions and images, no self-hosted runner, permissions, the pre-release job's guard, the jobs' outputs)
#  - ci/select.sh: its answers for the cases the workflows rely on
#  - ci/vivado.env against the Gitea bitstream stage (tools/check_vivado_env.py); ci/keys.sh against gateware_key of ci/lib.sh
#  - the scripts that read and write the builds repository, against a fake API on 127.0.0.1 (ci/test_builds.py)
#  - ci/roms.py: its unit test (no network) and the pins of ci/roms.txt
# bits:c64_basic:
#  - the RTL (designs/*/*.v): syntax only (yosys -defer), no elaboration; the VHDL and the benches are analysed by the benches of stage sims
#  - ci/sims.txt: every bench's script exists
# core:trx64u:
#  - software/trx64u (U18): the pinned SDK's reverse lint (rtl_check.py lint-src) on our libraries' sources and uci/; the SDK from SDK_FETCH
#    (fetch_sdk.sh: --download, the default, our Gitea registry; --release, the builds repository, GitHub's runners)
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
cd "$ROOT"

# hand-over directories of runs that never reached their end (failed stages)
[ ! -d /cache/runs ] || find /cache/runs -mindepth 1 -maxdepth 1 -mtime +2 -exec rm -rf {} + || true

group="${1:-}"
case "$group" in ''|tree|bits:c64_basic|core:trx64u) ;; *) echo "ci/lint.sh: no group $group (tree, bits:c64_basic, core:trx64u)" >&2; exit 2 ;; esac
fails=()
# group_has <group>: true when that group was asked for, or none was
group_has() { [ -z "$group" ] || [ "$group" = "$1" ]; }
check() { # check <name> <command...>
  local name="$1"; shift
  if "$@"; then echo "ok   $name"; else echo "FAIL $name"; fails+=("$name"); fi
}
# ci/keys.sh (the builder's names, U20 A7) prints exactly the gateware key the Gitea bitstream stage computes for the same part
keys_agree() {
  local part k
  part="$(sed -n 's/^  c64u) part=\([^;]*\);.*/\1/p' ci/bitstream.sh)"
  k="$(ci/keys.sh)" || return 1
  echo "    $k (part $part)"
  [ "$k" = "c64u-$(env -u TRX_SID -u T16_SLOT -u TRX_FEATURES bash -c '. ci/lib.sh; gateware_key "$1"' _ "$part")" ]
}

if group_has tree; then
check "bash -n" bash -c 'for f in $(git ls-files -- "ci/*.sh" "tools/*.sh" "vivado/*.sh" "designs/*/sim/*/*.sh"); do bash -n "$f" || exit 1; done'
check "shellcheck" bash -c 'git ls-files -- "ci/*.sh" "tools/*.sh" "vivado/*.sh" "designs/*/sim/*/*.sh" | xargs shellcheck -S error'

check "python compile" bash -c 'git ls-files -- "*.py" | xargs python3 -m py_compile'
check "ruff" bash -c 'git ls-files -- "*.py" | xargs ruff check --no-cache --select E9,F63,F7,F82'

check "select.sh" bash -c '
  t() { [ "$(env -i HOME="$HOME" PATH="$PATH" CI_MSG= "${@:2}" ci/select.sh $1)" = "$EXPECT" ]; }
  EXPECT=no  t ue2 GITHUB_REF_NAME=main GITHUB_EVENT_NAME=push &&
  EXPECT=no  t ue2 GITHUB_REF_NAME=main GITHUB_EVENT_NAME=workflow_dispatch UE2_INPUT=false &&
  EXPECT=yes t ue2 GITHUB_REF_NAME=main GITHUB_EVENT_NAME=workflow_dispatch UE2_INPUT=true &&
  EXPECT=yes t ue2 GITHUB_REF_NAME=main GITHUB_EVENT_NAME=workflow_call UE2_INPUT=true &&
  EXPECT=no  t ue2 GITHUB_REF_NAME=main GITHUB_EVENT_NAME=workflow_call UE2_INPUT= &&
  EXPECT=yes t ue2 GITHUB_REF_NAME=v1.0 GITHUB_REF_TYPE=tag GITHUB_EVENT_NAME=push &&
  EXPECT=yes t ue2 GITHUB_REF_NAME=release/1 GITHUB_EVENT_NAME=push &&
  EXPECT=1   t sims-tier GITHUB_REF_NAME=t40 GITHUB_EVENT_NAME=push &&
  EXPECT=2   t sims-tier GITHUB_REF_NAME=main GITHUB_EVENT_NAME=push &&
  EXPECT=2   t sims-tier GITHUB_REF_NAME=integ-m1 GITHUB_EVENT_NAME=push &&
  EXPECT=2   t sims-tier GITHUB_REF_NAME=t40 GITHUB_EVENT_NAME=push "CI_MSG=x [sims]" &&
  EXPECT=0   t sims-demand GITHUB_REF_NAME=main GITHUB_EVENT_NAME=push &&
  EXPECT=0   t sims-demand GITHUB_REF_NAME=t40 GITHUB_EVENT_NAME=push &&
  EXPECT=2   t sims-demand GITHUB_REF_NAME=integration GITHUB_EVENT_NAME=push &&
  EXPECT=2   t sims-demand GITHUB_REF_NAME=v0.4.0 GITHUB_REF_TYPE=tag GITHUB_EVENT_NAME=push &&
  EXPECT=2   t sims-demand CI_BRANCH=pr-7 GITHUB_REF_NAME=7/merge SIMS_INPUT=true &&
  EXPECT=0   t sims-demand CI_BRANCH=pr-7 GITHUB_REF_NAME=7/merge SIMS_INPUT=false &&
  EXPECT=2   t sims-demand GITHUB_REF_NAME=t40 GITHUB_EVENT_NAME=push "CI_MSG=x [sims]"'
check "workflow" python3 - <<'PY'
import glob, os, sys, yaml
ok = True
for p in glob.glob(".gitea/workflows/*.yml") + glob.glob(".github/workflows/*.yml"):
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
check "workflow lint (.github, U20 B)" bash -c 'python3 tools/check_workflows.py --selftest && python3 tools/check_workflows.py'
check "vivado env (U20 A7)" bash -c 'python3 tools/check_vivado_env.py --selftest && python3 tools/check_vivado_env.py'
check "keys (U20 A7)" keys_agree
check "builds repository scripts (U20 B)" python3 -I ci/test_builds.py
check "bench ROMs fetch test (U20 A6)" python3 -I ci/test_roms.py
fi

if group_has bits:c64_basic; then
check "verilog syntax" bash -c '
  rc=0
  for f in $(git ls-files | grep -E "^designs/[^/]+/[^/]+\.v$"); do
    yosys -q -p "read_verilog -sv -defer -I cores/icepi -I cores/icepi/vicii-kawari/hdl $f" >/tmp/lint.log 2>&1 || { echo "$f"; tail -5 /tmp/lint.log; rc=1; }
  done; exit $rc'
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
fi

# U18 (MultiCore T239.3 P1-3, D30 condition 1): no literal I/O address in the sources of our three libraries and of our UCI library (uci/, linked into
# the app); every address comes from c64_basic's TOML A through the driver table. The pinned SDK's rtl_check.py, from SDK_FETCH (above). The one --allow
# is SHA-256's K[47], which lies in the I/O space by chance. rtl_check.py check (the TOML A against the RTL) is not here
# yet: the tool of SDK 0.4.6 holds every block of it but one property, cmd_if's command_start, whose decoded read drives the constant 0 and which it
# takes for no read: MultiCore's tool fault, fixed in a later SDK (U18 §5).
if group_has core:trx64u; then
check "reverse lint (software/trx64u)" bash -c '
  T="$PWD/software/trx64u"
  [ -f "$T/sdk.pin" ] || exit 0
  GITEA_URL="${GITHUB_SERVER_URL:-http://gitea.example}" sh "$T/fetch_sdk.sh" "${SDK_FETCH:---download}" >/tmp/lint-sdk.log 2>&1 || { cat /tmp/lint-sdk.log; exit 1; }
  S="$T/.sdk/trx64-sdk-$(sed -n "s/^sdk_version=//p" "$T/sdk.pin")"
  cd "$T" && python3 -I "$S/tools/rtl_check.py" lint-src base ring monitor uci --toml-a gateware/c64_basic.toml --maps "$S/maps" \
      --allow trx_sha256.c:0x106aa070u'
fi

if [ ${#fails[@]} -gt 0 ]; then echo "FAILED: ${fails[*]}"; exit 1; fi
echo "=== lint${group:+ $group} done in $(lap)"
