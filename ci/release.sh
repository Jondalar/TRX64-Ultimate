#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Release pipeline (T49, docs/specs/T49-update-packages.md "Release pipeline"), in u64-build-env:ci.
#
#   ci/release.sh resolve   which commit is released, and does the CI package of that commit exist?
#                           If so: leave $HANDOFF/release-reuse, the stages app/bit/package then skip.
#   ci/release.sh build     bitstream + app from that package -> updater (.cfw/.ue2), our apps (.u2a),
#                           RELEASE_NOTES.md, SHA256SUMS, trx64-<id>.zip; publish as generic package
#                           trx64-release/<id> and as a Gitea Release.
#
# Environment: REL_REF (the ref to release; empty = the ref of the run), PACKAGE_TOKEN (generic packages),
# REL_TOKEN (Gitea API: releases; PACKAGE_TOKEN is tried when that one is refused), GITHUB_* of the run.
# The tokens are never printed. Locally (no run): REL_NO_PUBLISH=1 builds and stops before publishing.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

SERVER="${GITHUB_SERVER_URL:-}"
REPO="${GITHUB_REPOSITORY:-TREX/TRX64-Ultimate}"
OWNER="${REPO%%/*}"
REF="${REL_REF:-${GITHUB_REF_NAME:-main}}"
SLUG="$(printf '%s' "$REF" | tr '/' '_')"
# next to the hand-over directory, not in it: the package stage removes that one when it is done
ENVF="$(dirname "$HANDOFF")/release-$RUN_ID.env"
PKG_API="$SERVER/api/packages/$OWNER/generic"
# Apps of apps/ that are not for testers (diagnostics and bring-up tools). Everything else in apps/ is built.
# The SDK samples (software/appsdk/samples: hello, restcheck) are test programs and never part of it.
EXCLUDE_APPS="esp_probe"

pcurl() { curl -sf -H "Authorization: token ${PACKAGE_TOKEN:-}" "$@"; }

# find_version <sha8>: the version of the CI package trx64-ultimate built from that commit
find_version() {
  local sha8="$1" v="$SLUG-$1"
  if pcurl -o /dev/null "$PKG_API/trx64-ultimate/$v/manifest.json"; then echo "$v"; return 0; fi
  # another branch name (the same commit built under another ref): list the registry
  local page
  for page in 1 2 3 4 5 6 7 8 9 10; do
    pcurl "$SERVER/api/v1/packages/$OWNER?type=generic&q=trx64-ultimate&limit=50&page=$page" 2>/dev/null \
      | python3 -c '
import json, sys
sha8 = sys.argv[1]
try:
    rows = json.load(sys.stdin)
except ValueError:
    rows = []
for r in rows:
    if r.get("name") == "trx64-ultimate" and r.get("version", "").endswith("-" + sha8) and "-latest" not in r["version"]:
        print(r["version"]); break
' "$sha8" | grep . && return 0
  done
  return 1
}

cmd="${1:?resolve or build}"

case "$cmd" in
resolve)
  mkdir -p "$HANDOFF"
  rm -f "$HANDOFF/release-reuse" "$ENVF"; mkdir -p "$(dirname "$ENVF")"
  sha="$(git -C "$ROOT" rev-parse HEAD)"; sha8="${sha:0:8}"
  echo "release ref $REF = $sha"
  ver=""
  ver="$(find_version "$sha8" || true)"
  {
    echo "SHA=$sha"
    echo "VERSION=$ver"
  } > "$ENVF"
  if [ -n "$ver" ]; then
    echo "$ver" > "$HANDOFF/release-reuse"
    echo "=== the CI package trx64-ultimate/$ver exists: the release reuses it"
  else
    if [ "$REF" != "${GITHUB_REF_NAME:-$REF}" ]; then
      echo "No CI package for $REF ($sha8) and this run was started on ${GITHUB_REF_NAME}: the stages build the branch the run" >&2
      echo "was started on. Dispatch the workflow with $REF selected as the branch, or let the push pipeline finish first." >&2
      exit 1
    fi
    echo "=== no CI package for $sha8: the stages app, bit-c64u and package build it"
  fi
  ;;

