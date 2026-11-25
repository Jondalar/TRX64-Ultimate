#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# PROVENANCE: the form of TRX64U-MultiCore tools/release_mc.sh (t239.3 d9ddadeb1; Alex Damhuis' code in both repositories), U20 B. Replaces the Gitea
# release of this repository (ci/release.sh and .gitea/workflows/release.yml, gone with U20 A5).
# One release of trx64u.cor and the two sub-apps it carries (trex_crt.u2a, online.u2a; U20 D22), taken over from a finished pre-release of the builds
# repository into a GitHub release of this repository.
#
#   tools/release_trx64u.sh <pre-release tag> [--publish --notes FILE] [--out DIR]
#       --publish      also create the GitHub release v<x.y.z> (without it: download and check only)
#       --notes FILE   the release text (needed with --publish)
#       --out DIR      where the pre-release is downloaded to, DIR/pkg-<tag> [default: dist]
#
# <pre-release tag> is the pre-release the pipeline published in the builds repository (ci/pkg.env MC_BUILDS_REPO), <prefix><branch>-<8 hex of the
# commit>, e.g. fw-main-51e89821, or fw-v0.4.0-<sha8> for the build of the tag v0.4.0; `latest` takes the newest fw-main-<sha8> (tools/builds.sh latest).
# Nothing is packed, rebuilt or rewritten here: the release holds the bytes of the pre-release.
#
# What it does, in this order:
#   1. downloads every asset (tools/builds.sh: each file against the SHA-256 GitHub lists and the release's SHA256SUMS), then all of them against
#      SHA256SUMS; every asset must be listed there;
#   2. manifest.json: its commit starts with the tag's <sha8>, its source repository is this one, its `files` name every other asset but SHA256SUMS
#      (trx64u.cor, trex_crt.u2a and online.u2a at least) with the downloaded file's SHA-256 and size, and nothing else;
#   3. the .cor (software/trx64u/tools/emu_ops.py's reader): its own manifest (record 1) says id trx64u and the version manifest.json names, and it
#      carries trex_crt.u2a (record 8, apps/trex_crt.u2a) and online.u2a (a library of record 7) byte for byte;
#   4. with --publish: the version is a plain x.y.z (a release is made from the pre-release of its tag build), the public-tree check passes on every file
#      (tools/check_public_tree.py --files, U20 A8: no ROM of ci/private.txt whole or in part, no text rule hit), and the tag v<x.y.z> exists in this
#      repository and names the commit
#      (Alex makes the tag; gh release create --verify-tag never makes one);
#   5. without --publish: prints what would be published; with it: gh release create v<x.y.z> as a draft with every file of the pre-release, then
#      published once the draft holds them all.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# ci/pkg.env, a value of the environment first (tests point MC_BUILDS_REPO at another repository)
for v in MC_BUILDS_REPO MC_SRC_REPO BUILDS_TAG_PREFIX; do
  [ -n "${!v:-}" ] || printf -v "$v" %s "$(sed -n "s/^$v=//p" "$ROOT/ci/pkg.env")"
done
OUT="$ROOT/dist" PUBLISH=0 NOTES=""
VER="${1:-}"
[ -n "$VER" ] || { sed -n '7,11p' "$0"; exit 2; }
case "$VER" in -h|--help) sed -n '7,11p' "$0"; exit 0;; esac
shift
while [ $# -gt 0 ]; do
    case "$1" in
        --publish) PUBLISH=1; shift;;
        --notes) NOTES="$2"; shift 2;;
        --out) OUT="$2"; shift 2;;
        *) echo "unknown option $1" >&2; exit 2;;
    esac
done
die() { echo "$*" >&2; exit 1; }
[ "$PUBLISH" = 0 ] || [ -s "$NOTES" ] || die "--publish needs --notes FILE (the release text)"
# tools/builds.py sends GH_TOKEN to the API only (the rate limit); the token of the gh login is not handed over when BUILDS_API names a test API
if [ -z "${GH_TOKEN:-}" ] && [ -z "${BUILDS_API:-}" ]; then t="$(gh auth token 2>/dev/null || true)"; [ -z "$t" ] || export GH_TOKEN="$t"; unset t; fi
[ "$VER" != latest ] || VER="$("$HERE/builds.sh" latest)"
case "$VER" in "$BUILDS_TAG_PREFIX"*) ;; *) die "$VER is not a pre-release of this repository (its tags start with $BUILDS_TAG_PREFIX)";; esac

PKG="$OUT/pkg-$VER"
rm -rf "$PKG"; mkdir -p "$PKG"      # only what this run downloads counts: a file of an earlier download is not one of the pre-release
echo ">>> pre-release $VER of $MC_BUILDS_REPO"
files=$("$HERE/builds.sh" files "$VER") || die "pre-release $VER not found"
[ -n "$files" ] || die "pre-release $VER holds no file"
for f in $files; do
    [[ "$f" =~ ^[A-Za-z0-9][A-Za-z0-9._+-]{0,99}$ ]] || die "$f: GitHub would rename this asset: release stopped"
    "$HERE/builds.sh" fetch "$VER" "$f" "$PKG/$f" >/dev/null 2>&1 || die "cannot fetch $f (or its SHA-256 differs)"
