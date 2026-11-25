#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# U20 F-a (D17): Hetzner Cloud servers of the ephemeral-runner experiment, through the REST API
# (https://api.hetzner.cloud/v1) with curl and jq, no hcloud CLI. Token: $HCLOUD_TOKEN, a project token with Read & Write.
#
#   hcloud.sh create --name NAME --label k=v [--label k=v ...] --user-data FILE
#                    [--types "cpx02 cpx12 cx23 cpx22"] [--locations "nbg1 fsn1"] [--image ubuntu-24.04] [--ssh-key NAME]
#       Takes the first type/location pair that GET /server_types?name= lists as available, x86 and not deprecated, and
#       creates the server there (IPv4 only: github.com has no IPv6). A pair the create refuses as unavailable (412,
#       503, 422 placement_error) passes to the next. Prints server_id, server_name, server_type, location, ipv4,
#       price_hourly (net EUR, the API's price for that location), t_create (epoch of the request).
#   hcloud.sh wait --id ID [--timeout SEC] [--status running|off]
#                                                      until the server has that status (default running, 300 s);
#                                                      prints t_running (t_off)
#   hcloud.sh delete --id ID | --selector SEL          deletes, waits until the API answers 404 for the server, then
#                                                      deletes the Primary IPs it left unassigned; exit 1 if any remains
#   hcloud.sh sweep --older-than MIN [--selector SEL]  deletes every server of SEL created more than MIN minutes ago;
#                                                      SEL defaults to purpose=gh-ephemeral and must contain that term.
#                                                      Snapshots and SSH keys are never swept.
#   hcloud.sh list [--selector SEL]                    the servers (id, name, status, type, location, age in minutes,
#                                                      run label), the snapshots and the SSH keys of SEL
#
# U20 F-b (D25): the private snapshot with Vivado, and the one-time SSH key of the machine that makes it
# (ci/ephemeral/snapshot.sh):
#   hcloud.sh snapshot --id ID --description TEXT --label k=v [--label k=v ...] [--timeout SEC]
#       creates a snapshot of the server (it should be off) and waits until it is available (default 3600 s); prints
#       image_id, image_size (GB as Hetzner stores and bills it), disk_size (GB: the smallest server disk it fits)
#   hcloud.sh image --selector SEL                     the newest available snapshot of SEL: image_id, image_size,
#                                                      disk_size, created; exit 1 if there is none
#   hcloud.sh delete-image --id ID | --selector SEL [--keep-newest]
#                                                      deletes snapshots (all of SEL, or all but the newest); SEL must
#                                                      contain purpose=gh-ephemeral
#   hcloud.sh ssh-key-add --name NAME --public-key FILE --label k=v [--label k=v ...]    prints ssh_key_id
#   hcloud.sh ssh-key-delete --id ID
#
# Every snapshot and SSH key of this script carries purpose=gh-ephemeral, as its servers do.
#
#   Everywhere: --dry-run (print each request, answer it with canned data, send nothing; no token needed) and
#   --outputs FILE (append the result lines to FILE as well). HCLOUD_API overrides the base URL.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ci/ephemeral/common.sh
source "$HERE/common.sh"

