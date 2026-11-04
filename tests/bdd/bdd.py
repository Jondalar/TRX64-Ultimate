#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A small Gherkin runner, so a behaviour can be read without reading Python.

A feature file holds `Feature:`, an optional `Background:` and any number of
`Scenario:` / `Scenario Outline:` blocks whose steps begin with Given, When,
Then, And or But. `And`/`But` keep the kind of the step above them. A
`Scenario Outline:` is run once per row of its `Examples:` table, with
`<column>` replaced by the row's cell.

Steps are plain Python functions, registered by a regular expression that must
match the whole step text; its groups become the arguments after the world:

    @given(r'the setting Cartridge Preference is "([^"]+)"')
    def pref(world, value):
        world.settings["Cartridge Preference"] = value

The world is one object per scenario: anything may be put on it, and
`world.closing(fn)` registers a cleanup that runs when the scenario ends,
whether it passed or not. A step that raises `Skip` ends its scenario as SKIP,
which is not a failure: that is how a scenario says it needs something this
machine does not have (the emulator, a game cartridge).

This module is the framework; `run_bdd.py` is the command line around it.
"""

from __future__ import annotations

import re
import time
import traceback
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Iterator, Sequence

KEYWORDS = ("Given", "When", "Then", "And", "But")
KINDS = ("given", "when", "then")


class Skip(Exception):
    """The scenario cannot run here, and that is not a failure."""


class StepFailure(Exception):
    """A Then that did not hold, or a step that could not do its work."""


# ---- the step registry -------------------------------------------------------

@dataclass
class StepDef:
    kind: str
    pattern: re.Pattern[str]
    func: Callable[..., None]


REGISTRY: list[StepDef] = []


def _register(kind: str, pattern: str) -> Callable[[Callable[..., None]], Callable[..., None]]:
    def wrap(func: Callable[..., None]) -> Callable[..., None]:
        REGISTRY.append(StepDef(kind, re.compile(pattern + r"\s*$"), func))
        return func
    return wrap


def given(pattern: str):
    return _register("given", pattern)


def when(pattern: str):
    return _register("when", pattern)


def then(pattern: str):
    return _register("then", pattern)


def find(kind: str, text: str) -> tuple[StepDef, tuple[str, ...]]:
    for sd in REGISTRY:
        if sd.kind != kind:
            continue
        m = sd.pattern.match(text)
        if m:
            return sd, m.groups()
    raise StepFailure(f"no {kind} step matches {text!r}")


# ---- the world ---------------------------------------------------------------

class World:
    """One scenario's state, with the cleanups it asked for."""

    def __init__(self) -> None:
        self._closers: list[Callable[[], None]] = []

    def closing(self, fn: Callable[[], None]) -> None:
        self._closers.append(fn)

    def close(self) -> None:
        while self._closers:
            fn = self._closers.pop()
            try:
                fn()
            except Exception:                                  # noqa: BLE001
                traceback.print_exc()


# ---- the feature file --------------------------------------------------------

@dataclass
class Step:
    kind: str
    text: str
    keyword: str


@dataclass
class Scenario:
    name: str
    steps: list[Step]
    tags: list[str] = field(default_factory=list)
    line: int = 0


@dataclass
class Feature:
    name: str
    description: list[str]
    background: list[Step]
    scenarios: list[Scenario]
    path: Path


def _steps_of(lines: Iterable[tuple[int, str]]) -> list[Step]:
    steps: list[Step] = []
    kind = ""
    for _, line in lines:
        word, _, rest = line.partition(" ")
        if word in ("Given", "When", "Then"):
            kind = word.lower()
        elif word in ("And", "But"):
            if not kind:
                raise StepFailure(f"{word} without a Given, When or Then: {line!r}")
        else:
            raise StepFailure(f"not a step: {line!r}")
        steps.append(Step(kind, rest.strip(), word))
    return steps


def _table(rows: Sequence[str]) -> list[dict[str, str]]:
    cells = [[c.strip() for c in r.strip().strip("|").split("|")] for r in rows]
    head, body = cells[0], cells[1:]
    return [dict(zip(head, row)) for row in body]


