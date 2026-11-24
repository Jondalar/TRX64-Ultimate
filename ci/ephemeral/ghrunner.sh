#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# U20 F-a (D17): the GitHub side of an ephemeral runner, through the REST API with curl and jq. Token: $RUNNER_REG_TOKEN,
# a fine-grained personal access token on this one repository with "Administration: Read and write".
#
#   ghrunner.sh jit --repo OWNER/NAME --name RUNNER --label L [--label L ...] --out FILE
#       POST /repos/{repo}/actions/runners/generate-jitconfig, runner group 1 (the repository's default): a runner that
#       takes exactly one job and then deregisters. The encoded config goes to FILE (mode 600) and nowhere else; in
#       GitHub Actions it is masked before anything else happens. Prints runner_id, runner_name, runner_labels.
#       Label rule (public repository): a label a job can name without knowing it is refused (self-hosted and GitHub's
#       OS and architecture names), and the answer's label list must be exactly the requested one, else the config is
#       not written and the call fails. A fork pull request can bring its own workflow whose job waits for
#       `runs-on: self-hosted`; such a job never matches this runner. The runner adds no label of its own on the
#       --jitconfig path (actions/runner 2.338.2: config.sh's ConfigurationManager.CreateNewAgent adds self-hosted, OS and
#       architecture; Runner.cs only writes the decoded config files); GitHub's documentation does not say what its
#       server adds, so the answer is checked.
#   ghrunner.sh wait-online --repo R --id ID [--timeout SEC]
#       until GitHub lists the runner online (default 600 s); prints t_online. 404 (the runner is gone) is a failure.
#   ghrunner.sh delete --repo R (--id ID | --name NAME)
#       removes the registration; 404 means it deregistered itself (a JIT runner does after its job): fine.
#   ghrunner.sh user-data --jit-file FILE --label L --cap-minutes N --out FILE
#       ci/ephemeral/user-data.sh with the JIT config, the label and the lifetime cap filled in; sends nothing.
#
#   Everywhere: --dry-run and --outputs FILE (ci/ephemeral/common.sh). GITHUB_API_URL overrides https://api.github.com.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ci/ephemeral/common.sh
source "$HERE/common.sh"

GH_API=${GITHUB_API_URL:-https://api.github.com}
GH_HEADERS=("Accept: application/vnd.github+json" "X-GitHub-Api-Version: 2026-03-10")
USER_DATA_MAX=32768
FAKE_JIT=RkFLRS1KSVQtQ09ORklHLWRyeS1ydW4tb25seQ==   # base64 of FAKE-JIT-CONFIG-dry-run-only

gh() { eph_http "$1" "$GH_API$2" RUNNER_REG_TOKEN "$3" "$4" "${GH_HEADERS[@]}"; }

repo_ok() { [[ "$1" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || die "--repo wants OWNER/NAME, not '$1'"; }
label_ok() { [[ "$1" =~ ^[A-Za-z0-9._-]{1,100}$ ]] || die "runner label or name '$1': letters, digits, . _ - only"; }
jit_ok() { [[ "$1" =~ ^[A-Za-z0-9+/=]+$ ]] || die "the JIT config is not base64"; }
int_arg() { [[ "$2" =~ ^[0-9]+$ ]] || die "$1 needs a whole number, not '$2'"; }

cmd_jit() {
  local repo="" name="" out="" labels=() body="$EPH_TMP/jit-req.json" resp="$EPH_TMP/jit.json" st jit l
  while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || die "$1 needs a value"
    case "$1" in
      --repo) repo=$2 ;;
      --name) name=$2 ;;
      --label) labels+=("$2") ;;
      --out) out=$2 ;;
      *) die "jit: unknown option $1" ;;
    esac
    shift 2
  done
  repo_ok "$repo"; label_ok "$name"
  [ ${#labels[@]} -gt 0 ] || die "jit needs at least one --label"
  for l in "${labels[@]}"; do
    label_ok "$l"
    case "$(printf '%s' "$l" | tr '[:upper:]' '[:lower:]')" in
      self-hosted | linux | windows | macos | x64 | x86 | arm | arm64)
        die "label '$l' is one a job can name without knowing it; this runner carries only its own random label" ;;
    esac
  done
  [ -n "$out" ] || die "jit needs --out FILE"
  jq -n --arg name "$name" '{name: $name, runner_group_id: 1, labels: $ARGS.positional, work_folder: "_work"}' \
    --args "${labels[@]}" > "$body"
  st=$(gh POST "/repos/$repo/actions/runners/generate-jitconfig" "$body" "$resp")
  [ "$st" = 201 ] || die "generate-jitconfig for $name: HTTP $st $(eph_err "$resp")"
  jit=$(jq -r '.encoded_jit_config // empty' "$resp")
  if in_actions && [ -n "$jit" ]; then echo "::add-mask::$jit"; fi
  [ -n "$jit" ] || die "generate-jitconfig answered without encoded_jit_config"
  jit_ok "$jit"
  emit runner_id "$(jq -r '.runner.id' "$resp")"
  emit runner_name "$(jq -r '.runner.name' "$resp")"
  emit runner_labels "$(jq -r '[.runner.labels[]?.name] | join(",")' "$resp")"
  if ! jq -e --argjson want "$(jq -n '$ARGS.positional' --args "${labels[@]}")" \
      '[.runner.labels[]?.name] | sort == ($want | sort)' "$resp" > /dev/null; then
    die "GitHub gave runner $name the labels [$(jq -r '[.runner.labels[]?.name] | join(",")' "$resp")], not only the requested [$(IFS=,; echo "${labels[*]}")]: no config written (job stop removes the registration)"
  fi
  (umask 077 && printf '%s' "$jit" > "$out")
  rm -f "$resp"
}

