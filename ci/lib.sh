# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck shell=bash
# Sourced by the stage scripts (T31). Where a run keeps what one stage hands to
# the next: /cache/runs/<run id>/<stage>/ on the NAS runner (the volume the
# Vivado cache already uses), .ci/runs/local/<stage>/ in a checkout. A stage
# that finished writes its directory completely and a file DONE last.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_ID="${CI_RUN_ID:-${GITHUB_RUN_ID:-local}}"
if [ -n "${CI_HANDOFF:-}" ]; then
  HANDOFF="$CI_HANDOFF"
elif [ -d /cache ] && [ -w /cache ]; then
  HANDOFF="/cache/runs/$RUN_ID"
else
  HANDOFF="$ROOT/.ci/runs/$RUN_ID"
fi

# release_reuse_skip <stage>: the release workflow (release.yml, ci/release.sh resolve) found the CI
# package of the commit it releases and left $HANDOFF/release-reuse; the stages it calls then build
# nothing and publish nothing. A normal pipeline never has that file.
release_reuse_skip() {
  if [ -f "$HANDOFF/release-reuse" ]; then
    echo "=== $1 skipped: the release reuses the CI package $(cat "$HANDOFF/release-reuse")"
    exit 0
  fi
}

# stage_dir <stage>: the stage's hand-over directory, emptied.
stage_dir() {
  rm -rf "${HANDOFF:?}/$1"
  mkdir -p "$HANDOFF/$1"
  printf '%s\n' "$HANDOFF/$1"
}

# need_stage <stage>: the directory of a finished earlier stage, or stop.
need_stage() {
  if [ ! -f "$HANDOFF/$1/DONE" ]; then
    echo "stage $1 left nothing in $HANDOFF/$1" >&2
    return 1
  fi
  printf '%s\n' "$HANDOFF/$1"
}

# lap: seconds since the previous lap (or the start of the script).
_lap_t=$SECONDS
lap() { local d=$((SECONDS - _lap_t)); _lap_t=$SECONDS; printf '%dm%02ds' $((d / 60)) $((d % 60)); }

branch_slug() { printf '%s' "${GITHUB_REF_NAME:-$(git -C "$ROOT" rev-parse --abbrev-ref HEAD)}" | tr '/' '_'; }

# tree_key <pathspec...>: 16 hex digits over the content of the tracked files
# under the pathspecs and the free text in $KEY_EXTRA. Only the content counts,
# not commits: a push that leaves these files alone gets the same key.
tree_key() {
  {
    git -C "$ROOT" ls-files -s -- "$@"
    printf '%s\n' "${KEY_EXTRA:-}"
  } | sha256sum | cut -c1-16
}