HCLOUD_API=${HCLOUD_API:-https://api.hetzner.cloud/v1}
USER_DATA_MAX=32768   # the API's limit for user_data: 32 KiB

hc() { eph_http "$1" "$HCLOUD_API$2" HCLOUD_TOKEN "$3" "$4"; }

# RFC 3339 to epoch seconds: "Z", "+00:00" and fractions; anything else stops jq (a sweep then deletes nothing)
# shellcheck disable=SC2016  # jq code: its $names are jq's
JQ_EPOCH='def epoch: . as $t | (.[0:19] + "Z" | fromdateiso8601) as $b | (.[19:] | sub("^[.][0-9]+"; "")) as $z
  | if $z == "Z" or $z == "" then $b
    else (($z | capture("^(?<s>[+-])(?<h>[0-9]{2}):?(?<m>[0-9]{2})$")) // error("timestamp " + $t)) as $o
      | (($o.h | tonumber) * 3600 + ($o.m | tonumber) * 60) as $off
      | if $o.s == "+" then $b - $off else $b + $off end end;'

# label key and value as the API accepts them (no key prefix used here); a value with "/" is refused, so repo=owner/name
# must be passed as owner.name
LABEL_RE='^[A-Za-z0-9]([A-Za-z0-9._-]{0,61}[A-Za-z0-9])?$'
valid_label() {
  [[ "$1" =~ $LABEL_RE ]] || die "label key '$1' is not valid for the Hetzner API"
  [ -z "$2" ] || [[ "$2" =~ $LABEL_RE ]] || die "label value '$2' (key $1) is not valid for the Hetzner API"
}

int_arg() { [[ "$2" =~ ^[0-9]+$ ]] || die "$1 needs a whole number, not '$2'"; }

# every server of a label selector, all pages, as one JSON array in $EPH_TMP/servers.json
list_servers() {
  local sel=$1 page=1 st out="$EPH_TMP/page.json" acc="$EPH_TMP/servers.json"
  echo '[]' > "$acc"
  while :; do
    st=$(hc GET "/servers?label_selector=$(eph_uri "$sel")&per_page=50&page=$page" "" "$out")
    [ "$st" = 200 ] || die "list servers [$sel]: HTTP $st $(eph_err "$out")"
    jq -s '.[0] + (.[1].servers // [])' "$acc" "$out" > "$acc.n"
    mv "$acc.n" "$acc"
    page=$(jq -r '.meta.pagination.next_page // empty' "$out")
    [ -n "$page" ] || break
  done
}

# a Primary IP the deleted server leaves behind and nothing else holds; one assigned elsewhere stays
delete_left_ip() {
  local id=$1 st out="$EPH_TMP/ip.json" ip
  st=$(hc GET "/primary_ips/$id" "" "$out")
  case "$st" in
    404) note "  primary IP $id: gone with the server"; return 0 ;;
    200) ;;
    *) note "  primary IP $id: GET HTTP $st $(eph_err "$out")"; return 1 ;;
  esac
  ip=$(jq -r '.primary_ip.ip' "$out")
  if [ "$(jq -r '.primary_ip.assignee_id // "none"' "$out")" != none ]; then
    note "  primary IP $id ($ip): assigned to $(jq -r '"\(.primary_ip.assignee_type) \(.primary_ip.assignee_id)"' "$out"), left alone"
    return 0
  fi
  st=$(hc DELETE "/primary_ips/$id" "" "$out")
  case "$st" in
    200 | 204 | 404) note "  primary IP $id ($ip): deleted"; return 0 ;;
  esac
  note "  primary IP $id ($ip): DELETE HTTP $st $(eph_err "$out")"
  return 1
}

# one server: delete, wait until the API no longer knows it, then its unassigned Primary IPs
delete_server() {
  local id=$1 st out="$EPH_TMP/srv.json" ips ip name i=0
  st=$(hc GET "/servers/$id" "" "$out")
  case "$st" in
    404) note "server $id: not there (deleted before)"; return 0 ;;
    200) ;;
    *) note "server $id: GET HTTP $st $(eph_err "$out")"; return 1 ;;
  esac
  name=$(jq -r '.server.name' "$out")
  ips=$(jq -r '.server.public_net | [.ipv4.id?, .ipv6.id?] | map(select(. != null)) | .[]' "$out")
  st=$(hc DELETE "/servers/$id" "" "$out")
  case "$st" in
    200 | 201 | 204) ;;
    404) note "server $id ($name): gone before the delete" ;;
    *) note "server $id ($name): DELETE HTTP $st $(eph_err "$out")"; return 1 ;;
  esac
  while :; do
    st=$(hc GET "/servers/$id" "" "$out")
    [ "$st" != 404 ] || break
    i=$((i + 1))
    if [ "$i" -ge 40 ]; then
      note "server $id ($name): still there 120 s after the delete (HTTP $st, status $(jq -r '.server.status // "?"' "$out" 2>/dev/null || echo '?'))"
      return 1
    fi
    sleep 3
  done
  note "server $id ($name): deleted"
  for ip in $ips; do delete_left_ip "$ip" || return 1; done
}