done
[ -s "$PKG/SHA256SUMS" ] || die "pre-release $VER has no SHA256SUMS"
(cd "$PKG" && sha256sum -c --quiet SHA256SUMS 2>/dev/null || shasum -a 256 -c --quiet SHA256SUMS) || die "SHA256SUMS does not match the downloaded files: release stopped"
assets=()
for f in $files; do
    if [ "$f" != SHA256SUMS ] && ! awk -v f="$f" '$2 == f { found = 1 } END { exit !found }' "$PKG/SHA256SUMS"; then die "$f is not listed in SHA256SUMS: release stopped"; fi
    assets+=("$PKG/$f")
done
for f in trx64u.cor trex_crt.u2a online.u2a manifest.json; do [ -s "$PKG/$f" ] || die "pre-release $VER has no $f"; done
echo "    $(echo "$files" | wc -w | tr -d ' ') files downloaded, SHA256SUMS ok"

# the commit and the version go to a file: macOS's bash 3.2 expands the braces of a here-document inside <(...)
RES="$OUT/pkg-$VER.checked"
rm -f "$RES"
# shellcheck disable=SC2086  # files: the asset names, checked above to be plain
python3 -I - "$PKG" "$VER" "$MC_SRC_REPO" "$ROOT/software/trx64u/tools" "$RES" $files <<'PY' || die "the pre-release does not hold together: release stopped"
import hashlib, json, os, sys
pkg, tag, repo, tools, res = sys.argv[1:6]
m = json.load(open(os.path.join(pkg, "manifest.json")))
if m.get("source_repo") != repo:
    sys.exit("manifest.json names the repository %r, not %s" % (m.get("source_repo"), repo))
if not str(m.get("commit", "")).startswith(tag.rsplit("-", 1)[-1]) or len(m.get("commit", "")) != 40:
    sys.exit("manifest.json names commit %r, the tag %s another" % (m.get("commit"), tag))
want = set(sys.argv[6:]) - {"manifest.json", "SHA256SUMS"}
have = m.get("files") if isinstance(m.get("files"), dict) else {}
if set(have) != want:
    sys.exit("manifest.json's files name %s, the pre-release holds %s" % (sorted(have), sorted(want)))
data = {n: open(os.path.join(pkg, n), "rb").read() for n in sorted(want)}
for n, b in data.items():
    e = have[n] if isinstance(have[n], dict) else {}
    if e.get("sha256") != hashlib.sha256(b).hexdigest() or e.get("size") != len(b):
        sys.exit("%s is not the file manifest.json names (SHA-256 and size)" % n)
sys.path.insert(0, tools)
import emu_ops
try:
    man, recs, _ = emu_ops.read_cor(os.path.join(pkg, "trx64u.cor"))
    emu_ops.check_subapps(recs, data)
except emu_ops.Failed as e:
    sys.exit("trx64u.cor: %s" % e)
if man.get("id") != "trx64u" or man.get("version") != m.get("version"):
    sys.exit("the .cor says id %r version %r, manifest.json version %r" % (man.get("id"), man.get("version"), m.get("version")))
open(res, "w").write("%s %s\n" % (m["commit"], m["version"]))
PY
read -r COMMIT REL < "$RES"
rm -f "$RES"
echo "    commit $COMMIT, version $REL (manifest.json and the .cor's record 1 agree; every asset is the file manifest.json names;"
echo "    trex_crt.u2a and online.u2a are the files the .cor carries)"

echo ">>> would publish v$REL in $MC_SRC_REPO from $PKG:"
printf '    %s\n' $files
if [ "$PUBLISH" != 1 ]; then
    echo "    publish it with: tools/release_trx64u.sh $VER --publish --notes <file>"
    exit 0
fi
[[ "$REL" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || die "pre-release $VER is a dev build ($REL): a release is made from the pre-release of a tag build"
python3 -I "$ROOT/tools/check_public_tree.py" --files "${assets[@]}" || die "pre-release $VER holds private content: release stopped"
gh auth status >/dev/null 2>&1 || die "gh is not logged in (gh auth login): the tag is read and the release made with gh"
TAGSHA="$(gh api "repos/$MC_SRC_REPO/commits/v$REL" --jq .sha 2>/dev/null || true)"
[ -n "$TAGSHA" ] || die "no tag v$REL in $MC_SRC_REPO: Alex creates the tag, then run this again"
[ "$TAGSHA" = "$COMMIT" ] || die "tag v$REL is $TAGSHA, the pre-release was built from $COMMIT: release stopped"
echo "    tag v$REL names $COMMIT"
echo ">>> release v$REL in $MC_SRC_REPO"
gh release create "v$REL" --repo "$MC_SRC_REPO" --verify-tag --draft --title "TRX64 Ultimate $REL" --notes-file "$NOTES" "${assets[@]}"
want="${#assets[@]}"
have="$(gh release view "v$REL" --repo "$MC_SRC_REPO" --json assets --jq '.assets | length')"
[ "$have" = "$want" ] || die "the draft holds $have assets, $want were attached: left as a draft"
gh release edit "v$REL" --repo "$MC_SRC_REPO" --draft=false
gh release view "v$REL" --repo "$MC_SRC_REPO" --json url,isImmutable --jq '">>> \(.url)  immutable: \(.isImmutable)"'