build)
  [ -f "$ENVF" ] || { echo "no $ENVF: job resolve did not run on this machine" >&2; exit 1; }
  # shellcheck disable=SC1090
  source "$ENVF"
  git -C "$ROOT" checkout -q "$SHA"
  W="${REL_WORK:-$ROOT/.release}"
  rm -rf "$W"; mkdir -p "$W/in" "$W/out"

  # ---- the inputs: bitstream and app of the CI package of this commit
  ver="$(find_version "${SHA:0:8}")" || { echo "the CI package of ${SHA:0:8} is not in the registry" >&2; exit 1; }
  echo "=== inputs: trx64-ultimate/$ver"
  pcurl -o "$W/in/manifest.json" "$PKG_API/trx64-ultimate/$ver/manifest.json"
  python3 - "$W/in/manifest.json" "$SHA" > "$W/in/files.txt" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
if m["trx64_commit"] != sys.argv[2]:
    sys.exit("package commit %s is not the release commit %s" % (m["trx64_commit"], sys.argv[2]))
for f, i in sorted(m["files"].items()):
    if f in ("c64_basic.bit", "ultimate.app") or f.endswith(".rpt"):
        print(f, i["sha256"])
PY
  while read -r f sum; do
    pcurl -o "$W/in/$f" "$PKG_API/trx64-ultimate/$ver/$f"
    [ "$(sha256sum "$W/in/$f" | cut -d' ' -f1)" = "$sum" ] || { echo "$f: SHA-256 differs from the manifest" >&2; exit 1; }
  done < "$W/in/files.txt"
  [ -s "$W/in/c64_basic.bit" ] && [ -s "$W/in/ultimate.app" ] || { echo "package lacks c64_basic.bit or ultimate.app" >&2; exit 1; }

  # ---- the app submodule: history for the notes, the sources for the updater and the apps
  APP="$ROOT/software/ultimate-app"

  # ---- the updater (ci/lib.sh build_update: submodule, commit check, ESP-IDF env, make_update.sh)
  echo "=== updater"
  build_update "$W/in/manifest.json" "$W/in/c64_basic.bit" "$W/in/ultimate.app" "$W/out"
  if [ "$(git -C "$APP" rev-parse --is-shallow-repository)" = true ]; then
    git -C "$APP" fetch -q --unshallow || echo "app history stays shallow (notes will say so)"
  fi
  ID="$(basename "$(ls -d "$W"/out/trx64-*)")"; ID="${ID#trx64-}"
  D="$W/out/trx64-$ID"
  rm -f "$D/SHA256SUMS"

  # ---- our apps
  echo "=== apps"
  command -v nios2-elf-gcc >/dev/null || echo "WARNING: nios2-elf-gcc not on PATH"
  # shellcheck disable=SC2206
  built=(); skipped=($EXCLUDE_APPS)
  export TRXMON_BUILD="$(git -C "$APP" rev-parse --short HEAD)"
  for dir in "$APP"/apps/*/; do
    n="$(basename "$dir")"
    [ -f "$dir/Makefile" ] || continue
    case " $EXCLUDE_APPS " in *" $n "*) continue ;; esac
    echo "--- $n"
    ( cd "$dir" && make clean >/dev/null 2>&1 || true; make 2>&1 | tail -4 )
    [ -s "$dir/$n.u2a" ] || { echo "$n.u2a was not built" >&2; exit 1; }
    cp "$dir/$n.u2a" "$D/$n.u2a"
    ( cd "$dir" && make clean >/dev/null 2>&1 || true )
    built+=("$n")
  done
  for s in "$APP"/software/appsdk/samples/*/; do [ -f "$s/Makefile" ] && skipped+=("$(basename "$s")"); done
  [ "${#built[@]}" -gt 0 ] || { echo "no app built" >&2; exit 1; }

  # ---- notes
  echo "=== notes"
  RUN_URL=""
  [ -z "${GITHUB_RUN_NUMBER:-}" ] || RUN_URL="$SERVER/$REPO/actions/runs/$GITHUB_RUN_NUMBER"
  python3 "$ROOT/tools/release_notes.py" --id "$ID" --date "$(TZ=Europe/Berlin date +%F)" --repo "$ROOT" --ref "$SHA" \
    --manifest "$W/in/manifest.json" --reports "$W/in" --run-url "$RUN_URL" \
    --apps "$(IFS=,; echo "${built[*]}")" --excluded "$(IFS=,; echo "${skipped[*]}")" \
    -o "$D/RELEASE_NOTES.md"

  # ---- sums, check, zip
  ( cd "$D" && sha256sum -- * > SHA256SUMS && cat SHA256SUMS )
  python3 "$ROOT/tools/trx_pkg_check.py" --bitstream "$W/in/c64_basic.bit" --app "$W/in/ultimate.app" \
    "$D"/trx64_update_*.cfw "$D"/trx64_update_*.ue2
  python3 - "$D" "$W/trx64-$ID.zip" "trx64-$ID" <<'PY'