cmd_create() {
  local name="" ud="" types="cpx02 cpx12 cx23 cpx22" locs="nbg1 fsn1" image="ubuntu-24.04" sshkey="" labels='{}'
  local k v t loc ok price cands="" c rest st size t_create pw
  local out="$EPH_TMP/types.json" body="$EPH_TMP/create.json" resp="$EPH_TMP/created.json"
  while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || die "$1 needs a value"
    case "$1" in
      --name) name=$2 ;;
      --label)
        k=${2%%=*}; v=${2#*=}
        [ "$k" != "$2" ] || die "--label wants key=value, not '$2'"
        valid_label "$k" "$v"
        labels=$(jq -c --arg k "$k" --arg v "$v" '. + {($k): $v}' <<< "$labels") ;;
      --user-data) ud=$2 ;;
      --types) types=$2 ;;
      --locations) locs=$2 ;;
      --image) image=$2 ;;
      --ssh-key) sshkey=$2 ;;
      *) die "create: unknown option $1" ;;
    esac
    shift 2
  done
  [[ "$name" =~ ^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$ ]] || die "--name '$name' is not a host name (RFC 1123, lower case)"
  [ -f "$ud" ] || die "--user-data '$ud' is not a file"
  size=$(wc -c < "$ud" | tr -d ' ')
  [ "$size" -le "$USER_DATA_MAX" ] || die "user data has $size bytes, the API takes $USER_DATA_MAX"
  [ "$(jq -r '.purpose // ""' <<< "$labels")" = gh-ephemeral ] || die "a server of this script carries --label purpose=gh-ephemeral"

  for t in $types; do
    st=$(hc GET "/server_types?name=$(eph_uri "$t")" "" "$out")
    [ "$st" = 200 ] || die "server type $t: HTTP $st $(eph_err "$out")"
    if [ "$(jq '.server_types | length' "$out")" = 0 ]; then note "server type $t: the API does not know it"; continue; fi
    if [ "$(jq -r '.server_types[0].architecture' "$out")" != x86 ]; then note "server type $t: not x86, skipped (the runner is linux-x64)"; continue; fi
    for loc in $locs; do
      ok=$(jq -r --arg l "$loc" '[.server_types[0].locations[]? | select(.name == $l and .available == true and .deprecation == null)] | length' "$out")
      price=$(jq -r --arg l "$loc" '[.server_types[0].prices[]? | select(.location == $l) | .price_hourly.net][0] // "?"' "$out")
      if [ "$ok" = 1 ]; then
        note "candidate: $t in $loc, EUR $price per hour (net)"
        cands="$cands $t@$loc@$price"
      else
        note "$t in $loc: not offered there, or not available now"
      fi
    done
  done
  [ -n "$cands" ] || die "none of [$types] is available in [$locs]"

  st=""
  for c in $cands; do
    t=${c%%@*}; rest=${c#*@}; loc=${rest%%@*}; price=${rest#*@}
    jq -n --arg name "$name" --arg t "$t" --arg loc "$loc" --arg image "$image" --argjson labels "$labels" \
      --rawfile ud "$ud" --arg key "$sshkey" \
      '{name: $name, server_type: $t, location: $loc, image: $image, labels: $labels, user_data: $ud,
        start_after_create: true, public_net: {enable_ipv4: true, enable_ipv6: false}}
       + (if $key == "" then {} else {ssh_keys: [$key]} end)' > "$body"
    t_create=$(date -u +%s)
    st=$(hc POST /servers "$body" "$resp")
    if [ "$st" = 201 ]; then break; fi
    if [ "$st" = 412 ] || [ "$st" = 503 ] || [ "$(jq -r '.error.code // ""' "$resp" 2>/dev/null || true)" = placement_error ]; then
      note "$t in $loc: HTTP $st $(eph_err "$resp"); next candidate"
      continue
    fi
    die "create $name ($t in $loc): HTTP $st $(eph_err "$resp")"
  done
  [ "$st" = 201 ] || die "create $name: no candidate could be created"
  # the answer carries the root password (no SSH key given); it is never printed, and masked in case
  pw=$(jq -r '.root_password // empty' "$resp")
  if in_actions && [ -n "$pw" ]; then echo "::add-mask::$pw"; fi
  emit server_id "$(jq -r '.server.id' "$resp")"
  emit server_name "$(jq -r '.server.name' "$resp")"
  emit server_type "$t"
  emit location "$loc"
  emit ipv4 "$(jq -r '.server.public_net.ipv4.ip // "-"' "$resp")"
  emit price_hourly "$price"
  emit created "$(jq -r '.server.created' "$resp")"
  emit t_create "$t_create"
  rm -f "$body" "$resp"
}

