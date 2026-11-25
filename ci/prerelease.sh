#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# U20 B (MultiCore T239.1 D1, ci/prerelease.sh form): the last job of a push to main or a v* tag. Its pre-release in the builds repository (D10, D12) is
# what MultiCore's runners take trx64u.cor and the two sub-apps from (D22, MultiCore T261 D7), each by its SHA-256 and size, unchanged:
#   trx64u.cor      the .cor the core lane packed (artifact deliver-trx64u), byte for byte
#   trex_crt.u2a    the sub-app TREX CRT, the file the .cor carries in record 8 (apps/trex_crt.u2a)
#   online.u2a      the online search, the library the .cor carries in record 7
#                   (the core lane's step deliver checked both against the .cor; softboot is not ours to ship, D22)
#   manifest.json   source repository, commit, ref, the .cor's version, the bitstream entry and its release, the SDK it was linked from, and `files`:
#                   SHA-256 and size of each of the three files above
#   SHA256SUMS      the four files
# No ELF, report or bitstream goes in: the bitstream release <prefix>bit-<entry> holds the bitstream and its reports.
#
#   ci/prerelease.sh     env: CI_DELIVER_IN (the directory of the artifact deliver-trx64u), CI_STAGE (default ./prerelease-stage),
#                        PRERELEASE_NO_PUBLISH=1 (tests: stage and check, publish nothing); for the publish ci/publish_release.py's env (BUILDS_TOKEN, ...)
# Tag: <prefix><branch>-<sha8> (ci/pkg.env BUILDS_TAG_PREFIX; a v* tag's branch slug is the tag: fw-v0.4.0-<sha8>).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$HERE/lib.sh"
# ci/pkg.env, a value of the environment first (tests point MC_BUILDS_REPO at another repository)
for v in MC_BUILDS_REPO MC_SRC_REPO BUILDS_TAG_PREFIX; do
  [ -n "${!v:-}" ] || printf -v "$v" %s "$(sed -n "s/^$v=//p" "$CI_CODE/ci/pkg.env")"
done
: "${CI_DELIVER_IN:?CI_DELIVER_IN: the directory of the artifact deliver-trx64u}"
D="$CI_DELIVER_IN"
stage="${CI_STAGE:-$PWD/prerelease-stage}"
die() { echo "prerelease: $*" >&2; exit 1; }
ASSETS=(trx64u.cor trex_crt.u2a online.u2a)
for f in "${ASSETS[@]}" deliver.json SHA256SUMS; do [ -s "$D/$f" ] || die "the artifact deliver-trx64u has no $f"; done
for f in "${ASSETS[@]}"; do grep -q "  $f\$" "$D/SHA256SUMS" || die "the artifact's SHA256SUMS does not list $f"; done
(cd "$D" && sha256sum -c --quiet SHA256SUMS) || die "the artifact's SHA256SUMS does not match its files"
sha="$(git -C "$ROOT" rev-parse HEAD)"
tag="${BUILDS_TAG_PREFIX}$(branch_slug)-${sha:0:8}"
rm -rf "$stage"; mkdir -p "$stage"
for f in "${ASSETS[@]}"; do cp "$D/$f" "$stage/"; done
python3 -I - "$D/deliver.json" "$stage" "$sha" "$tag" "$MC_SRC_REPO" "$(branch_slug)" "${ASSETS[@]}" <<'PY'
import hashlib, json, os, sys
deliver, stage, sha, tag, repo, ref = sys.argv[1:7]
d = json.load(open(deliver))
if d.get("commit") != sha:
    sys.exit("prerelease: deliver.json is of commit %s, this run is %s" % (d.get("commit"), sha))
files = {}
for n in sys.argv[7:]:
    b = open(os.path.join(stage, n), "rb").read()
    files[n] = {"sha256": hashlib.sha256(b).hexdigest(), "size": len(b)}
m = {"source_repo": repo, "commit": sha, "ref": ref, "tag": tag, "version": d["version"],
     "bitstream": d["bitstream"], "sdk": d["sdk"], "files": files}
json.dump(m, open(os.path.join(stage, "manifest.json"), "w"), indent=2, sort_keys=True)
PY
(cd "$stage" && sha256sum "${ASSETS[@]}" manifest.json > SHA256SUMS && cat SHA256SUMS)
echo "prerelease: $tag staged in $stage"
[ -z "${PRERELEASE_NO_PUBLISH:-}" ] || { echo "prerelease: PRERELEASE_NO_PUBLISH set, nothing published"; exit 0; }
python3 -I "$CI_CODE/ci/publish_release.py" --dir "$stage" --tag "$tag" --sha "$sha"