# files_key <pathspec...>: 16 hex digits over the CONTENT (working tree, so an
# edited checkout counts as well as a clean clone) of the tracked files under the
# pathspecs, plus the commit of every submodule among them. Returns 1, printing
# nothing, when a plain pathspec matches no tracked file: a typo in a dependency
# list must never turn into "depends on nothing". A pathspec that starts with
# ':(exclude)' only narrows the others.
#
# A pathspec of the form <submodule>//<pathspec inside it> counts the CONTENT of
# those files instead of the submodule's commit: the app submodule carries
# Gideon's VHDL beside the application, and a commit that touches the
# application alone must not rerun a bench that reads only his VHDL.
files_key() {
  local spec list names subnames sub inner outer=() subspecs=()
  for spec in "$@"; do
    case "$spec" in
      *//*)
        sub="${spec%%//*}"; inner="${spec#*//}"
        if [ -z "$(git -C "$ROOT/$sub" ls-files -- "$inner" 2>/dev/null | head -1)" ]; then
          echo "files_key: '$spec' matches no tracked file in the submodule" >&2
          return 1
        fi
        subspecs+=("$spec")
        continue ;;
      ':(exclude)'*) outer+=("$spec"); continue ;;
    esac
    if [ -z "$(git -C "$ROOT" ls-files -- "$spec" | head -1)" ]; then
      echo "files_key: '$spec' matches no tracked file" >&2
      return 1
    fi
    outer+=("$spec")
  done
  list="$(mktemp)"; names="$(mktemp)"
  if [ ${#outer[@]} -gt 0 ]; then
    git -C "$ROOT" ls-files -s -- "${outer[@]}" > "$list"
  else
    : > "$list"
  fi
  awk -F'\t' '{ split($1, a, " "); if (a[1] != "160000") print $2 }' "$list" > "$names"
  {
    awk -F'\t' '{ split($1, a, " "); if (a[1] == "160000") print "submodule", a[2], $2 }' "$list"
    (cd "$ROOT" && git hash-object --stdin-paths < "$names") | paste -d' ' - "$names"
    for spec in "${subspecs[@]:-}"; do
      [ -n "$spec" ] || continue
      sub="${spec%%//*}"; inner="${spec#*//}"
      subnames="$(mktemp)"
      git -C "$ROOT/$sub" ls-files -- "$inner" | LC_ALL=C sort > "$subnames"
      (cd "$ROOT/$sub" && git hash-object --stdin-paths < "$subnames") |
        paste -d' ' - "$subnames" | sed "s|^\([0-9a-f]\{40\}\) |\1 $sub/|"
      rm -f "$subnames"
    done
  } | sha256sum | cut -c1-16
  rm -f "$list" "$names"
}

# tool_versions: the first version line of each simulator on PATH, on one line.
tool_versions() {
  { iverilog -V 2>/dev/null | head -1; ghdl --version 2>/dev/null | head -1; verilator --version 2>/dev/null | head -1; } | tr '\n' ' '
}

# gateware_key <part>: what decides a bitstream. The RTL and the build scripts
# (not the benches), the constraints, our copy of Gideon's files in vendor/gideon
# (U20 A1: a renewal that moves them counts), the options that change the
# netlist, the part and the Vivado image. Two files of vendor/gideon are left
# out: VENDOR.md only records the commit the copy was made from (hashing it tied
# the key to the app commit, 2026-10-30, b151154b -> 11265524; MultiCore leaves
# it out too), and software/io/audio holds a bench input only (sid_coeff.c).
#
# The excludes need the '/**': a pathspec without it matches the directory
# entry, not what is under it, so ':(exclude)designs/*/sim' excluded nothing and
# every bench edit rebuilt the bitstream (found 2026-10-30, U5). Check with
#   git ls-files designs ':(exclude)designs/*/sim/**' | grep -c /sim/   -> 0
gateware_key() {
  KEY_EXTRA="$1 vivado-env:2024.1 sid=${TRX_SID:-} slot=${T16_SLOT:-} features=${TRX_FEATURES:-}" tree_key \
    designs constraints cores vendor/gideon tools/vivado_lib.tcl tools/vendor_gideon.sh tools/patches/gideon \
    ':(exclude)designs/*/sim/**' ':(exclude)designs/*/build/**' \
    ':(exclude)vendor/gideon/VENDOR.md' ':(exclude)vendor/gideon/software/io/audio/**'
}

# cache_put <dir> <key-dir>: copy a finished stage directory into the cache
# atomically; keep the 8 newest entries of that kind.
cache_put() {
  local src="$1" dst="$2"
  mkdir -p "$(dirname "$dst")"
  rm -rf "$dst.tmp.$$"
  cp -R "$src" "$dst.tmp.$$"
  rm -rf "$dst"
  mv "$dst.tmp.$$" "$dst"
  # shellcheck disable=SC2012
  ls -1dt "$(dirname "$dst")"/"$(basename "$dst" | sed 's/-[0-9a-f]*$//')"-* 2>/dev/null | tail -n +9 | xargs rm -rf
}

# publish_files <dir> [reset]: every file of <dir> into the generic package
# trx64-ultimate, versions <branch>-<8 digit commit> and <branch>-latest of the
# Gitea registry. A file cannot be replaced in place, so the old one is deleted
# first; with "reset" the whole version goes first. Needs PACKAGE_TOKEN.
publish_files() {
  local dir="$1" reset="${2:-}" owner version f
  if [ -z "${PACKAGE_TOKEN:-}" ]; then
    echo "PACKAGE_TOKEN is not set for this repository; nothing is published"
    return 0
  fi
  owner="${GITHUB_REPOSITORY%%/*}"
  local api="${GITHUB_SERVER_URL}/api/packages/${owner}/generic/trx64-ultimate"
  for version in "$(branch_slug)-$(git -C "$ROOT" rev-parse --short=8 HEAD)" "$(branch_slug)-latest"; do
    [ -z "$reset" ] || curl -s -o /dev/null -X DELETE -H "Authorization: token ${PACKAGE_TOKEN}" "$api/$version" || true
    for f in "$dir"/*; do
      curl -s -o /dev/null -X DELETE -H "Authorization: token ${PACKAGE_TOKEN}" "$api/$version/$(basename "$f")" || true
      curl -sf -X PUT -H "Authorization: token ${PACKAGE_TOKEN}" --upload-file "$f" "$api/$version/$(basename "$f")"
    done
    echo "published trx64-ultimate $version"
  done
}

# heavy_lock: one heavy stage at a time per machine, across all runs. Two pipelines
# can be admitted together (the `gateware` group only sees a run between two of its
# jobs as idle), so the heavy stages take this lock first: the file lives on /cache,
# flock works across the job containers of one host because they share its kernel.
# The builder reaches /cache over NFS; there flock becomes a POSIX lock on the NAS,
# which a NAS-local flock does not see, so NAS and builder each run one heavy stage.
# Held until the calling script exits.
heavy_lock() {
  [ -d /cache ] && [ -w /cache ] && command -v flock >/dev/null || return 0
  exec 9>/cache/heavy.lock
  if ! flock -n 9; then
    echo "waiting for another heavy stage (/cache/heavy.lock)"
    flock 9
  fi
  echo "heavy lock held"
}

# build_update <manifest.json> <c64_basic.bit> <ultimate.app> <outdir>: the tester update files
# (tools/make_update.sh --native -> <outdir>/trx64-<id>/trx64_update_*.cfw/.ue2) from the inputs of
# one CI package. Shared by the stage `update` (ci/update.sh) and the release (ci/release.sh).
# Needs the u64-build-env:ci image. Fetches the app submodule (and its nested ones) when the
# checkout has none, and refuses a submodule whose commit is not the app commit of the manifest.
# The file name part comes from tools/make_update.sh (TRX_UPDATE_NAME replaces it, the hook for
# the release tag); the caller exports it.
build_update() {
  local manifest="$1" bit="$2" app="$3" out="$4" appdir="$ROOT/software/ultimate-app"
  if [ ! -e "$appdir/.git" ]; then
    NESTED="software/lwip software/httpd" "$ROOT/ci/submodule.sh"
  fi
  [ "$(git -C "$appdir" rev-parse HEAD)" = "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["app_commit"])' "$manifest")" ] \
    || { echo "the submodule commit is not the app commit of the package" >&2; return 1; }
  # ESP-IDF's environment brings the `python` the app's makefiles call (as in ci/app.sh)
  if [ -f /opt/esp/esp-idf/export.sh ]; then
    set +u; . /opt/esp/esp-idf/export.sh >/dev/null; set -u
  fi
  "$ROOT/tools/make_update.sh" --native --manifest-json "$manifest" --out "$out" "$bit" "$app"
}