cmd_wait() {
  local id="" timeout=300 want=running t0 st status out="$EPH_TMP/srv.json"
  while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || die "$1 needs a value"
    case "$1" in
      --id) id=$2 ;;
      --timeout) timeout=$2 ;;
      --status) want=$2 ;;
      *) die "wait: unknown option $1" ;;
    esac
    shift 2
  done
  int_arg --id "$id"; int_arg --timeout "$timeout"
  case "$want" in running | off) ;; *) die "wait: --status is running or off, not '$want'" ;; esac
  t0=$(date -u +%s)
  while :; do
    st=$(hc GET "/servers/$id" "" "$out")
    [ "$st" = 200 ] || die "server $id: HTTP $st $(eph_err "$out")"
    status=$(jq -r '.server.status' "$out")
    if [ "$status" = "$want" ]; then break; fi
    [ $(($(date -u +%s) - t0)) -lt "$timeout" ] || die "server $id: status $status after $timeout s"
    sleep 5
  done
  note "server $id: $want after $(($(date -u +%s) - t0)) s of waiting"
  emit "t_$want" "$(date -u +%s)"
}

# labels from --label k=v options into a JSON object; purpose=gh-ephemeral must be among them
labels_json() {
  local labels='{}' k v
  for kv in "$@"; do
    k=${kv%%=*}; v=${kv#*=}
    [ "$k" != "$kv" ] || die "--label wants key=value, not '$kv'"
    valid_label "$k" "$v"
    labels=$(jq -c --arg k "$k" --arg v "$v" '. + {($k): $v}' <<< "$labels")
  done
  [ "$(jq -r '.purpose // ""' <<< "$labels")" = gh-ephemeral ] || die "everything this script makes carries --label purpose=gh-ephemeral"
  echo "$labels"
}

# a selector that reaches only what this experiment labelled
need_purpose() {
  case ",${1// /}," in
    *,purpose=gh-ephemeral,* | *,purpose==gh-ephemeral,*) ;;
    *) die "the selector must contain the term purpose=gh-ephemeral, not [$1]" ;;
  esac
}

cmd_snapshot() {
  local id="" desc="" timeout=3600 lbl=() labels st out="$EPH_TMP/img.json" body="$EPH_TMP/snap.json" img t0 status
  while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || die "$1 needs a value"
    case "$1" in
      --id) id=$2 ;;
      --description) desc=$2 ;;
      --label) lbl+=("$2") ;;
      --timeout) timeout=$2 ;;
      *) die "snapshot: unknown option $1" ;;
    esac
    shift 2
  done
  int_arg --id "$id"; int_arg --timeout "$timeout"
  [ -n "$desc" ] || die "snapshot needs --description"
  labels=$(labels_json ${lbl[@]+"${lbl[@]}"})
  jq -n --arg d "$desc" --argjson l "$labels" '{type: "snapshot", description: $d, labels: $l}' > "$body"
  st=$(hc POST "/servers/$id/actions/create_image" "$body" "$out")
  [ "$st" = 201 ] || die "snapshot of server $id: HTTP $st $(eph_err "$out")"
  img=$(jq -r '.image.id' "$out")
  note "snapshot $img of server $id: requested"
  t0=$(date -u +%s)
  while :; do
    st=$(hc GET "/images/$img" "" "$out")
    [ "$st" = 200 ] || die "snapshot $img: HTTP $st $(eph_err "$out")"
    status=$(jq -r '.image.status' "$out")
    if [ "$status" = available ]; then break; fi
    [ "$status" = creating ] || die "snapshot $img: status $status"
    [ $(($(date -u +%s) - t0)) -lt "$timeout" ] || die "snapshot $img: still $status after $timeout s"
    sleep 10
  done
  note "snapshot $img: available after $(($(date -u +%s) - t0)) s"
  emit image_id "$img"
  emit image_size "$(jq -r '.image.image_size // "?"' "$out")"
  emit disk_size "$(jq -r '.image.disk_size // "?"' "$out")"
}

# every snapshot of a label selector, newest first, as one JSON array in $EPH_TMP/images.json
list_images() {
  local sel=$1 page=1 st out="$EPH_TMP/page.json" acc="$EPH_TMP/images.json"
  echo '[]' > "$acc"
  while :; do
    st=$(hc GET "/images?type=snapshot&label_selector=$(eph_uri "$sel")&sort=created:desc&per_page=50&page=$page" "" "$out")
    [ "$st" = 200 ] || die "list snapshots [$sel]: HTTP $st $(eph_err "$out")"
    jq -s '.[0] + (.[1].images // [])' "$acc" "$out" > "$acc.n"
    mv "$acc.n" "$acc"
    page=$(jq -r '.meta.pagination.next_page // empty' "$out")
    [ -n "$page" ] || break
  done
  jq 'sort_by(.created) | reverse' "$acc" > "$acc.n"
  mv "$acc.n" "$acc"
}

