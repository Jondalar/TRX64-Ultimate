# Behaviour scenarios

What the machine does, written so it can be read without reading Python. Each
file under `features/` describes one behaviour in Gherkin; the steps behind the
sentences live in `steps/emulator_steps.py`, and `bdd.py` is the small runner
that ties the two together. There is no framework to install: the runner is
~230 lines in this directory and uses the same Python the other test scripts
here use.

```sh
tests/bdd/run_bdd.py                       # every scenario
tests/bdd/run_bdd.py cartridge_port        # one feature file
tests/bdd/run_bdd.py --tags needs-game     # only the scenarios with that tag
tests/bdd/run_bdd.py --list                # what there is, run nothing
```

The exit code is 1 when a scenario failed. A scenario that cannot run here says
SKIP and does not fail the run.

## What it drives

The scenarios run in **ue2emu**, through the application's own end-to-end
library (`software/ultimate-app/tests/e2e/lib`), so they see the machine the
way the app's suites do. Two things have to be there:

- the emulator: `U64_EMU`, else ue2emu's default place;
- the firmware: `U64_EMU_FIRMWARE`, else `ultimate.elf`/`ultimate.app` from the
  submodule's build. A released `ultimate.app` works as well:

```sh
U64_EMU_FIRMWARE=~/Downloads/ultimate.app tests/bdd/run_bdd.py
```

## What is in the port

`features/cartridge_port.feature` needs a cartridge. The repository carries no
game and no ROM, so the scenarios build their own: `data/port_probe.asm` is an
8 KiB autostart cartridge that writes one line on the screen, assembled with
`tools/64tass.sh` and packed with `tests/cart/bin2crt.py` while the scenario
runs. The scenario tagged `@needs-game` takes a real EasyFlash from
`$TRX64_BDD_CRT` and skips without it:

```sh
TRX64_BDD_CRT=~/crt/wasteland_ef_v1.0.1.crt tests/bdd/run_bdd.py --tags needs-game
```

## Writing a scenario

A step is a function with the sentence as its regular expression; the groups
are its arguments, and the first one is the scenario's world:

```python
@then(r'cart-info reports "([a-z_]+)" as "([^"]*)"')
def info_is(world, key, value):
    ...
```

`Given` builds the machine, the first `When` starts it. Put anything on the
world; `world.closing(fn)` registers a cleanup that runs when the scenario
ends, failed or not. `raise Skip("...")` when the scenario needs something this
machine does not have, `raise StepFailure("...")` when the behaviour did not
hold — the message is what the report shows, so say the measured value in it.

`Scenario Outline:` with an `Examples:` table runs once per row, with
`<column>` replaced by the cell.
