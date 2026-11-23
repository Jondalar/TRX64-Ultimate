#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Stage `cor`: trx64u.cor (U18) from this commit's CI package. Its c64_basic.bit, the pinned TRX64 SDK from our registry
# (software/trx64u/sdk.pin, fetch_sdk.sh --download) and our libraries, TOMLs, icon and files (software/trx64u/pack_cor.sh)
# make trx64u_<commit8>.cor, which goes into the same package version (<branch>-<commit8> and <branch>-latest of
# trx64-ultimate) with its SHA256SUMS extended. Runs in u64-build-env:ci after `package`, on every branch that has
# software/trx64u; on any other it prints "cor SKIPPED" and ends green at once (decided here, never by a job-level `if:`,
# docs/specs/T31-ci-pipeline.md decision 2).
#
#   PACKAGE_TOKEN=... ci/cor.sh
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

T="$ROOT/software/trx64u"
if [ ! -f "$T/pack_cor.sh" ]; then
  echo "cor SKIPPED: this branch has no software/trx64u"
  exit 0
fi
release_reuse_skip cor
: "${PACKAGE_TOKEN:?PACKAGE_TOKEN is not set: the package cannot be read or extended}"
: "${GITHUB_SERVER_URL:?}" "${GITHUB_REPOSITORY:?}"

OWNER="${GITHUB_REPOSITORY%%/*}"
API="$GITHUB_SERVER_URL/api/packages/$OWNER/generic/trx64-ultimate"
sha8="$(git -C "$ROOT" rev-parse --short=8 HEAD)"
ver="$(branch_slug)-$sha8"
pcurl() { curl -sf -H "Authorization: token $PACKAGE_TOKEN" "$@"; }

W="$ROOT/.cor"
rm -rf "$W"; mkdir -p "$W/in" "$W/pub"

echo "=== inputs: trx64-ultimate/$ver"
pcurl -o "$W/in/manifest.json" "$API/$ver/manifest.json" || { echo "no package trx64-ultimate/$ver: stage package did not publish it" >&2; exit 1; }
sum="$(python3 - "$W/in/manifest.json" "$(git -C "$ROOT" rev-parse HEAD)" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
if m["trx64_commit"] != sys.argv[2]:
    sys.exit("package commit %s is not the commit of this run %s" % (m["trx64_commit"], sys.argv[2]))
print(m["files"]["c64_basic.bit"]["sha256"])
PY
)"
pcurl -o "$W/in/c64_basic.bit" "$API/$ver/c64_basic.bit"
[ "$(sha256sum "$W/in/c64_basic.bit" | cut -d' ' -f1)" = "$sum" ] || { echo "c64_basic.bit: SHA-256 differs from the manifest" >&2; exit 1; }
echo "c64_basic.bit matches the manifest"

echo "=== SDK"
GITEA_URL="$GITHUB_SERVER_URL" sh "$T/fetch_sdk.sh" --download

echo "=== trx64u.cor"
sh "$T/pack_cor.sh" --bit "$W/in/c64_basic.bit" --bit-commit "$sha8" --version "$ver" --out "$W/pub/trx64u_$sha8.cor"

# SHA256SUMS of the package version: its lines for earlier .cor files (a rerun) go, ours are added; those earlier
# files are removed from both versions below.
pcurl -o "$W/in/SHA256SUMS" "$API/$ver/SHA256SUMS" || { echo "package $ver has no SHA256SUMS" >&2; exit 1; }
{ grep -v ' trx64u_.*\.cor$' "$W/in/SHA256SUMS" || true; (cd "$W/pub" && sha256sum -- trx64u_*.cor); } > "$W/pub/SHA256SUMS"
for v in "$ver" "$(branch_slug)-latest"; do
  pcurl "$GITHUB_SERVER_URL/api/v1/packages/$OWNER/generic/trx64-ultimate/$v/files" 2>/dev/null \
    | python3 -c 'import json,sys
for r in json.load(sys.stdin):
    if r["name"].startswith("trx64u_") and r["name"].endswith(".cor"): print(r["name"])' 2>/dev/null \
    | while read -r old; do
        [ -e "$W/pub/$old" ] || curl -s -o /dev/null -X DELETE -H "Authorization: token $PACKAGE_TOKEN" "$API/$v/$old" || true
      done || true
done

publish_files "$W/pub"
echo "=== cor done in $(lap): trx64u_$sha8.cor"