cmd_image() {
  local sel="" n
  while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || die "$1 needs a value"
    case "$1" in
      --selector) sel=$2 ;;
      *) die "image: unknown option $1" ;;
    esac
    shift 2
  done
  need_purpose "$sel"
  list_images "$sel"
  n=$(jq '[.[] | select(.status == "available")] | length' "$EPH_TMP/images.json")
  [ "$n" -gt 0 ] || die "no available snapshot of [$sel]"
  [ "$n" = 1 ] || note "snapshots of [$sel]: $n available, the newest is taken"
  jq -r '[.[] | select(.status == "available")][0] | "image_id=\(.id)\nimage_size=\(.image_size // "?")\ndisk_size=\(.disk_size // "?")\ncreated=\(.created)"' \
    "$EPH_TMP/images.json" | while IFS='=' read -r k v; do emit "$k" "$v"; done
}

cmd_delete_image() {
  local id="" sel="" keep=0 ids i st out="$EPH_TMP/del.json" rc=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --id) [ $# -ge 2 ] || die "--id needs a value"; id=$2; shift ;;
      --selector) [ $# -ge 2 ] || die "--selector needs a value"; sel=$2; shift ;;
      --keep-newest) keep=1 ;;
      *) die "delete-image: unknown option $1" ;;
    esac
    shift
  done
  if [ -n "$id" ]; then
    int_arg --id "$id"
    # a snapshot named by id is deleted only when it carries this experiment's label
    st=$(hc GET "/images/$id" "" "$out")
    case "$st" in
      404) note "snapshot $id: not there"; return 0 ;;
      200) ;;
      *) die "snapshot $id: HTTP $st $(eph_err "$out")" ;;
    esac
    [ "$(jq -r '.image.labels.purpose // ""' "$out")" = gh-ephemeral ] || die "snapshot $id does not carry purpose=gh-ephemeral"
    ids=$id
  else
    [ -n "$sel" ] || die "delete-image needs --id ID or --selector SEL"
    need_purpose "$sel"
    list_images "$sel"
    ids=$(jq -r --argjson k "$keep" '.[$k:] | .[].id' "$EPH_TMP/images.json")
  fi
  for i in $ids; do
    st=$(hc DELETE "/images/$i" "" "$out")
    case "$st" in
      200 | 204 | 404) note "snapshot $i: deleted" ;;
      *) note "snapshot $i: DELETE HTTP $st $(eph_err "$out")"; rc=1 ;;
    esac
  done
  return "$rc"
}

cmd_ssh_key_add() {
  local name="" pub="" lbl=() labels st out="$EPH_TMP/key.json" body="$EPH_TMP/keybody.json"
  while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || die "$1 needs a value"
    case "$1" in
      --name) name=$2 ;;
      --public-key) pub=$2 ;;
      --label) lbl+=("$2") ;;
      *) die "ssh-key-add: unknown option $1" ;;
    esac
    shift 2
  done
  [ -n "$name" ] || die "ssh-key-add needs --name"
  [ -f "$pub" ] || die "--public-key '$pub' is not a file"
  labels=$(labels_json ${lbl[@]+"${lbl[@]}"})
  jq -n --arg n "$name" --rawfile k "$pub" --argjson l "$labels" '{name: $n, public_key: ($k | rtrimstr("\n")), labels: $l}' > "$body"
  st=$(hc POST /ssh_keys "$body" "$out")
  [ "$st" = 201 ] || die "ssh key $name: HTTP $st $(eph_err "$out")"
  emit ssh_key_id "$(jq -r '.ssh_key.id' "$out")"
}

cmd_ssh_key_delete() {
  local id="" st out="$EPH_TMP/key.json"
  while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || die "$1 needs a value"
    case "$1" in
      --id) id=$2 ;;
      *) die "ssh-key-delete: unknown option $1" ;;
    esac
    shift 2
  done
  int_arg --id "$id"
  st=$(hc DELETE "/ssh_keys/$id" "" "$out")
  case "$st" in
    200 | 204 | 404) note "ssh key $id: deleted" ;;
    *) die "ssh key $id: DELETE HTTP $st $(eph_err "$out")" ;;
  esac
}

