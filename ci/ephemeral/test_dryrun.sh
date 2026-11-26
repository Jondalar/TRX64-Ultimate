#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# U20 F-a (D17): the experiment's scripts end to end with --dry-run and fake tokens; no request leaves the machine.
# The path of .github/workflows/ephemeral-a.yml (sweep, JIT config, user data, create, wait, online, report, delete),
# then:
#  - no token, no JIT config and no root password in any output; under GitHub Actions the JIT config and the root
#    password appear only in their ::add-mask:: lines, which the runner swallows;
#  - the rendered user data passes bash -n, has no @FIELD@ left, fits the API's 32 KiB and holds the JIT config;
#  - create picks the first available pair (the canned cpx02 is unavailable in nbg1, available in fsn1);
#  - the sweep deletes the servers older than its limit and keeps the younger one, and refuses a selector without
#    purpose=gh-ephemeral; create refuses a label value with "/"; without a token nothing is sent;
#  - delete --selector "run in (...)" finds its server and deletes the Primary IP that server left unassigned;
#  - proof.sh writes its table from a stamps file;
#  - the label rule: the JIT request carries only the run's random label; the workflow neither requests nor names
#    self-hosted (or an OS or architecture label); jit refuses such a label, and fails without writing the config when
#    GitHub's answer lists a label more than requested.
#
# U20 F-b (D25): the snapshot commands of hcloud.sh (image, delete-image, snapshot, ssh-key-*: the newest snapshot is
# taken, --keep-newest keeps it, a selector or label set without purpose=gh-ephemeral is refused), the whole path of
# snapshot.sh (snapshot made, server and one-time key deleted), and the label rule for ephemeral-b.yml as well.
#
#   ci/ephemeral/test_dryrun.sh          exit 0 when every check passed
set -euo pipefail
E="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WF="$E/../../.github/workflows/ephemeral-a.yml"
WFB="$E/../../.github/workflows/ephemeral-b.yml"
W=$(mktemp -d "${TMPDIR:-/tmp}/eph-test.XXXXXX")
trap 'rm -rf "$W"' EXIT
export HCLOUD_TOKEN=fake-hcloud-token-0123456789 RUNNER_REG_TOKEN=github_pat_fake_0123456789
unset GITHUB_ACTIONS GITHUB_OUTPUT GITHUB_STEP_SUMMARY
JIT=RkFLRS1KSVQtQ09ORklHLWRyeS1ydW4tb25seQ==
ROOTPW=FAKE-ROOT-PASSWORD-dry-run
LOG=$W/all.log
OUT=$W/outputs
: > "$OUT"   # a failed first step must not end the test: every check reports
fails=0

result() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails + 1)); fi; }
check() { local name=$1 rc=0; shift; "$@" || rc=$?; result "$rc" "$name"; }
# a step of the path: its output into the log (and into its own file), its exit code a check
step() {
  local name=$1 rc=0; shift
  "$@" > "$W/step.log" 2>&1 || rc=$?
  cat "$W/step.log" >> "$LOG"
  result "$rc" "step: $name"
}
out_is() { [ "$(awk -F= -v k="$1" '$1 == k { v = substr($0, length(k) + 2) } END { print v }' "$OUT")" = "$2" ]; }
not_in() { ! grep -F -q -- "$1" "$2"; }
only_masked() { awk -v s="$1" 'index($0, s) && index($0, "::add-mask::") != 1 { bad = 1 } END { exit bad }' "$2"; }
has() { grep -F -q -- "$1" "$2"; }

# ---- the workflow's path
step "start: sweep" "$E/hcloud.sh" --dry-run sweep --older-than 90 --selector "purpose=gh-ephemeral,repo=Example.dry-run"
check "sweep: 999-1 (200 min) and 998-2 (120 min) deleted" bash -c "grep -F -q 'server 100002 (gh-eph-999-1): deleted' '$W/step.log' &&
  grep -F -q 'server 100003 (gh-eph-998-2): deleted' '$W/step.log'"