cmd_wait_online() {
  local repo="" id="" timeout=600 t0 st status out="$EPH_TMP/runner.json"
  while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || die "$1 needs a value"
    case "$1" in
      --repo) repo=$2 ;;
      --id) id=$2 ;;
      --timeout) timeout=$2 ;;
      *) die "wait-online: unknown option $1" ;;
    esac
    shift 2
  done
  repo_ok "$repo"; int_arg --id "$id"; int_arg --timeout "$timeout"
  t0=$(date -u +%s)
  while :; do
    st=$(gh GET "/repos/$repo/actions/runners/$id" "" "$out")
    [ "$st" = 200 ] || die "runner $id: HTTP $st $(eph_err "$out")"
    status=$(jq -r '.status' "$out")
    if [ "$status" = online ]; then break; fi
    [ $(($(date -u +%s) - t0)) -lt "$timeout" ] || die "runner $id: $status after $timeout s (the server's user data did not start it)"
    sleep 5
  done
  note "runner $id: online after $(($(date -u +%s) - t0)) s of waiting (busy: $(jq -r '.busy' "$out"))"
  emit t_online "$(date -u +%s)"
}

delete_id() {
  local repo=$1 id=$2 st out="$EPH_TMP/del.json"
  st=$(gh DELETE "/repos/$repo/actions/runners/$id" "" "$out")
  case "$st" in
    204) note "runner $id: registration removed" ;;
    404) note "runner $id: not registered (a JIT runner deregisters after its job)" ;;
    *) note "runner $id: DELETE HTTP $st $(eph_err "$out")"; return 1 ;;
  esac
}

cmd_delete() {
  local repo="" id="" name="" rc=0 st out="$EPH_TMP/runners.json" i
  while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || die "$1 needs a value"
    case "$1" in
      --repo) repo=$2 ;;
      --id) id=$2 ;;
      --name) name=$2 ;;
      *) die "delete: unknown option $1" ;;
    esac
    shift 2
  done
  repo_ok "$repo"
  [ -n "$id" ] || [ -n "$name" ] || die "delete needs --id ID or --name NAME"
  if [ -n "$id" ]; then int_arg --id "$id"; delete_id "$repo" "$id" || rc=1; fi
  if [ -n "$name" ]; then
    label_ok "$name"
    st=$(gh GET "/repos/$repo/actions/runners?name=$(eph_uri "$name")&per_page=100" "" "$out")
    if [ "$st" = 200 ]; then
      for i in $(jq -r --arg n "$name" '.runners[]? | select(.name == $n) | .id' "$out"); do delete_id "$repo" "$i" || rc=1; done
    else
      note "runners named $name: HTTP $st $(eph_err "$out")"
      rc=1
    fi
  fi
  return "$rc"
}

