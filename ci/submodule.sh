#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch the Ultimate app submodule (the app comes from it; Gideon's VHDL for the
# gateware and the benches is our copy in vendor/gideon since U20 A1). Read with
# APP_TOKEN, a read-only token of a user who can read TREX/1541ultimate; without
# it the plain submodule URL is used.
#
#   APP_TOKEN=... GITHUB_SERVER_URL=... [NESTED="software/lwip software/httpd"] ci/submodule.sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -n "${APP_TOKEN:-}" ]; then
  SERVER="${GITHUB_SERVER_URL#*://}"
  git -C "$ROOT" config submodule.software/ultimate-app.url \
    "http://ci:${APP_TOKEN}@${SERVER}/TREX/1541ultimate.git"
fi
git -C "$ROOT" submodule update --init --depth 1 software/ultimate-app \
  || git -C "$ROOT" submodule update --init software/ultimate-app
# NESTED="software/lwip software/httpd": the app's own submodules the build needs
# (not neorv32 and ecpprog: nothing here builds them). Whole clones, not depth 1:
# a shallow clone cannot fetch a recorded commit that is not a branch tip. The
# NAS failed to resolve github.com once (run 343), so each is tried three times.
for n in ${NESTED:-}; do
  for try in 1 2 3; do
    git -C "$ROOT/software/ultimate-app" submodule update --init --jobs 1 "$n" && break
    [ "$try" -lt 3 ] || exit 1
    sleep 5
  done
done
git -C "$ROOT/software/ultimate-app" log --oneline -1
