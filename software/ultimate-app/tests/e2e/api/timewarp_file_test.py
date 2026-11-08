#!/usr/bin/env python3
# E2E: Verifies the Timewarp files of T66 - .c64re, .c64rering and .trxring -
# over REST, on a board.
#
# What each check is for:
#   - a .c64re carries one machine state. Loading it replaces the machine and
#     the ring starts over, so the frame count right after a load is small.
#   - a .trxring carries the ring itself. Loading it has to bring the timeline
#     back, so the frame count right after a load is the one the file was
#     written with. That difference is the whole point of part 6, and it is the
#     one thing no host test can show.
#   - every load has to leave a device that still answers. A load stops the
#     machine, writes its memories and lets it go; a firmware that gets that
#     wrong leaves the gate shut and the REST server with it.

import argparse
import ftplib
import sys
import time
import urllib.error
from pathlib import Path

# The one stanza that puts the shared library on sys.path; see tests/lib/bootstrap.py.
sys.path.insert(0, str(next(p for p in Path(__file__).resolve().parents
                            if (p / "tests" / "lib").is_dir()) / "tests" / "lib"))
import bootstrap  # noqa: E402,F401
import cli  # noqa: E402
import ftp as ftp_lib  # noqa: E402  (needs tests/lib on sys.path first)
import rest as rest_lib  # noqa: E402  (needs tests/lib on sys.path first)
from api import UltimateApi  # noqa: E402  (needs tests/lib on sys.path first)
from report import (  # noqa: E402  (needs tests/lib on sys.path first)
    Failure, teardown_step, check_count, check_fail, check_ok, check_skip, check_start, detail,
    format_exception, section, suite_fail, suite_ok)

SUITE = "timewarp_file_test"
# Where the files go. /Temp is the device's RAM drive, which is enough for the
# two small ones; a .trxring carries the log and is as big as the ring it was
# written from, so --dir /Usb0 is the way to run this suite against a long ring.
DEFAULT_DIR = "/Temp"
# The ring is restarted and refilled before the .trxring, so the file stays the
# size of this many seconds rather than of whatever ran before.
NATIVE_SECONDS = 1.0
# Seconds of machine time to let the ring fill before anything is written.
FILL_SECONDS = 3.0
LIVENESS_TIMEOUT_SECONDS = 15.0


