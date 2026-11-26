#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# U20 F-a (D17): the report of the proof job `work`, run on the ephemeral server. Markdown to $GITHUB_STEP_SUMMARY
# (stdout without it): the machine (uname -a, nproc, CPU, memory, disk, OS) and the seconds from the server's create
# request to this job, with the server's own steps between them.
#
#   T_CREATE, T_RUNNING, T_ONLINE  epoch seconds, measured by job `start` on GitHub's runner (create request, API says
#                                  running, GitHub lists the runner online)
#   T_JOB                          epoch seconds of this job's first step
#   SERVER_TYPE, SERVER_LOCATION, PRICE_HOURLY, RUNNER_LABEL   from job `start`
#   EPH_STAMPS                     the stamps of ci/ephemeral/user-data.sh (default /var/lib/gh-ephemeral/stamps)
#   EPH_TITLE                      the heading (default F-a's; F-b's job work names its own)
# The two clocks (GitHub's runner, the server) are both NTP-synchronised; a second either way is within reading.
# No grep: under pipefail a grep without a match ends the script (MultiCore T239.5); awk answers an empty line instead.
set -euo pipefail
out=${GITHUB_STEP_SUMMARY:-/dev/stdout}
stamps=${EPH_STAMPS:-/var/lib/gh-ephemeral/stamps}
now=$(date -u +%s)

stamp() { if [ -r "$stamps" ]; then awk -v k="$1" '$1 == k { print $2; exit }' "$stamps"; fi; }
since() { if [ -n "${1:-}" ] && [ -n "${T_CREATE:-}" ]; then echo "$(($1 - T_CREATE))"; else echo "-"; fi; }

boot=""
if [ -r /proc/uptime ]; then boot=$((now - $(awk '{ print int($1) }' /proc/uptime))); fi
cpu=$( (lscpu 2>/dev/null || true) | awk -F: '/^Model name/ { sub(/^ +/, "", $2); print $2; exit }')
mem=$( (free -m 2>/dev/null || true) | awk '/^Mem:/ { print $2 " MiB total, " $7 " MiB available" }')
disk=$( (df -h / 2>/dev/null || true) | awk 'NR == 2 { print $2 " size, " $3 " used, " $4 " available to users" }')
os=$( (cat /etc/os-release 2>/dev/null || true) | awk -F= '$1 == "PRETTY_NAME" { gsub(/"/, "", $2); print $2 }')
failed=$( (cat "$stamps" 2>/dev/null || true) | awk '$1 ~ /^failed/ { print $1 }')

{
  echo "## ${EPH_TITLE:-U20 F-a: one job on an ephemeral Hetzner Cloud server}"
  echo
  echo "| | |"
  echo "|---|---|"
  echo "| server | ${SERVER_TYPE:-?} in ${SERVER_LOCATION:-?}, EUR ${PRICE_HOURLY:-?} per hour net (the API's price), label \`${RUNNER_LABEL:-?}\` |"
  echo "| \`uname -a\` | \`$(uname -a)\` |"
  echo "| \`nproc\` | $(nproc 2>/dev/null || echo '?') (${cpu:-CPU model unknown}) |"
  echo "| memory | ${mem:-?} |"
  echo "| disk / | ${disk:-?} |"
  echo "| OS | ${os:-?} |"
  echo
  echo "### Seconds after the create request"
  echo
  echo "| step | s | clock |"
  echo "|---|---:|---|"
  echo "| server running (Hetzner API) | $(since "${T_RUNNING:-}") | GitHub runner |"
  echo "| kernel booted | $(since "$boot") | server |"
  echo "| user data starts | $(since "$(stamp userdata)") | server |"
  echo "| runner downloaded, SHA-256 checked, unpacked | $(since "$(stamp downloaded)") | server |"
  echo "| runner dependencies installed | $(since "$(stamp deps)") | server |"
  echo "| runner started | $(since "$(stamp runner_start)") | server |"
  echo "| runner online (GitHub API) | $(since "${T_ONLINE:-}") | GitHub runner |"
  echo "| this job's first step | $(since "${T_JOB:-}") | server |"
  echo "| this report | $(since "$now") | server |"
  if [ -n "$failed" ]; then echo; echo "User data stamped a failure: \`$failed\`."; fi
} >> "$out"
