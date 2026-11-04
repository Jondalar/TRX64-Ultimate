#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Stage `update`: the tester update files trx64_update_<commit8>_<YYYYMMDD>.cfw / .ue2 (T49), built
# from exactly the c64_basic.bit and ultimate.app of this commit's CI package and added to the same
# package version (<branch>-<8 digit commit> and <branch>-latest of trx64-ultimate), with its
# SHA256SUMS extended. Runs in u64-build-env:ci after `package`.
#
# Only for integ-m1, main and v* tags (ci/select.sh update); any other ref prints
# "update SKIPPED" and ends green at once. The decision is made here, never by a job-level `if:`
# (docs/specs/T31-ci-pipeline.md, decision 2). UPDATE_FORCE=true (dispatch input `update`) builds
# on any branch.
#
# Hook for the release tag: TRX_UPDATE_NAME replaces the file name base (tools/make_update.sh); the
# tag logic itself is not built here.
#
#   PACKAGE_TOKEN=... [APP_TOKEN=...] ci/update.sh
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

if [ "$("$ROOT/ci/select.sh" update)" != yes ]; then
  echo "update SKIPPED: only integ-m1/main (and v* tags); dispatch with update = true to build on this branch"
  exit 0
fi
release_reuse_skip update
: "${PACKAGE_TOKEN:?PACKAGE_TOKEN is not set: the package cannot be read or extended}"
: "${GITHUB_SERVER_URL:?}" "${GITHUB_REPOSITORY:?}"

OWNER="${GITHUB_REPOSITORY%%/*}"
API="$GITHUB_SERVER_URL/api/packages/$OWNER/generic/trx64-ultimate"
sha8="$(git -C "$ROOT" rev-parse --short=8 HEAD)"
ver="$(branch_slug)-$sha8"
pcurl() { curl -sf -H "Authorization: token $PACKAGE_TOKEN" "$@"; }

W="$ROOT/.update"
rm -rf "$W"; mkdir -p "$W/in" "$W/out"

echo "=== inputs: trx64-ultimate/$ver"
pcurl -o "$W/in/manifest.json" "$API/$ver/manifest.json" || { echo "no package trx64-ultimate/$ver: stage package did not publish it" >&2; exit 1; }
python3 - "$W/in/manifest.json" "$(git -C "$ROOT" rev-parse HEAD)" > "$W/in/files.txt" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
if m["trx64_commit"] != sys.argv[2]:
    sys.exit("package commit %s is not the commit of this run %s" % (m["trx64_commit"], sys.argv[2]))
for f in ("c64_basic.bit", "ultimate.app"):
    print(f, m["files"][f]["sha256"])
PY
while read -r f sum; do
  pcurl -o "$W/in/$f" "$API/$ver/$f"
  [ "$(sha256sum "$W/in/$f" | cut -d' ' -f1)" = "$sum" ] || { echo "$f: SHA-256 differs from the manifest" >&2; exit 1; }
done < "$W/in/files.txt"
echo "inputs match the manifest ($(cut -d' ' -f1 "$W/in/files.txt" | tr '\n' ' '))"

echo "=== updater"
build_update "$W/in/manifest.json" "$W/in/c64_basic.bit" "$W/in/ultimate.app" "$W/out"
D="$(ls -d "$W"/out/trx64-*)"
pub="$W/pub"; mkdir -p "$pub"
cp "$D"/trx64_update_*.cfw "$D"/trx64_update_*.ue2 "$pub/"
ls "$pub"/*.cfw "$pub"/*.ue2 >/dev/null

# SHA256SUMS of the package version: its lines for earlier update files (another build date, a rerun)
# go, ours are added. Those earlier files are removed from both versions below.
pcurl -o "$W/in/SHA256SUMS" "$API/$ver/SHA256SUMS" || { echo "package $ver has no SHA256SUMS" >&2; exit 1; }
{ grep -v ' trx64_update_' "$W/in/SHA256SUMS" || true; (cd "$pub" && sha256sum -- *); } > "$pub/SHA256SUMS"
cat "$pub/SHA256SUMS"

# stale update files of earlier builds of this commit
for v in "$ver" "$(branch_slug)-latest"; do
  pcurl "$GITHUB_SERVER_URL/api/v1/packages/$OWNER/generic/trx64-ultimate/$v/files" 2>/dev/null \
    | python3 -c 'import json,sys
for r in json.load(sys.stdin):
    if r["name"].startswith("trx64_update_"): print(r["name"])' 2>/dev/null \
    | while read -r old; do
        [ -e "$pub/$old" ] || curl -s -o /dev/null -X DELETE -H "Authorization: token $PACKAGE_TOKEN" "$API/$v/$old" || true
      done || true
done

publish_files "$pub"
echo "=== update done in $(lap): $(cd "$pub" && ls trx64_update_* | tr '\n' ' ')"
