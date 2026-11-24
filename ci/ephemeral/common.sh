# shellcheck shell=bash
# SPDX-License-Identifier: GPL-3.0-or-later
# U20 F-a (D17): shared by ci/ephemeral/hcloud.sh and ci/ephemeral/ghrunner.sh; sourced, not run.
#
#  - eph_http: one HTTPS request with curl. The token reaches curl on stdin (-K -), never on a command line and never in
#    the log. GET and DELETE are retried on 429, 5xx and no answer; POST never (creating a server is not idempotent).
#  - --dry-run (or EPH_DRY_RUN=1): every request is printed (method, URL, headers with the token left out, the body with
#    user_data shortened to its size) and answered by the calling script's fake_response, so the whole path runs
#    without an account. A dry run needs no token.
#  - --outputs FILE: every result line key=value is also appended to FILE ($GITHUB_OUTPUT in the workflow).
#  - No grep on data: under pipefail a grep without a match ends the script (MultiCore T239.5, bits:vc20 report).
#    jq and awk select instead; they answer nothing without failing.
#
# Needs bash (3.2 is enough: the Mac's), curl and jq.

EPH_DRY_RUN=${EPH_DRY_RUN:-0}
EPH_OUTPUTS=""
EPH_TMP=$(mktemp -d "${TMPDIR:-/tmp}/eph.XXXXXX")
chmod 700 "$EPH_TMP"
trap 'rm -rf "$EPH_TMP"' EXIT

die() { echo "error: $*" >&2; exit 1; }
note() { echo "$*" >&2; }
in_actions() { [ "${GITHUB_ACTIONS:-}" = true ]; }

# a result line: stdout, and the --outputs file when one is given; never a secret
emit() {
  printf '%s=%s\n' "$1" "$2"
  if [ -n "$EPH_OUTPUTS" ]; then printf '%s=%s\n' "$1" "$2" >> "$EPH_OUTPUTS"; fi
}

# take --dry-run and --outputs FILE from anywhere in the arguments; the rest is left in EPH_ARGS
eph_globals() {
  EPH_ARGS=()
  while [ $# -gt 0 ]; do
    case "$1" in
      --dry-run) EPH_DRY_RUN=1 ;;
      --outputs) [ $# -ge 2 ] || die "--outputs needs a file"; EPH_OUTPUTS=$2; shift ;;
      *) EPH_ARGS+=("$1") ;;
    esac
    shift
  done
}

eph_need_tools() {
  local t
  for t in curl jq; do command -v "$t" >/dev/null 2>&1 || die "$t is missing"; done
}

# URL-encode one query value
eph_uri() { jq -rn --arg v "$1" '$v | @uri'; }

# eph_http METHOD URL TOKEN_VAR BODY_FILE OUT_FILE [HEADER ...]
# Writes the answer's body to OUT_FILE and prints the HTTP status ("000": no answer). BODY_FILE may be "".
eph_http() {
  local method=$1 url=$2 tokvar=$3 body=$4 out=$5 h status try=1 tok
  shift 5
  if [ "$EPH_DRY_RUN" = 1 ]; then
    {
      echo "DRY-RUN $method $url"
      echo "    Authorization: Bearer <\$$tokvar, not shown>"
      for h in "$@"; do echo "    $h"; done
      if [ -n "$body" ]; then
        echo "    Content-Type: application/json"
        jq 'if type == "object" and has("user_data")
            then .user_data = "<\(.user_data | length) bytes of cloud-init user data, not shown>" else . end' "$body" |
          sed 's/^/    /'
      fi
    } >&2
    fake_response "$method" "$url" "$out" "$body"
    return 0
  fi
  tok=${!tokvar:-}
  [ -n "$tok" ] || die "$tokvar is not set"
  local data=()
  if [ -n "$body" ]; then data=(--data-binary "@$body" -H "Content-Type: application/json"); fi
  while :; do
    status=$({
      printf 'header = "Authorization: Bearer %s"\n' "$tok"
      for h in "$@"; do printf 'header = "%s"\n' "$h"; done
    } | curl -sS -K - -X "$method" -o "$out" -w '%{http_code}' --connect-timeout 15 --max-time 120 \
          ${data[@]+"${data[@]}"} "$url") || status=000
    case "$method:$status" in
      GET:429 | GET:5?? | GET:000 | DELETE:429 | DELETE:5?? | DELETE:000)
        if [ "$try" -lt 6 ]; then
          note "    $method $url: HTTP $status, again in $((try * 5)) s"
          sleep $((try * 5))
          try=$((try + 1))
          continue
        fi ;;
    esac
    echo "$status"
    return 0
  done
}

# dry-run state of one invocation: fake_mark KIND ID, fake_is KIND ID, fake_list KIND (JSON array of numbers)
fake_mark() { echo "$2" >> "$EPH_TMP/fake-$1"; }
fake_is() { [ -f "$EPH_TMP/fake-$1" ] && awk -v i="$2" '$1 == i { f = 1 } END { exit !f }' "$EPH_TMP/fake-$1"; }
fake_list() { if [ -f "$EPH_TMP/fake-$1" ]; then jq -s . "$EPH_TMP/fake-$1"; else echo '[]'; fi; }

# one line from an error answer of either API (Hetzner: .error.code/.message, GitHub: .message); no body, no secret
eph_err() {
  jq -r '[(.error.code // empty), (.error.message // .message // empty)] | join(": ")' "$1" 2>/dev/null ||
    echo "(answer is not JSON, $(wc -c < "$1" | tr -d ' ') bytes)"
}