cmd_user_data() {
  local jitf="" label="" cap="" out="" tpl="$HERE/user-data.sh" size
  while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || die "$1 needs a value"
    case "$1" in
      --jit-file) jitf=$2 ;;
      --label) label=$2 ;;
      --cap-minutes) cap=$2 ;;
      --out) out=$2 ;;
      *) die "user-data: unknown option $1" ;;
    esac
    shift 2
  done
  [ -f "$jitf" ] || die "--jit-file '$jitf' is not a file"
  jit_ok "$(cat "$jitf")"
  label_ok "$label"
  int_arg --cap-minutes "$cap"
  [ "$cap" -ge 5 ] && [ "$cap" -le 360 ] || die "--cap-minutes $cap: 5 to 360"
  [ -n "$out" ] || die "user-data needs --out FILE"
  (umask 077 && jq -Rrs --rawfile jit "$jitf" --arg label "$label" --arg cap "$cap" \
    'gsub("@JITCONFIG@"; $jit | rtrimstr("\n")) | gsub("@RUNNER_LABEL@"; $label) | gsub("@CAP_MINUTES@"; $cap)' \
    "$tpl" > "$out")
  awk '/@[A-Z_]+@/ { bad = 1 } END { exit bad }' "$out" || die "user data: a @FIELD@ is left unfilled"
  size=$(wc -c < "$out" | tr -d ' ')
  [ "$size" -le "$USER_DATA_MAX" ] || die "user data has $size bytes, Hetzner takes $USER_DATA_MAX"
  emit user_data_bytes "$size"
}

# ---- dry run: canned answers
fake_response() {
  local m=$1 u=$2 out=$3 body=${4:-} path id
  path=${u#"$GH_API"}
  case "$m $path" in
    "POST /repos/"*"/actions/runners/generate-jitconfig")
      # EPH_FAKE_EXTRA_LABEL: the answer lists one label more (as if GitHub's server added a default label)
      jq --arg jit "$FAKE_JIT" --arg extra "${EPH_FAKE_EXTRA_LABEL:-}" '{runner: {id: 31337, runner_group_id: 1, name: .name,
        os: "unknown", status: "offline", busy: false, ephemeral: true,
        labels: ([.labels[] | {name: ., type: "custom"}] + (if $extra == "" then [] else [{name: $extra, type: "read-only"}] end))},
        encoded_jit_config: $jit}' "$body" > "$out"
      echo 201 ;;
    "GET /repos/"*"/actions/runners?"*)
      jq -n '{total_count: 1, runners: [{id: 31337, name: "gh-eph-dry-run", status: "offline", busy: false}]}' > "$out"
      echo 200 ;;
    "GET /repos/"*"/actions/runners/"*)
      id=${path##*/}
      jq -n --argjson id "$id" '{id: $id, name: "gh-eph-dry-run", status: "online", busy: false}' > "$out"
      echo 200 ;;
    "DELETE /repos/"*"/actions/runners/"*)
      id=${path##*/}
      if fake_is runner "$id"; then echo '{"message": "Not Found"}' > "$out"; echo 404; return 0; fi
      fake_mark runner "$id"
      : > "$out"
      echo 204 ;;
    *) echo '{"message": "Not Found (no canned answer)"}' > "$out"; echo 404 ;;
  esac
}

main() {
  eph_globals "$@"
  set -- ${EPH_ARGS[@]+"${EPH_ARGS[@]}"}
  eph_need_tools
  [ $# -ge 1 ] || die "usage: ghrunner.sh [--dry-run] jit|wait-online|delete|user-data ... (see the header)"
  local cmd=$1
  shift
  case "$cmd" in
    jit) cmd_jit "$@" ;;
    wait-online) cmd_wait_online "$@" ;;
    delete) cmd_delete "$@" ;;
    user-data) cmd_user_data "$@" ;;
    *) die "unknown subcommand $cmd (jit, wait-online, delete, user-data)" ;;
  esac
}

main "$@"