cmd_delete() {
  local id="" sel="" rc=0 s left
  while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || die "$1 needs a value"
    case "$1" in
      --id) id=$2 ;;
      --selector) sel=$2 ;;
      *) die "delete: unknown option $1" ;;
    esac
    shift 2
  done
  [ -n "$id" ] || [ -n "$sel" ] || die "delete needs --id ID or --selector SEL"
  if [ -n "$id" ]; then
    int_arg --id "$id"
    delete_server "$id" || rc=1
  fi
  if [ -n "$sel" ]; then
    list_servers "$sel"
    note "selector [$sel]: $(jq length "$EPH_TMP/servers.json") server(s)"
    for s in $(jq -r '.[].id' "$EPH_TMP/servers.json"); do delete_server "$s" || rc=1; done
    list_servers "$sel"
    left=$(jq -r '[.[] | "\(.id) \(.name) \(.status)"] | join(", ")' "$EPH_TMP/servers.json")
    if [ -n "$left" ]; then note "selector [$sel]: still there: $left"; rc=1; fi
  fi
  return "$rc"
}

cmd_sweep() {
  local older="" sel="purpose=gh-ephemeral" cut ids s rc=0 n kept
  while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || die "$1 needs a value"
    case "$1" in
      --older-than) older=$2 ;;
      --selector) sel=$2 ;;
      *) die "sweep: unknown option $1" ;;
    esac
    shift 2
  done
  int_arg --older-than "$older"
  # a sweep never reaches a server this experiment did not label
  case ",${sel// /}," in
    *,purpose=gh-ephemeral,* | *,purpose==gh-ephemeral,*) ;;
    *) die "a sweep selector must contain the term purpose=gh-ephemeral, not [$sel]" ;;
  esac
  cut=$(($(date -u +%s) - older * 60))
  list_servers "$sel"
  ids=$(jq -r --argjson cut "$cut" "$JQ_EPOCH"' [.[] | select((.created | epoch) < $cut) | .id] | .[]' "$EPH_TMP/servers.json")
  kept=$(jq -r --argjson cut "$cut" "$JQ_EPOCH"' [.[] | select((.created | epoch) >= $cut)] | length' "$EPH_TMP/servers.json")
  n=0
  for s in $ids; do n=$((n + 1)); delete_server "$s" || rc=1; done
  note "sweep [$sel], older than $older min: $n deleted or tried, $kept younger kept"
  emit swept "$n"
  return "$rc"
}

