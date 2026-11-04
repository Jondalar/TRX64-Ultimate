#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the behaviour scenarios of tests/bdd/features in ue2emu.

    tests/bdd/run_bdd.py                      # every feature
    tests/bdd/run_bdd.py cartridge_port       # one feature file, by name
    tests/bdd/run_bdd.py --tags needs-game    # only the scenarios with that tag
    tests/bdd/run_bdd.py --list               # what there is, run nothing

The firmware image comes from U64_EMU_FIRMWARE, else from the application the
submodule has built; the emulator from U64_EMU, else ue2emu's default place
(tests/bdd/README.md). A scenario that misses one of them says SKIP, which does
not fail the run. The exit code is 1 when a scenario failed.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import bdd                                   # noqa: E402
import steps.emulator_steps                  # noqa: E402,F401  (registers the steps)

FEATURES = HERE / "features"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("feature", nargs="*", help="feature files by name, without .feature")
    ap.add_argument("--tags", default="", help="comma separated tags; only those scenarios run")
    ap.add_argument("--list", action="store_true", help="print the scenarios and stop")
    args = ap.parse_args()

    features = list(bdd.features_in(FEATURES))
    if args.feature:
        want = {name.removesuffix(".feature") for name in args.feature}
        features = [f for f in features if f.path.stem in want]
        if not features:
            print(f"no feature file named {', '.join(sorted(want))} in {FEATURES}", file=sys.stderr)
            return 2

    if args.list:
        for feature in features:
            print(f"{feature.path.name}: {feature.name}")
            for scenario in feature.scenarios:
                tags = " ".join(f"@{t}" for t in scenario.tags)
                print(f"  {scenario.name}{'  ' + tags if tags else ''}")
        return 0

    tags = [t.strip() for t in args.tags.split(",") if t.strip()]
    results = bdd.run(features, tags)

    print()
    for r in results:
        mark = {"ok": "PASS", "failed": "FAIL", "skipped": "SKIP"}[r.status]
        print(f"{mark}  {r.scenario}  ({r.seconds:.1f}s)" + (f"  {r.detail.strip()}" if r.detail else ""))
    failed = [r for r in results if r.status == "failed"]
    skipped = [r for r in results if r.status == "skipped"]
    print(f"\n{len(results)} scenarios, {len(results) - len(failed) - len(skipped)} passed, "
          f"{len(failed)} failed, {len(skipped)} skipped")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
