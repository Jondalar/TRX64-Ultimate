#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Which optional parts a run does, one answer per call.
#
#   ci/select.sh sims-tier   2 on main, integration branches (integ-*), release/* and tags,
#                            workflow_dispatch; else 1 (Alex, 2026-10-06: tier 2 before main, not after)
#   ci/select.sh ue2         yes for a workflow_dispatch with the input ue2 = true (of the
#                            pipeline, or of bitstream.yml/package.yml alone), a tag, and a
#                            release/* branch; else no. Alex, 2026-10-03:
#                            the 100T (UE2) is built "at the end, when the thing is
#                            round", never on a normal push.
#   ci/select.sh update      yes on integ-m1, main and tags v* (the tester update files are built for
#                            every commit there), or when UPDATE_FORCE=true (dispatch input `update`, for
#                            any other branch); else no.
#
# Read from GITHUB_REF_NAME, GITHUB_REF_TYPE, GITHUB_EVENT_NAME, UE2_INPUT (the
# dispatch input; a called workflow does not see the caller's event inputs, so the
# pipeline passes it on with `with: ue2:`) and the message of HEAD, so the same
# call answers on the Mac for the checkout there. CI_MSG, when set (also empty),
# stands in for that message (ci/lint.sh tests the answers without HEAD's message).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ref="${GITHUB_REF_NAME:-$(git -C "$ROOT" rev-parse --abbrev-ref HEAD)}"
event="${GITHUB_EVENT_NAME:-}"
release=no
{ [ "${GITHUB_REF_TYPE:-}" = tag ] || [[ "$ref" == release/* ]]; } && release=yes
msg="${CI_MSG-$(git -C "$ROOT" log -1 --format=%B)}"
case "${1:?sims-tier, ue2 or update}" in
  sims-tier)
    if [ "$ref" = main ] || [[ "$ref" == integ-* ]] || [ "$release" = yes ] || [ "$event" = workflow_dispatch ] || [[ "$msg" == *"[sims]"* ]]; then echo 2; else echo 1; fi ;;
  ue2)
    # UE2_INPUT is only ever true from a dispatch (a push has no such input); the event
    # name cannot be checked, a called workflow sees "workflow_call" there
    if [ "$release" = yes ] || [ "${UE2_INPUT:-}" = true ]; then echo yes; else echo no; fi ;;
  update)
    if [ "$ref" = integ-m1 ] || [ "$ref" = main ] || [ "${GITHUB_REF_TYPE:-}" = tag ] || [ "${UPDATE_FORCE:-}" = true ]; then echo yes; else echo no; fi ;;
  *) echo "unknown question $1" >&2; exit 2 ;;
esac