cmd_list() {
  local sel="purpose=gh-ephemeral"
  while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || die "$1 needs a value"
    case "$1" in
      --selector) sel=$2 ;;
      *) die "list: unknown option $1" ;;
    esac
    shift 2
  done
  list_servers "$sel"
  jq -r --argjson now "$(date -u +%s)" "$JQ_EPOCH"'
    (["id", "name", "status", "type", "location", "age_min", "run"] | @tsv),
    (.[] | [.id, .name, .status, .server_type.name, .location.name,
            ((($now - (.created | epoch)) / 60) | floor), (.labels.run // "-")] | @tsv)' "$EPH_TMP/servers.json"
  list_images "$sel"
  jq -r '"", (["snapshot", "status", "image_size_gb", "disk_size_gb", "created", "role"] | @tsv),
    (.[] | [.id, .status, (.image_size // "-"), (.disk_size // "-"), .created, (.labels.role // "-")] | @tsv)' "$EPH_TMP/images.json"
  local st out="$EPH_TMP/keys.json"
  st=$(hc GET "/ssh_keys?label_selector=$(eph_uri "$sel")&per_page=50" "" "$out")
  [ "$st" = 200 ] || die "list ssh keys [$sel]: HTTP $st $(eph_err "$out")"
  jq -r '"", (["ssh_key", "name", "created"] | @tsv), (.ssh_keys[]? | [.id, .name, .created] | @tsv)' "$out"
}

# ---- dry run: canned answers, one invocation's state in $EPH_TMP (deleted ids)

# three servers of earlier runs: 5, 120 and 200 minutes old, with the three timestamp forms the parser takes
fake_servers() {
  jq -n --argjson now "$(date -u +%s)" --argjson gone "$(fake_list server)" '
    def srv($id; $run; $age; $form; $ip):
      {id: $id, name: "gh-eph-\($run)", status: (if $age > 100 then "off" else "running" end),
       created: (($now - $age * 60) | todate | if $form == "z" then . elif $form == "frac" then sub("Z$"; ".123456Z")
                 else sub("Z$"; "+00:00") end),
       server_type: {name: "cpx02"}, location: {name: "fsn1"},
       labels: {purpose: "gh-ephemeral", repo: "Example.dry-run", run: $run},
       public_net: {ipv4: {id: $ip, ip: "192.0.2.\($ip - 200000)"}, ipv6: null}};
    [srv(100001; "1000-1"; 5; "z"; 200001), srv(100002; "999-1"; 200; "offset"; 200002),
     srv(100003; "998-2"; 120; "frac"; 200003)] | map(select(.id as $i | $gone | index($i) | not))'
}

# two snapshots of earlier F-b preparations (vivado), 2 and 30 days old
fake_images() {
  jq -n --argjson now "$(date -u +%s)" --argjson gone "$(fake_list image)" '
    def img($id; $days; $size):
      {id: $id, type: "snapshot", status: "available", description: "vivado-env:2024.1 (dry run)",
       created: (($now - $days * 86400) | todate), image_size: $size, disk_size: 80,
       labels: {purpose: "gh-ephemeral", role: "vivado"}};
    [img(300001; 30; 9.5), img(300002; 2; 9.6)] | map(select(.id as $i | $gone | index($i) | not))'
}

# a selector over the canned servers: k=v, k==v, k!=v, k in (a,b), k, !k
# shellcheck disable=SC2016  # jq code: its $names are jq's
JQ_SELECT='
  def term_ok($t; $l):
    if ($t | test("^[^=!() ]+ in [(]")) then
      ($t | capture("^(?<k>[^ ]+) in [(](?<vs>[^)]*)[)]$")) as $c
      | ($c.vs | split(",") | map(gsub("^ +| +$"; "")) | any(.[]; . == $l[$c.k]))
    elif ($t | test("!=")) then ($t | capture("^(?<k>[^!]+)!=(?<v>.*)$")) as $c | $l[$c.k] != $c.v
    elif ($t | test("=")) then ($t | capture("^(?<k>[^=]+)==?(?<v>.*)$")) as $c | $l[$c.k] == $c.v
    elif ($t | startswith("!")) then ($l | has($t[1:]) | not)
    else $l | has($t) end;
  def sel_ok($sel): .labels as $l | all($sel | [scan("[^,()]+ in [(][^)]*[)]|[^,]+")] | .[]; term_ok(.; $l));'

fake_response() {
  local m=$1 u=$2 out=$3 body=${4:-} path id q sel
  path=${u#"$HCLOUD_API"}
  q=${path#*\?}
  case "$m $path" in
    "GET /server_types?"*)
      jq -n --arg n "${q#name=}" '{server_types: (if $n == "cpx02" then [{
        id: 0, name: "cpx02", description: "CPX02 (dry run)", architecture: "x86", cpu_type: "shared", cores: 1, memory: 1, disk: 20,
        locations: [{id: 2, name: "nbg1", available: false, recommended: false, deprecation: null},
                    {id: 1, name: "fsn1", available: true, recommended: true, deprecation: null}],
        prices: [{location: "fsn1", price_hourly: {net: "0.0000", gross: "0.0000"}},
                 {location: "nbg1", price_hourly: {net: "0.0000", gross: "0.0000"}}]}] else [] end)}' > "$out"
      echo 200 ;;
    "POST /servers")
      jq --argjson now "$(date -u +%s)" '{server: {id: 100100, name: .name, status: "initializing", created: ($now | todate),
        server_type: {name: .server_type}, location: {name: .location}, labels: .labels,
        public_net: {ipv4: {id: 200100, ip: "192.0.2.100"}, ipv6: null}},
        action: {id: 1, command: "create_server", status: "running"}, root_password: "FAKE-ROOT-PASSWORD-dry-run"}' "$body" > "$out"
      echo 201 ;;
    "GET /servers?"*)
      sel=${q#label_selector=}; sel=${sel%%&*}
      sel=$(printf '%b' "${sel//\%/\\x}")
      fake_servers | jq --arg sel "$sel" "$JQ_SELECT"' {servers: map(select(sel_ok($sel))),
        meta: {pagination: {page: 1, per_page: 50, previous_page: null, next_page: null, last_page: 1}}}' > "$out"
      echo 200 ;;
    "GET /servers/"*)
      id=${path#/servers/}
      if fake_servers | jq -e --argjson i "$id" 'any(.[]; .id == $i)' > /dev/null; then
        fake_servers | jq --argjson i "$id" '{server: (.[] | select(.id == $i))}' > "$out"
        echo 200
      elif [ "$id" = 100100 ] && ! fake_is server "$id"; then
        jq -n --arg s "${EPH_FAKE_STATUS:-running}" '{server: {id: 100100, name: "gh-eph-dry-run", status: $s,
                public_net: {ipv4: {id: 200100, ip: "192.0.2.100"}, ipv6: null}}}' > "$out"
        echo 200
      else
        echo '{"error": {"code": "not_found", "message": "server not found"}}' > "$out"
        echo 404
      fi ;;
    "DELETE /servers/"*)
      fake_mark server "${path#/servers/}"
      echo '{"action": {"id": 2, "command": "delete_server", "status": "running"}}' > "$out"
      echo 200 ;;
    "GET /primary_ips/"*)
      id=${path#/primary_ips/}
      # 200002 stays behind unassigned (auto_delete off); the others went with their server
      if [ "$id" = 200002 ] && ! fake_is ip "$id"; then
        echo '{"primary_ip": {"id": 200002, "ip": "192.0.2.2", "type": "ipv4", "assignee_id": null, "assignee_type": "server", "auto_delete": false}}' > "$out"
        echo 200
      else
        echo '{"error": {"code": "not_found", "message": "primary_ip not found"}}' > "$out"
        echo 404
      fi ;;
    "DELETE /primary_ips/"*)
      fake_mark ip "${path#/primary_ips/}"
      : > "$out"
      echo 204 ;;
    "POST /servers/"*"/actions/create_image")
      jq '{image: {id: 300100, type: "snapshot", status: "creating", description: .description, labels: .labels,
           image_size: null, disk_size: 80}, action: {id: 3, command: "create_image", status: "running"}}' "$body" > "$out"
      echo 201 ;;
    "GET /images?"*)
      sel=${q#*label_selector=}; sel=${sel%%&*}
      sel=$(printf '%b' "${sel//\%/\\x}")
      fake_images | jq --arg sel "$sel" "$JQ_SELECT"' {images: map(select(sel_ok($sel))),
        meta: {pagination: {page: 1, per_page: 50, previous_page: null, next_page: null, last_page: 1}}}' > "$out"
      echo 200 ;;
    "GET /images/"*)
      id=${path#/images/}
      if fake_images | jq -e --argjson i "$id" 'any(.[]; .id == $i)' > /dev/null; then
        fake_images | jq --argjson i "$id" '{image: (.[] | select(.id == $i))}' > "$out"
        echo 200
      elif [ "$id" = 300100 ] && ! fake_is image "$id"; then
        jq -n '{image: {id: 300100, type: "snapshot", status: "available", image_size: 9.87, disk_size: 80,
                labels: {purpose: "gh-ephemeral", role: "vivado"}}}' > "$out"
        echo 200
      else
        echo '{"error": {"code": "not_found", "message": "image not found"}}' > "$out"
        echo 404
      fi ;;
    "DELETE /images/"*)
      fake_mark image "${path#/images/}"
      : > "$out"
      echo 204 ;;
    "POST /ssh_keys")
      jq '{ssh_key: {id: 400100, name: .name, fingerprint: "00:00", labels: .labels}}' "$body" > "$out"
      echo 201 ;;
    "GET /ssh_keys?"*)
      echo '{"ssh_keys": []}' > "$out"
      echo 200 ;;
    "DELETE /ssh_keys/"*)
      : > "$out"
      echo 204 ;;
    *) echo '{"error": {"code": "not_found", "message": "no canned answer"}}' > "$out"; echo 404 ;;
  esac
}

main() {
  eph_globals "$@"
  set -- ${EPH_ARGS[@]+"${EPH_ARGS[@]}"}
  eph_need_tools
  [ $# -ge 1 ] || die "usage: hcloud.sh [--dry-run] create|wait|delete|sweep|list|snapshot|image|delete-image|ssh-key-add|ssh-key-delete ... (see the header)"
  local cmd=$1
  shift
  case "$cmd" in
    create) cmd_create "$@" ;;
    wait) cmd_wait "$@" ;;
    delete) cmd_delete "$@" ;;
    sweep) cmd_sweep "$@" ;;
    list) cmd_list "$@" ;;
    snapshot) cmd_snapshot "$@" ;;
    image) cmd_image "$@" ;;
    delete-image) cmd_delete_image "$@" ;;
    ssh-key-add) cmd_ssh_key_add "$@" ;;
    ssh-key-delete) cmd_ssh_key_delete "$@" ;;
    *) die "unknown subcommand $cmd (create, wait, delete, sweep, list, snapshot, image, delete-image, ssh-key-add, ssh-key-delete)" ;;
  esac
}

main "$@"