def parse(path: Path) -> Feature:
    """The feature file as objects; an outline becomes one scenario per row."""
    name = ""
    description: list[str] = []
    background: list[Step] = []
    scenarios: list[Scenario] = []

    block: list[tuple[int, str]] = []      # the steps of the block being read
    kind = ""                              # "background", "scenario", "outline"
    title = ""
    tags: list[str] = []
    pending_tags: list[str] = []
    table: list[str] = []
    start_line = 0

    def flush() -> None:
        nonlocal block, kind, title, tags, table
        if not kind:
            block, table = [], []
            return
        if kind == "background":
            background.extend(_steps_of(block))
        elif kind == "scenario":
            scenarios.append(Scenario(title, _steps_of(block), tags, start_line))
        elif kind == "outline":
            if not table:
                raise StepFailure(f"{path}: the outline {title!r} has no Examples table")
            for row in _table(table):
                steps = []
                for s in _steps_of(block):
                    text = s.text
                    for col, value in row.items():
                        text = text.replace(f"<{col}>", value)
                    steps.append(Step(s.kind, text, s.keyword))
                shown = ", ".join(f"{k}={v}" for k, v in row.items())
                scenarios.append(Scenario(f"{title} [{shown}]", steps, tags, start_line))
        block, table, kind, tags = [], [], "", []

    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("@"):
            pending_tags = [t.lstrip("@") for t in line.split()]
            continue
        head, sep, rest = line.partition(":")
        if sep and head in ("Feature", "Background", "Scenario", "Scenario Outline", "Examples"):
            if head == "Examples":
                kind_before = kind
                if kind_before != "outline":
                    raise StepFailure(f"{path}:{number}: Examples outside a Scenario Outline")
                continue
            flush()
            if head == "Feature":
                name = rest.strip()
            elif head == "Background":
                kind, title, start_line = "background", "", number
            else:
                kind = "scenario" if head == "Scenario" else "outline"
                title, start_line = rest.strip(), number
                tags, pending_tags = pending_tags, []
            continue
        if line.startswith("|"):
            table.append(line)
            continue
        if line.split(" ")[0] in KEYWORDS:
            block.append((number, line))
        elif not kind:
            description.append(line)
        else:
            raise StepFailure(f"{path}:{number}: not a step, a table row or a header: {line!r}")
    flush()
    return Feature(name, description, background, scenarios, path)


# ---- running -----------------------------------------------------------------

@dataclass
class Result:
    feature: str
    scenario: str
    status: str            # "ok", "failed", "skipped"
    detail: str = ""
    seconds: float = 0.0


def run_scenario(feature: Feature, scenario: Scenario, verbose: bool = True) -> Result:
    world = World()
    started = time.monotonic()
    status, detail = "ok", ""
    if verbose:
        print(f"  Scenario: {scenario.name}")
    try:
        for step in feature.background + scenario.steps:
            sd, args = find(step.kind, step.text)
            t0 = time.monotonic()
            sd.func(world, *args)
            if verbose:
                print(f"    {step.keyword} {step.text} ... ok ({time.monotonic() - t0:.1f}s)")
    except Skip as exc:
        status, detail = "skipped", str(exc)
        if verbose:
            print(f"    SKIP: {detail}")
    except Exception as exc:                                    # noqa: BLE001
        status = "failed"
        detail = str(exc) if isinstance(exc, StepFailure) else traceback.format_exc(limit=3)
        if verbose:
            print(f"    FAIL: {detail.strip()}")
    finally:
        world.close()
    return Result(feature.name, scenario.name, status, detail, time.monotonic() - started)


def run(features: Sequence[Feature], tags: Sequence[str] = (), verbose: bool = True) -> list[Result]:
    results: list[Result] = []
    for feature in features:
        if verbose:
            print(f"Feature: {feature.name}  ({feature.path.name})")
        for scenario in feature.scenarios:
            if tags and not set(tags) & set(scenario.tags):
                continue
            results.append(run_scenario(feature, scenario, verbose))
    return results


def features_in(path: Path) -> Iterator[Feature]:
    for file in sorted(path.glob("*.feature")):
        yield parse(file)
