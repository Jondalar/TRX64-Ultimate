#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# U20 F-b (D25): the private Hetzner snapshot the Vivado servers of ci/ephemeral/... start from. Run once, by hand, on a
# machine that reaches both the Hetzner API and the machine holding the image archive (the Vivado image is not public:
# AMD's licence, so it travels from our own copy, never through a registry).
#
#   HCLOUD_TOKEN=... ci/ephemeral/snapshot.sh --source DEST --archive PATH --image-id sha256:<64 hex> [options]
#   ci/ephemeral/snapshot.sh --keychain SERVICE ...   takes the token from the macOS keychain item SERVICE instead
#
#   --source DEST      ssh destination that holds the archive (and the cache); its name stays out of this repository
#   --archive PATH     on DEST: `docker save <image-ref> | zstd --long=27` (no spaces in the path; ~ is DEST's home)
#   --image-id ID      the Docker image ID the load must give (`docker image inspect -f '{{.Id}}' <image-ref>` on DEST)
#   --image-ref REF    default vivado-env:2024.1
#   --cache DIR        on DEST: a Vivado cache directory (TRX_CACHE of tools/vivado_lib.tcl); its ip_* and ooc_* entries
#                      go to /opt/trx-cache on the snapshot. Optional.
#   --type T           the server that makes the snapshot (default cpx22: 80 GB disk, room for the archive's load; a
#                      snapshot fits every server whose disk is at least this one's)
#   --locations "L.."  default "nbg1 fsn1"
#   --replace          after the new snapshot is available, delete the older ones with role=vivado
#   --dry-run          the API calls answered by hcloud.sh's canned data, the ssh steps printed and not run
#
# Steps: a one-time SSH key (made here, uploaded, deleted at the end), a server from ubuntu-24.04 with that key, docker.io
# and zstd from Ubuntu, Docker's classic overlay2 store (Ubuntu's docker.io 29 defaults to the containerd image store, whose
# image ID is the manifest's digest, not the config's as on the builder's Docker 26, and which keeps every layer twice:
# packed and unpacked), the archive streamed DEST -> here -> server into `docker load`, the image ID compared (docker load
# checks every layer against the image's config, so the ID proves the content), `vivado -version` in the image, the cache,
# then: apt lists, cloud-init state, machine-id, SSH host keys and the key's authorized_keys removed, fstrim, power off,
# snapshot (labels purpose=gh-ephemeral, role=vivado), server deleted. On any failure the server and the key are deleted;
# a snapshot that became available stays.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
H="$HERE/hcloud.sh"

src="" archive="" image_id="" image_ref="vivado-env:2024.1" cache="" type="cpx22" locs="nbg1 fsn1" replace=0 dry=0 keychain=""
while [ $# -gt 0 ]; do
  case "$1" in
    --source) src=$2; shift ;;
    --archive) archive=$2; shift ;;
    --image-id) image_id=$2; shift ;;
    --image-ref) image_ref=$2; shift ;;
    --cache) cache=$2; shift ;;
    --type) type=$2; shift ;;
    --locations) locs=$2; shift ;;
    --keychain) keychain=$2; shift ;;
    --replace) replace=1 ;;
    --dry-run) dry=1 ;;
    *) echo "error: unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
die() { echo "error: $*" >&2; exit 1; }
[ -n "$src" ] || die "--source DEST is needed"
[ -n "$archive" ] && [[ "$archive" != *" "* ]] || die "--archive PATH is needed (no spaces)"
[[ "$image_id" =~ ^sha256:[0-9a-f]{64}$ ]] || die "--image-id must be sha256:<64 hex>, not '$image_id'"
[[ "$image_ref" =~ ^[a-z0-9][a-z0-9._/-]*:[A-Za-z0-9._-]+$ ]] || die "--image-ref '$image_ref' is not name:tag"
[ -z "$cache" ] || [[ "$cache" =~ ^[A-Za-z0-9._/~-]+$ ]] || die "--cache '$cache': letters, digits and ._/~- only"
short=${image_id:7:12}

if [ "$dry" = 1 ]; then
  export EPH_DRY_RUN=1
elif [ -z "${HCLOUD_TOKEN:-}" ]; then
  [ -n "$keychain" ] || die "HCLOUD_TOKEN is not set (or give --keychain SERVICE)"
  HCLOUD_TOKEN=$(security find-generic-password -s "$keychain" -w) || die "no keychain item $keychain"
fi
export HCLOUD_TOKEN

T=$(mktemp -d "${TMPDIR:-/tmp}/eph-snap.XXXXXX")
chmod 700 "$T"
server_id="" key_id=""
cleanup() {
  local rc=$?
  set +e
  if [ -n "$server_id" ]; then "$H" delete --id "$server_id"; fi
  if [ -n "$key_id" ]; then "$H" ssh-key-delete --id "$key_id"; fi
  rm -rf "$T"
  exit "$rc"
}
trap cleanup EXIT

t0=$(date -u +%s)
lap() { echo "=== +$(($(date -u +%s) - t0)) s: $*"; }
val() { awk -F= -v k="$1" '$1 == k { v = substr($0, length(k) + 2) } END { print v }' "$2"; }

name="gh-eph-snapshot-$t0"
ssh-keygen -q -t ed25519 -N '' -C "$name" -f "$T/key"
"$H" ssh-key-add --name "$name" --public-key "$T/key.pub" --label purpose=gh-ephemeral --label role=snapshot-build > "$T/key.out"
key_id=$(val ssh_key_id "$T/key.out")
lap "ssh key $key_id"

