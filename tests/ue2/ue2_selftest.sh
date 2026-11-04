#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Offline test of the UE2 session scripts against the fake board (no hardware): tests/ue2/ue2_selftest.py
cd "$(dirname "$0")" && exec nice -n 15 python3 -u ue2_selftest.py "$@"
