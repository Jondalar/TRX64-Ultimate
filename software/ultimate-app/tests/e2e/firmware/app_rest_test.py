#!/usr/bin/env python3
"""E2E: apps are listed and started over REST (doc/AppLoader_Spec.md section 9).

software/appsdk/samples/restcheck names three actions in its manifest's
`rest` list. The suite runs it once from the file browser, which registers
it, and then drives it only over REST in the emulator:

- GET /v1/apps lists it with its `rest` actions, GET /v1/apps/{id} alone;
- PUT /v1/apps/restcheck:run starts it without a screen, with the file and
  args of the call, and answers with its exit code and what it replied;
- the app reads a setting through the ABI's config table;
- a non-zero exit code comes back as it is;
- an unknown app answers 404, an action the manifest does not name 400.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import socket
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

# The one stanza that puts the shared library on sys.path; see tests/lib/bootstrap.py.
sys.path.insert(0, str(next(p for p in Path(__file__).resolve().parents
                            if (p / "tests" / "lib").is_dir()) / "tests" / "lib"))
import bootstrap  # noqa: E402,F401

sys.path.insert(0, str(Path(bootstrap.__file__).resolve().parents[1] / "e2e" / "lib"))

import carttool                                                 # noqa: E402
from emulator import Emulator, emulator_binary, firmware_image, repo_root  # noqa: E402
from report import (Failure, check, detail, format_exception,   # noqa: E402
                    suite_fail, suite_ok, suite_skip)

SUITE = "app_rest_test"
APP = "software/appsdk/samples/restcheck/restcheck.u2a"
MODE = "ultimate"
REST_TIMEOUT = 30.0


class SkipSuite(Exception):
    pass


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def abi_of_the_tree() -> str:
    """The ABI the firmware of this tree reports, from the header that sets it.

    Written out, the expectation goes stale: it still read 2.0 after
    U2P_ABI_MINOR became 1 (f3ed17e4, 2026-09-29), and the suite failed on the
    version instead of on the launch it is about.
    """
    header = repo_root() / "software" / "app_loader" / "app_api.h"
    text = header.read_text(encoding="utf-8", errors="replace")

    def one(name: str) -> str:
        m = re.search(rf"^#define\s+{name}\s+(\d+)", text, re.M)
        if not m:
            raise Failure(f"{header} defines no {name}")
        return m.group(1)

    return f"{one('U2P_ABI_MAJOR')}.{one('U2P_ABI_MINOR')}"


class Rest:
    def __init__(self, port: int) -> None:
        self.base = f"http://127.0.0.1:{port}"

    def call(self, method: str, path: str, **params: str) -> tuple[int, dict]:
        url = self.base + path
        if params:
            url += "?" + urllib.parse.urlencode(params, quote_via=urllib.parse.quote)
        req = urllib.request.Request(url, method=method, data=b"" if method == "PUT" else None)
        try:
            with urllib.request.urlopen(req, timeout=REST_TIMEOUT) as r:
                return r.status, json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            body = e.read()
            return e.code, json.loads(body) if body else {}

    def wait_ready(self) -> None:
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                if self.call("GET", "/v1/version")[0] == 200:
                    return
            except OSError:
                pass
            time.sleep(0.5)
        raise Failure("REST did not answer within 60 s")

    def readmem(self, address: int) -> int:
        url = f"{self.base}/v1/machine:readmem?address={address:04X}&length=1"
        with urllib.request.urlopen(url, timeout=REST_TIMEOUT) as r:
            return r.read()[0]

    def clock_runs(self) -> bool:
        first = self.readmem(0xA2)
        time.sleep(1.0)
        return self.readmem(0xA2) != first

    def run(self, app: str, **params: str) -> tuple[int, dict]:
        return self.call("PUT", f"/v1/apps/{app}:run", **params)


def expect(status: int, body: dict, want: int, what: str) -> dict:
    if status != want:
        raise Failure(f"{what}: HTTP {status}, not {want}: {body}")
    return body


def run(work: Path) -> None:
    stick = work / "usb"
    stick.mkdir(parents=True)
    shutil.copy(repo_root() / APP, stick / "restcheck.u2a")
    port = free_port()
    emu = Emulator(usb_dir=stick, board="c64u", settings=carttool.settings_for(MODE),
                   extra=["--net", "user", "--hostfwd", f"tcp:{port}:80", "--web-port", "0"])
    emu.start()
    try:
        rest = Rest(port)
        with check("running the app once from the file browser registers it"):
            carttool.run_app(emu, MODE, app="restcheck.u2a")
            rest.wait_ready()
            apps = expect(*rest.call("GET", "/v1/apps"), 200, "GET /v1/apps")["apps"]
            mine = [a for a in apps if a.get("id") == "restcheck"]
            if not mine:
                raise Failure(f"restcheck is not in {apps}")
            if mine[0].get("rest") != ["echo", "config", "fail"]:
                raise Failure(f"rest actions are {mine[0].get('rest')}")
            one = expect(*rest.call("GET", "/v1/apps/restcheck"), 200, "GET /v1/apps/restcheck")
            if one.get("app", {}).get("id") != "restcheck":
                raise Failure(f"GET /v1/apps/restcheck answered {one}")

        with check("with the menu closed the C64 runs"):
            emu.button(settle=1500)
            if not rest.clock_runs():
                raise Failure("the jiffy clock at $A2 stands with the menu closed")

        with check("a REST launch passes action, file and args, without a screen"):
            body = expect(*rest.run("restcheck", action="echo", file="/USB0/some file.prg",
                                    args="one two"), 200, "echo")
            want = (f"abi={abi_of_the_tree()} ui=0 source=3 action=echo "
                    "file=/USB0/some file.prg args=one two")
            if body.get("exit_code") != 0 or body.get("output") != want:
                raise Failure(f"echo answered {body}")
            detail(body["output"])

        with check("action defaults to the first rest action"):
            body = expect(*rest.run("restcheck"), 200, "default action")
            if "action=echo" not in body.get("output", ""):
                raise Failure(f"answered {body}")

        with check("the app reads a setting through the config table"):
            body = expect(*rest.run("restcheck", action="config",
                                    args="Drive A Settings/Drive Bus ID"), 200, "config")
            if body.get("exit_code") != 0 or body.get("output") != "8":
                raise Failure(f"config answered {body}")

        with check("a non-zero exit code comes back as it is"):
            body = expect(*rest.run("restcheck", action="fail"), 200, "fail")
            if body.get("exit_code") != 7 or body.get("output") != "failed on purpose":
                raise Failure(f"fail answered {body}")

        with check("an unknown app answers 404, an unlisted action 400"):
            expect(*rest.run("nosuchapp"), 404, "unknown app")
            expect(*rest.call("GET", "/v1/apps/nosuchapp"), 404, "unknown app info")
            expect(*rest.run("restcheck", action="flash"), 400, "unlisted action")

        with check("the C64 runs on after the REST launches"):
            # The loader pauses the C64 for each launch and lets it go afterwards.
            if not rest.clock_runs():
                raise Failure("the jiffy clock at $A2 stands after the launches")
    finally:
        emu.stop()


def main() -> int:
    parser = argparse.ArgumentParser(description="Start an app over REST in the emulator.")
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
    args = parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix="app-rest-"))
    try:
        try:
            emulator_binary()
            firmware_image()
        except Failure as exc:
            raise SkipSuite(str(exc)) from None
        if not (repo_root() / APP).is_file():
            raise SkipSuite(f"no {APP}; see build/build_app.sh restcheck")
        run(work)
    except SkipSuite as exc:
        suite_skip(SUITE, str(exc))
        return 0
    except Exception as exc:            # noqa: BLE001
        suite_fail(SUITE, format_exception(exc))
        return 1
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
    suite_ok(SUITE)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