class SuiteRunner:
    def __init__(self, args) -> None:
        self.args = args
        self.api = UltimateApi(args.host, args.password, args.timeout)
        d = args.dir.rstrip("/")
        self.files = {
            "snap": f"{d}/t66_one.c64re",
            "ring": f"{d}/t66_ring.c64rering",
            "native": f"{d}/t66_ring.trxring",
        }

    # -- helpers ------------------------------------------------------------
    def alive(self) -> str | None:
        return self.api.unreachable_reason(LIVENESS_TIMEOUT_SECONDS)

    def status(self) -> dict:
        code, _, body = self.api.rest.request("GET", "/v1/timewarp:status", idempotent=True)
        if code != 200:
            raise Failure(f"timewarp:status answered HTTP {code}: {body[:160]!r}")
        return rest_lib.json_object("timewarp:status", body)

    def put(self, verb: str, params: dict, expected: int = 200) -> dict:
        code, _, body = self.api.rest.request("PUT", f"/v1/timewarp:{verb}", params=params)
        if code != expected:
            raise Failure(f"timewarp:{verb} answered HTTP {code}, expected {expected}: {body[:200]!r}")
        return rest_lib.json_object(f"timewarp:{verb}", body)

    def ftp_delete(self, paths) -> None:
        try:
            with ftp_lib.session(self.args.host, self.args.password) as client:
                for path in paths:
                    ftp_lib.delete_quietly(client, path)
        except (*ftplib.all_errors, OSError, Failure):
            pass

    def size_of(self, path: str) -> int:
        info = self.api.files.info(path)
        if info is None:
            raise Failure(f"{path} does not exist")
        return int(info.size)

    def fill_ring(self, seconds: float = FILL_SECONDS) -> None:
        """A fresh ring: start() empties the timeline, so what follows is
        only what this call let run."""
        self.put("start", {})
        time.sleep(seconds)

    def after_load(self, what: str) -> dict | None:
        """Every load is followed by the same two questions."""
        check_start(f"the device still answers after loading {what}")
        reason = self.alive()
        if reason:
            check_fail(f"the device stopped answering after loading {what}: {reason}. "
                       "It needs a power cycle.")
            return None
        check_ok()
        check_start(f"the ring runs again after loading {what}")
        st = self.status()
        if not st.get("on"):
            check_fail(f"the ring is off after loading {what}: {st}")
            return None
        check_ok()
        return st

    # -- the cases ----------------------------------------------------------
    def case_snapshot(self) -> bool:
        section("one frame (.c64re)")
        path = self.files["snap"]
        check_start(f"timewarp:dump writes {path}")
        answer = self.put("dump", {"path": path, "frames": 0})
        size = self.size_of(path)
        if size < 1024:
            check_fail(f"{path} is only {size} bytes")
            return False
        detail(f"{size} bytes, the call said {answer.get('bytes')}")
        check_ok()

        check_start("timewarp:load takes it back")
        self.put("load", {"path": path})
        check_ok()
        st = self.after_load(".c64re")
        if st is None:
            return False
        check_start("the ring starts over after a .c64re (it carries no ring)")
        frames = int(st.get("frames", 0))
        if frames > 60:
            check_fail(f"the ring still holds {frames} frames; a .c64re should leave it starting over")
            return False
        detail(f"{frames} frames")
        check_ok()
        return True

    def case_ring_file(self) -> bool:
        section("many frames (.c64rering)")
        path = self.files["ring"]
        self.fill_ring()
        check_start(f"timewarp:ringdump writes {path}")
        answer = self.put("ringdump", {"path": path, "seconds": 2, "stride": 25})
        entries = int(answer.get("entries", 0))
        if entries < 2:
            check_fail(f"the file has {entries} entries")
            return False
        detail(f"{entries} entries, {self.size_of(path)} bytes")
        check_ok()

        check_start("timewarp:load takes its cursor")
        self.put("load", {"path": path})
        check_ok()
        if self.after_load(".c64rering") is None:
            return False

        check_start("timewarp:load takes the oldest entry by #0")
        self.put("load", {"path": path, "entry": "#0"})
        check_ok()
        if self.after_load(".c64rering #0") is None:
            return False

        check_start("an entry that is not in the file is refused")
        self.put("load", {"path": path, "entry": "no-such-entry"}, expected=422)
        check_ok()
        return True

    def case_native(self) -> bool:
        section("the ring itself (.trxring)")
        path = self.files["native"]
        self.fill_ring(NATIVE_SECONDS)
        before = self.status()
        frames_before = int(before.get("frames", 0))
        if frames_before < 10:
            check_skip(f"the ring only holds {frames_before} frames: nothing to tell apart")
            return True

        check_start(f"timewarp:ringsave writes {path}")
        answer = self.put("ringsave", {"path": path})
        size = self.size_of(path)
        if size < 65536:
            check_fail(f"{path} is only {size} bytes; it has to carry the memories at least")
            return False
        detail(f"{size} bytes for {frames_before} frames, the call said {answer.get('bytes')}")
        check_ok()

        # Replace the machine with something else first, so the load below has
        # to bring the ring back rather than find it already there.
        check_start("a .c64re in between leaves the ring starting over")
        self.put("dump", {"path": self.files["snap"], "frames": 0})
        self.put("load", {"path": self.files["snap"]})
        st = self.status()
        if int(st.get("frames", 0)) > 60:
            check_fail(f"the ring still holds {st.get('frames')} frames after a .c64re")
            return False
        check_ok()

        check_start("timewarp:load brings the ring back")
        self.put("load", {"path": path})
        check_ok()
        st = self.after_load(".trxring")
        if st is None:
            return False
        check_start("the timeline is the one the file was written with")
        frames_after = int(st.get("frames", 0))
        if frames_after < frames_before - 2:
            check_fail(f"the ring holds {frames_after} frames, the file was written with {frames_before}")
            return False
        detail(f"{frames_after} frames back, {frames_before} were written")
        check_ok()

        check_start("rewinding works from there")
        self.put("rewind", {"frames": 5})
        check_ok()
        if self.after_load(".trxring and a rewind") is None:
            return False

        check_start("an entry is refused for a .trxring")
        self.put("load", {"path": path, "entry": "#0"}, expected=422)
        check_ok()
        return True

    def case_refusals(self) -> bool:
        section("refusals")
        check_start("a path that is not there is refused, not crashed into")
        self.put("load", {"path": f"{self.args.dir.rstrip(chr(47))}/t66_does_not_exist.c64re"},
                 expected=422)
        check_ok()
        check_start("the device still answers")
        reason = self.alive()
        if reason:
            check_fail(f"the device stopped answering: {reason}")
            return False
        check_ok()
        return True

    # -- the suite ----------------------------------------------------------
    def run(self) -> bool:
        section("preconditions")
        check_start("device reachable")
        reason = self.alive()
        if reason:
            check_fail(f"the device did not answer before the suite started: {reason}")
            return False
        check_ok()

        check_start("this bitstream has the frame ring")
        code, _, body = self.api.rest.request("GET", "/v1/timewarp:status", idempotent=True)
        if code == 503:
            check_skip("no frame ring in this bitstream")
            return True
        if code != 200:
            check_fail(f"timewarp:status answered HTTP {code}: {body[:160]!r}")
            return False
        st = rest_lib.json_object("timewarp:status", body)
        if st.get("reason"):
            check_skip(str(st["reason"]))      # A08: the REU is on
            return True
        check_ok()

        self.fill_ring()
        check_start("the ring is filling")
        st = self.status()
        if not st.get("on") or int(st.get("frames", 0)) < 2:
            check_fail(f"the ring did not fill: {st}")
            return False
        detail(f"{st.get('frames')} frames, {int(st.get('seconds_x100', 0)) / 100:.2f} s")
        check_ok()

        ok = self.case_snapshot()
        ok = self.case_ring_file() and ok
        ok = self.case_native() and ok
        ok = self.case_refusals() and ok
        return ok

    def cleanup(self) -> None:
        self.ftp_delete(self.files.values())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    cli.add_device_arguments(parser, password=None, colour=False)
    parser.add_argument("--dir", default=DEFAULT_DIR,
                        help="where to write the files (default %(default)s; a long ring needs a stick)")
    args = parser.parse_args()

    runner = SuiteRunner(args)
    try:
        passed = runner.run()
    except Failure as exc:
        suite_fail(SUITE, format_exception(exc))
        return 1
    except (OSError, TimeoutError, urllib.error.URLError) as exc:
        if rest_lib.looks_unreachable(exc):
            suite_fail(SUITE, f"connection failure: {format_exception(exc)}")
        else:
            suite_fail(SUITE, f"REST failure: {format_exception(exc)}")
        return 1
    finally:
        teardown_step("remove the files this run wrote", runner.cleanup)

    if passed:
        suite_ok(SUITE, f"{check_count()} checks")
        return 0
    suite_fail(SUITE, "see the failed check above")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
