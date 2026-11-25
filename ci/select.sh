#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Which optional parts a run does, one answer per call.
#
#   ci/select.sh sims-tier   2 on main, integration branches (integ-*), release/* and tags,
#                            workflow_dispatch; else 1 (Alex, 2026-10-06: tier 2 before main, not after)
#   ci/select.sh sims-demand 2 on integration, integ-*, release/* and tags, with SIMS_INPUT=true (the GitHub dispatch
#                            input `extra` with sims) or "[sims]" in HEAD's message; else 0, no benches. The GitHub
#                            matrix's rule (U20 B; MultiCore's sims-tier since 2026-11-08: sims on demand, before
#                            Vivado). The Gitea pipeline keeps sims-tier until the switch (U20 E).
#   ci/select.sh ue2         yes for a workflow_dispatch with the input ue2 = true (of the
#                            pipeline, or of bitstream.yml/package.yml alone), a tag, and a
#                            release/* branch; else no. Alex, 2026-10-03:
#                            the 100T (UE2) is built "at the end, when the thing is
#                            round", never on a normal push.
#
# Read from CI_BRANCH (GitHub's workflows: pr-<number> on a pull request) or GITHUB_REF_NAME, GITHUB_REF_TYPE, GITHUB_EVENT_NAME, UE2_INPUT (the
# dispatch input; a called workflow does not see the caller's event inputs, so the
# pipeline passes it on with `with: ue2:`) and the message of HEAD, so the same
# call answers on the Mac for the checkout there. CI_MSG, when set (also empty),
# stands in for that message (ci/lint.sh tests the answers without HEAD's message).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ref="${CI_BRANCH:-${GITHUB_REF_NAME:-$(git -C "$ROOT" rev-parse --abbrev-ref HEAD)}}"
event="${GITHUB_EVENT_NAME:-}"
release=no
{ [ "${GITHUB_REF_TYPE:-}" = tag ] || [[ "$ref" == release/* ]]; } && release=yes
msg="${CI_MSG-$(git -C "$ROOT" log -1 --format=%B)}"
case "${1:?sims-tier, sims-demand or ue2}" in
  sims-tier)
    if [ "$ref" = main ] || [[ "$ref" == integ-* ]] || [ "$release" = yes ] || [ "$event" = workflow_dispatch ] || [[ "$msg" == *"[sims]"* ]]; then echo 2; else echo 1; fi ;;
  sims-demand)
    if [ "$ref" = integration ] || [[ "$ref" == integ-* ]] || [ "$release" = yes ] || [ "${SIMS_INPUT:-}" = true ] || [[ "$msg" == *"[sims]"* ]]; then echo 2; else echo 0; fi ;;
  ue2)
    # UE2_INPUT is only ever true from a dispatch (a push has no such input); the event
    # name cannot be checked, a called workflow sees "workflow_call" there
    if [ "$release" = yes ] || [ "${UE2_INPUT:-}" = true ]; then echo yes; else echo no; fi ;;
  *) echo "unknown question $1" >&2; exit 2 ;;
esac