import os, sys, zipfile
d, out, top = sys.argv[1:]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for f in sorted(os.listdir(d)):
        zi = zipfile.ZipInfo("%s/%s" % (top, f), date_time=(2026, 1, 1, 0, 0, 0))
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.external_attr = 0o644 << 16
        with open(os.path.join(d, f), "rb") as fh:
            z.writestr(zi, fh.read())
PY
  cp "$W/trx64-$ID.zip" "$D/"
  ls -l "$D"
  echo "ID=$ID" >> "$ENVF"

  [ -z "${REL_NO_PUBLISH:-}" ] || { echo "REL_NO_PUBLISH: done, nothing published ($D)"; exit 0; }

  # ---- publish: generic package trx64-release/<id>
  RAPI="$PKG_API/trx64-release/$ID"
  pcurl -o /dev/null -X DELETE "$RAPI" || true
  for f in "$D"/*; do
    pcurl -X PUT --upload-file "$f" "$RAPI/$(basename "$f")" >/dev/null
  done
  echo "published generic package trx64-release/$ID"

  # ---- publish: Gitea Release (a tag run: named after the tag; else a prerelease trx64-<id>)
  if [ "${GITHUB_REF_TYPE:-}" = tag ] && [ "$REF" = "${GITHUB_REF_NAME:-}" ]; then
    TAG="$REF"; NAME="$REF"; PRE=false
  else
    TAG="trx64-$ID"; NAME="trx64-$ID"; PRE=true
  fi
  API="$SERVER/api/v1/repos/$REPO"
  # api <method> <url> [curl args]: REL_TOKEN first, PACKAGE_TOKEN when that one is refused
  api() {
    local m="$1" u="$2" t out; shift 2
    for t in "${REL_TOKEN:-}" "${PACKAGE_TOKEN:-}"; do
      [ -n "$t" ] || continue
      out="$(curl -s -o "$W/api.out" -w '%{http_code}' -X "$m" -H "Authorization: token $t" "$@" "$u")" || true
      case "$out" in 401|403) continue ;; esac
      [ "$out" -lt 300 ] || { echo "$m $u: HTTP $out" >&2; head -c 400 "$W/api.out" >&2; echo >&2; return 1; }
      return 0
    done
    echo "$m $u: refused with every token" >&2; return 1
  }
  # a release of the same tag from an earlier run goes first
  if api GET "$API/releases/tags/$TAG" 2>/dev/null; then
    old="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$W/api.out")"
    api DELETE "$API/releases/$old"
  fi
  python3 - "$TAG" "$NAME" "$PRE" "$SHA" "$D/RELEASE_NOTES.md" > "$W/release.json" <<'PY'
import json, sys
tag, name, pre, sha, notes = sys.argv[1:]
json.dump({"tag_name": tag, "target_commitish": sha, "name": name, "prerelease": pre == "true",
           "body": open(notes, encoding="utf-8").read()}, sys.stdout)
PY
  api POST "$API/releases" -H 'Content-Type: application/json' --data-binary "@$W/release.json"
  rid="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$W/api.out")"
  for f in "$D"/*; do
    api POST "$API/releases/$rid/assets?name=$(basename "$f")" -F "attachment=@$f"
  done
  echo "=== Release $NAME (tag $TAG, prerelease=$PRE): $SERVER/$REPO/releases/tag/$TAG"
  echo "=== package: $SERVER/$OWNER/-/packages/generic/trx64-release/$ID"
  # the hand-over directory of this run has served its purpose
  rm -rf "${HANDOFF:?}" "$ENVF"
  ;;
*) echo "unknown command $cmd" >&2; exit 2 ;;
esac