check "sweep: 1000-1 (5 min) kept" bash -c "! grep -F -q 'server 100001' '$W/step.log' && grep -F -q '1 younger kept' '$W/step.log'"
check "sweep: the Primary IP left by 999-1 deleted" has "primary IP 200002 (192.0.2.2): deleted" "$W/step.log"
step "start: JIT config" "$E/ghrunner.sh" --dry-run jit --repo Example/dry-run --name gh-eph-1-1 \
  --label eph-1-1-abcdef --out "$W/jit" --outputs "$OUT"
check "JIT request: the labels are exactly [eph-1-1-abcdef]" bash -c "awk '/\"labels\": \\[/ { on = 1; next } on && /\\]/ { exit }
  on { gsub(/[ \",]/, \"\"); print }' '$W/step.log' | tr '\n' ' ' | awk '{ exit !(\$0 == \"eph-1-1-abcdef \") }'"
check "JIT request: no self-hosted label" not_in self-hosted "$W/step.log"
step "start: user data" "$E/ghrunner.sh" user-data --jit-file "$W/jit" --label eph-1-1-abcdef --cap-minutes 45 \
  --out "$W/user-data" --outputs "$OUT"
step "start: create" "$E/hcloud.sh" --dry-run create --name gh-eph-1-1 --label purpose=gh-ephemeral \
  --label repo=Example.dry-run --label run=1-1 --user-data "$W/user-data" --outputs "$OUT"
check "create: cpx02 in fsn1 (nbg1 unavailable)" bash -c "$(declare -f out_is); OUT='$OUT'; out_is server_type cpx02 && out_is location fsn1"
check "create: the body named the labels, IPv4 only, no IPv6" bash -c "grep -F -q '\"run\": \"1-1\"' '$W/step.log' &&
  grep -F -q '\"enable_ipv6\": false' '$W/step.log' && grep -F -q 'bytes of cloud-init user data, not shown' '$W/step.log'"
sid=$(awk -F= '$1 == "server_id" { print $2 }' "$OUT")
rid=$(awk -F= '$1 == "runner_id" { print $2 }' "$OUT")
step "start: wait for the server" "$E/hcloud.sh" --dry-run wait --id "$sid" --outputs "$OUT"
step "start: wait for the runner" "$E/ghrunner.sh" --dry-run wait-online --repo Example/dry-run --id "$rid" --outputs "$OUT"
check "outputs: server_id, runner_id, labels, the three times" bash -c "$(declare -f out_is); OUT='$OUT';
  out_is server_id 100100 && out_is runner_id 31337 && out_is runner_labels eph-1-1-abcdef &&
  [ -n \"\$(awk -F= '\$1 == \"t_create\"' '$OUT')\" ] && [ -n \"\$(awk -F= '\$1 == \"t_running\"' '$OUT')\" ] &&
  [ -n \"\$(awk -F= '\$1 == \"t_online\"' '$OUT')\" ]"

# ---- the work job's report from a stamps file
t0=$(date -u +%s)
printf 'userdata %s\ndownloaded %s\ndeps %s\nrunner_start %s\n' $((t0 + 20)) $((t0 + 35)) $((t0 + 60)) $((t0 + 70)) > "$W/stamps"
step "work: report" env EPH_STAMPS="$W/stamps" GITHUB_STEP_SUMMARY="$W/summary" T_CREATE="$t0" T_RUNNING=$((t0 + 15)) \
  T_ONLINE=$((t0 + 80)) T_JOB=$((t0 + 85)) SERVER_TYPE=cpx02 SERVER_LOCATION=fsn1 PRICE_HOURLY=0.0000 \
  RUNNER_LABEL=eph-1-1-abcdef "$E/proof.sh"
check "report: the server's stamps and the job start, in seconds after create" bash -c "grep -F -q '| runner started | 70 |' '$W/summary' &&
  grep -F -q '| this job'\"'\"'s first step | 85 |' '$W/summary' && grep -F -q 'cpx02 in fsn1' '$W/summary'"

# ---- the workflow's stop
step "stop: delete by id" "$E/hcloud.sh" --dry-run delete --id "$sid"
step "stop: delete by label, every attempt" "$E/hcloud.sh" --dry-run delete --selector "purpose=gh-ephemeral,run in (999-1,999-2)"
check "stop: the selector found 999-1 and only it" bash -c "grep -F -q 'selector [purpose=gh-ephemeral,run in (999-1,999-2)]: 1 server(s)' '$W/step.log' &&
  grep -F -q 'server 100002 (gh-eph-999-1): deleted' '$W/step.log'"
step "stop: runner by id" "$E/ghrunner.sh" --dry-run delete --repo Example/dry-run --id "$rid"
step "stop: runner by name" "$E/ghrunner.sh" --dry-run delete --repo Example/dry-run --name gh-eph-1-1
step "list" "$E/hcloud.sh" --dry-run list

# ---- the user data
check "user data: bash -n" bash -n "$W/user-data"
check "user data: no @FIELD@ left" awk '/@[A-Z_]+@/ { bad = 1 } END { exit bad }' "$W/user-data"
check "user data: at most 32 KiB" test "$(wc -c < "$W/user-data" | tr -d ' ')" -le 32768
check "user data: holds the JIT config, the label and the cap" bash -c "grep -F -q \"JIT='$JIT'\" '$W/user-data' &&
  grep -F -q 'RUNNER_LABEL=eph-1-1-abcdef' '$W/user-data' && grep -F -q 'CAP_MINUTES=45' '$W/user-data'"
check "JIT file: mode 600" bash -c "[ \"\$(ls -l '$W/jit' | cut -c1-10)\" = '-rw-------' ]"

# ---- refusals
check "sweep refuses a selector without purpose=gh-ephemeral" bash -c "! '$E/hcloud.sh' --dry-run sweep --older-than 1 --selector repo=x > '$W/r1' 2>&1"
check "create refuses a label value with /" bash -c "! '$E/hcloud.sh' --dry-run create --name a --label purpose=gh-ephemeral --label repo=a/b --user-data '$W/user-data' > '$W/r2' 2>&1"
check "create refuses a server without purpose=gh-ephemeral" bash -c "! '$E/hcloud.sh' --dry-run create --name a --label run=1 --user-data '$W/user-data' > '$W/r3' 2>&1"
check "without a token nothing is sent" bash -c "env -u HCLOUD_TOKEN '$E/hcloud.sh' list > '$W/r4' 2>&1; [ \$? = 1 ] && grep -F -q 'HCLOUD_TOKEN is not set' '$W/r4'"
cat "$W"/r? >> "$LOG"

# ---- the label rule (the quoted $... below are text of the workflow, not shell)
for wf in "$WF" "$WFB"; do
  b=$(basename "$wf")
  # shellcheck disable=SC2016
  check "$b: no self-hosted, OS or architecture label outside comments" awk '
    !/^[[:space:]]*#/ && tolower($0) ~ /self-hosted|--label (linux|x64|arm64)/ { bad = 1 } END { exit bad }' "$wf"
  check "$b: work runs on the run's label alone" grep -F -q "runs-on: ['\${{ needs.start.outputs.label }}']" "$wf"
  # shellcheck disable=SC2016,SC1003
  check "$b: the JIT step requests the run's label alone" grep -F -q 'jit --repo "$GITHUB_REPOSITORY" --name "$NAME" --label "$LABEL" \' "$wf"
done
check "jit refuses self-hosted" bash -c "! '$E/ghrunner.sh' --dry-run jit --repo Example/dry-run --name n --label self-hosted --label l --out '$W/j1' > '$W/l1' 2>&1 && [ ! -e '$W/j1' ]"
check "jit refuses X64" bash -c "! '$E/ghrunner.sh' --dry-run jit --repo Example/dry-run --name n --label X64 --label l --out '$W/j2' > '$W/l2' 2>&1"
check "jit fails, no config written, when GitHub adds a label" bash -c "! EPH_FAKE_EXTRA_LABEL=self-hosted '$E/ghrunner.sh' --dry-run jit \
  --repo Example/dry-run --name n --label eph-1-1-abcdef --out '$W/j3' > '$W/l3' 2>&1 && [ ! -e '$W/j3' ] &&
  grep -F -q 'not only the requested' '$W/l3'"
cat "$W"/l? >> "$LOG"

# ---- F-b (D25): the snapshot commands and snapshot.sh
IMG=sha256:49521e302a028d5bf9cf8c7fa063b4ced38a7ce760335b283c31945fda4b195b
step "image: the newest snapshot of role=vivado" "$E/hcloud.sh" --dry-run image --selector purpose=gh-ephemeral,role=vivado
check "image: 300002 (2 days old) before 300001 (30 days)" has "image_id=300002" "$W/step.log"
step "delete-image --keep-newest" "$E/hcloud.sh" --dry-run delete-image --selector purpose=gh-ephemeral,role=vivado --keep-newest
check "delete-image: 300001 deleted, 300002 kept" bash -c "grep -F -q 'snapshot 300001: deleted' '$W/step.log' && ! grep -F -q 'DELETE https://api.hetzner.cloud/v1/images/300002' '$W/step.log'"
check "delete-image: a selector without purpose=gh-ephemeral is refused" bash -c "! '$E/hcloud.sh' --dry-run delete-image --selector role=vivado > /dev/null 2>&1"
check "snapshot: labels without purpose=gh-ephemeral are refused" bash -c "! '$E/hcloud.sh' --dry-run snapshot --id 1 --description d --label role=vivado > /dev/null 2>&1"
step "snapshot.sh: the whole path" "$E/snapshot.sh" --dry-run --type cpx02 --source host.example --archive a.tar.zst --image-id "$IMG" --cache /c --replace
check "snapshot.sh: snapshot 300100 made" has "image_id=300100" "$W/step.log"
check "snapshot.sh: the server made with the one-time key" has '"ssh_keys": [' "$W/step.log"
check "snapshot.sh: the server and the key deleted" bash -c "grep -F -q 'server 100100 (gh-eph-dry-run): deleted' '$W/step.log' && grep -F -q 'ssh key 400100: deleted' '$W/step.log'"
check "snapshot.sh: --replace deletes the older snapshots" has "snapshot 300001: deleted" "$W/step.log"
check "snapshot.sh: a bad image ID is refused" bash -c "! '$E/snapshot.sh' --dry-run --source h --archive a --image-id sha256:00 > /dev/null 2>&1"

# ---- secrets
check "log: no Hetzner token" not_in "$HCLOUD_TOKEN" "$LOG"
check "log: no GitHub token" not_in "$RUNNER_REG_TOKEN" "$LOG"
check "log: no JIT config" not_in "$JIT" "$LOG"
check "log: no root password" not_in "$ROOTPW" "$LOG"
check "outputs: no JIT config, no root password" bash -c "! grep -F -q -e '$JIT' -e '$ROOTPW' '$OUT'"
GA=$W/actions.log
GITHUB_ACTIONS=true "$E/ghrunner.sh" --dry-run jit --repo Example/dry-run --name n --label l --out "$W/jit2" > "$GA" 2>&1 || true
GITHUB_ACTIONS=true "$E/hcloud.sh" --dry-run create --name a --label purpose=gh-ephemeral --user-data "$W/user-data" >> "$GA" 2>&1 || true
check "Actions: JIT config masked, and only in its ::add-mask:: line" bash -c "grep -F -q '::add-mask::$JIT' '$GA' && $(declare -f only_masked); only_masked '$JIT' '$GA'"
check "Actions: root password masked, and only in its ::add-mask:: line" bash -c "grep -F -q '::add-mask::$ROOTPW' '$GA' && $(declare -f only_masked); only_masked '$ROOTPW' '$GA'"
check "Actions: no token" bash -c "! grep -F -q -e '$HCLOUD_TOKEN' -e '$RUNNER_REG_TOKEN' '$GA'"

if command -v shellcheck > /dev/null 2>&1; then check "shellcheck" shellcheck -x "$E"/*.sh; else echo "skip shellcheck (not installed)"; fi

echo "log of the dry run: $(wc -l < "$LOG" | tr -d ' ') lines"
if [ "$fails" -gt 0 ]; then echo "FAILED: $fails"; exit 1; fi
echo "all passed"