printf '#!/bin/bash\n# U20 F-b: the snapshot server; the work is done over ssh by ci/ephemeral/snapshot.sh\nexit 0\n' > "$T/user-data"
"$H" create --name "$name" --label purpose=gh-ephemeral --label role=snapshot-build --user-data "$T/user-data" \
  --types "$type" --locations "$locs" --image ubuntu-24.04 --ssh-key "$name" > "$T/create.out"
server_id=$(val server_id "$T/create.out")
ip=$(val ipv4 "$T/create.out")
lap "server $server_id ($(val server_type "$T/create.out") in $(val location "$T/create.out"), EUR $(val price_hourly "$T/create.out")/h), $ip"
"$H" wait --id "$server_id" --timeout 300 > /dev/null
lap "running"

# -F /dev/null: no ssh_config, so no locale variables go to the server (SendEnv LANG LC_* of the Mac's; Ubuntu's image has
# none of them, and perl warns on every apt call)
S=(ssh -F /dev/null -i "$T/key" -o IdentitiesOnly=yes -o BatchMode=yes -o StrictHostKeyChecking=accept-new
   -o UserKnownHostsFile="$T/known_hosts" -o ServerAliveInterval=30 -o ConnectTimeout=10 "root@$ip")
# run: on the server; from: a command on DEST piped into one on the server
run() { if [ "$dry" = 1 ]; then echo "DRY-RUN ssh root@$ip: $1" >&2; else "${S[@]}" "$1"; fi; }
from() {
  if [ "$dry" = 1 ]; then echo "DRY-RUN ssh $src: $1 | ssh root@$ip: $2" >&2; return 0; fi
  ssh -o BatchMode=yes "$src" "$1" | "${S[@]}" "set -o pipefail; $2"
}

if [ "$dry" = 0 ]; then
  for i in $(seq 1 60); do
    if "${S[@]}" true 2> /dev/null; then break; fi
    [ "$i" -lt 60 ] || die "no ssh to $ip after 300 s"
    sleep 5
  done
fi
lap "ssh"

# shellcheck disable=SC2016  # expanded on the server
run 'cloud-init status --wait > /dev/null || true
  export DEBIAN_FRONTEND=noninteractive
  apt-get -o DPkg::Lock::Timeout=600 update -q > /dev/null
  apt-get -o DPkg::Lock::Timeout=600 install -y -q docker.io zstd > /dev/null
  printf "{\"features\": {\"containerd-snapshotter\": false}}\n" > /etc/docker/daemon.json
  systemctl enable docker > /dev/null 2>&1
  systemctl restart docker
  echo "docker $(docker version --format "{{.Server.Version}}"), storage $(docker info --format "{{.Driver}}"), zstd $(dpkg-query -W -f="\${Version}" zstd)"
  [ "$(docker info --format "{{.Driver}}")" = overlay2 ]'
lap "docker (overlay2) and zstd installed"

if [ "$dry" = 0 ]; then
  bytes=$(ssh -o BatchMode=yes "$src" "stat -c %s $archive")
else
  bytes=0
fi
t1=$(date -u +%s)
from "cat -- $archive" "zstd -dc --long=27 | docker load"
t2=$(date -u +%s)
[ "$t2" -gt "$t1" ] || t2=$((t1 + 1))
lap "archive streamed and loaded: $bytes bytes in $((t2 - t1)) s ($((bytes * 8 / (t2 - t1) / 1000000)) Mbit/s)"

if [ "$dry" = 0 ]; then
  got=$("${S[@]}" "docker image inspect --format '{{.Id}}' $image_ref")
  [ "$got" = "$image_id" ] || die "the loaded $image_ref is $got, not $image_id"
  "${S[@]}" "docker run --rm --user 1000:1000 -e HOME=/tmp $image_ref vivado -version 2>&1 | sed -n '/^vivado v/p'"
fi
lap "image ID $image_id checked, Vivado starts"

if [ -n "$cache" ]; then
  from "cd $cache && tar -cf - ip_* ooc_*" "mkdir -p /opt/trx-cache && tar -C /opt/trx-cache -xf - && chmod -R a+rX /opt/trx-cache && du -sh /opt/trx-cache && ls /opt/trx-cache | wc -l"
  lap "cache in /opt/trx-cache"
fi

run "printf 'image_ref %s\nimage_id %s\ncache %s\nmade %s\n' '$image_ref' '$image_id' '$([ -n "$cache" ] && echo /opt/trx-cache || echo none)' '$(date -u +%Y-%m-%dT%H:%M:%SZ)' > /etc/gh-ephemeral-image
  apt-get clean
  rm -rf /var/lib/apt/lists/*
  cloud-init clean --logs > /dev/null
  truncate -s 0 /etc/machine-id
  rm -f /var/lib/dbus/machine-id /root/.ssh/authorized_keys /etc/ssh/ssh_host_*
  df -h / | tail -1
  fstrim -av || true
  sync
  systemd-run --on-active=2 /usr/bin/systemctl poweroff > /dev/null"
EPH_FAKE_STATUS=off "$H" wait --id "$server_id" --status off --timeout 300 > /dev/null
lap "cleaned and off"

"$H" snapshot --id "$server_id" --description "$image_ref $short$([ -n "$cache" ] && echo ' + Vivado cache'), U20 F-b" \
  --label purpose=gh-ephemeral --label role=vivado --label "docker-image=$short" > "$T/snap.out"
lap "snapshot $(val image_id "$T/snap.out"): $(val image_size "$T/snap.out") GB stored, for disks from $(val disk_size "$T/snap.out") GB"
cat "$T/snap.out"

"$H" delete --id "$server_id"
server_id=""
if [ "$replace" = 1 ]; then "$H" delete-image --selector purpose=gh-ephemeral,role=vivado --keep-newest; fi
lap "done"
