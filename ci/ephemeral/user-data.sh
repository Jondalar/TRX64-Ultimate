#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# U20 F-a (D17): the cloud-init user data of an ephemeral runner server (Ubuntu 24.04). ci/ephemeral/ghrunner.sh user-data
# fills in the three @...@ fields; cloud-init runs the result once, as root, at the first boot.
#
#  1. Lifetime cap: CAP_MINUTES after this script starts, the machine powers off, whatever runs then. Hetzner bills a
#     server that is off until it is deleted, so the cap ends the runner and the job, not the bill; deleting is the
#     workflow's stop job (or a sweep). No Hetzner token is on this machine.
#  2. No inbound shell: the create request named no SSH key and the root password of its answer was dropped; sshd is
#     switched off as well. Hetzner's web console still reaches the machine.
#  3. The official actions/runner release, pinned by version and SHA-256 (checked before it is unpacked), runs as user
#     runner (passwordless sudo, as on GitHub's hosted runners) with the JIT config: one job, then it deregisters and
#     ends, and the machine powers off.
#  Times go to /var/lib/gh-ephemeral/stamps ("name epoch-seconds"); the work job's report (ci/ephemeral/proof.sh) reads
#  them. The JIT config is never printed (this script's output lands in /var/log/cloud-init-output.log).
set -euo pipefail
umask 022

RUNNER_VERSION=2.338.2
RUNNER_SHA256=baebf204307bac6193480852732f0aba2ae97b6c5597c244f4d5558390147c40
CAP_MINUTES=@CAP_MINUTES@
RUNNER_LABEL=@RUNNER_LABEL@

D=/var/lib/gh-ephemeral
mkdir -p "$D"
chmod 755 "$D"
stamp() { echo "$1 $(date -u +%s)" >> "$D/stamps"; }
stamp userdata
trap 'stamp "failed_at_line_$LINENO"' ERR
echo "label $RUNNER_LABEL" > "$D/info"
echo "cap_minutes $CAP_MINUTES" >> "$D/info"

systemd-run --unit=gh-ephemeral-cap --on-active="${CAP_MINUTES}min" --timer-property=AccuracySec=1s \
  /usr/bin/systemctl poweroff --force
systemctl disable --now ssh.service ssh.socket > /dev/null 2>&1 || true

# unattended-upgrades may hold the dpkg lock at the first boot: apt waits for it instead of failing
echo 'DPkg::Lock::Timeout "600";' > /etc/apt/apt.conf.d/90-gh-ephemeral

useradd --create-home --shell /bin/bash runner
echo 'runner ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/90-gh-runner
chmod 440 /etc/sudoers.d/90-gh-runner

R=/home/runner/actions-runner
T=$(mktemp -d)
curl -fsSL --retry 5 --retry-all-errors -o "$T/runner.tar.gz" \
  "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz"
echo "${RUNNER_SHA256}  $T/runner.tar.gz" | sha256sum --check --strict -
mkdir -p "$R"
tar -xzf "$T/runner.tar.gz" -C "$R"
rm -rf "$T"
stamp downloaded
"$R/bin/installdependencies.sh" > "$D/deps.log" 2>&1
stamp deps
chown -R runner:runner "$R"

JIT='@JITCONFIG@'
stamp runner_start
cd "$R"
rc=0
runuser -u runner -- env -i HOME=/home/runner USER=runner LOGNAME=runner LANG=C.UTF-8 \
  PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin ./run.sh --jitconfig "$JIT" || rc=$?
unset JIT
stamp runner_end
echo "runner_rc $rc" >> "$D/info"
systemctl poweroff
