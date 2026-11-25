#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A ue2emu session of trx64u.cor that stays up between commands, for UX pictures of the real application (test tooling).

    python3 -I tests/ui/emu_session.py start --cor <trx64u.cor> --app <app.elf> --work <dir> [--speed realtime|max] [--net user|none] [--hours N]
    python3 -I tests/ui/emu_session.py do --work <dir> "<control line>" ...        (S08: key f3, wait 500, png <path>, screen, type "x", button)
    python3 -I tests/ui/emu_session.py state --work <dir> [--keys k1,k2]           (GET /v1/ui/state, the UI service's snapshot)
    python3 -I tests/ui/emu_session.py stop --work <dir>

`start` builds the machine exactly as software/trx64u/tools/emu_ops.py does (its read_cor, run_image, make_sd, Control, wait_ready; the stick with
cores/<id>.cor and the core folder's ROMs, an empty SD card, --net user with REST and TCP 4312 forwarded), starts ue2emu in a session of its own,
waits for READY. of the core, writes the ports to <dir>/session.json and leaves the emulator running. The default speed is realtime: the online
client's timeouts (HTTP_CONNECT_MS, HTTP_IDLE_MS) count emulated milliseconds, so at --speed max a slow server would time out early.
`do` connects to the control port (ue2emu takes one client at a time and the next after it), sends the lines in order and prints what each answers.
"""
import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "software", "trx64u", "tools"))
import emu_ops  # noqa: E402  (the start of the machine is emu_ops.py's, not a copy)


def session_file(work):
    return os.path.join(work, "session.json")


def load(work):
    with open(session_file(work)) as f:
        return json.load(f)


def start(a):
    work = os.path.abspath(a.work)
    if os.path.exists(work) and os.listdir(work):
        sys.exit("start: %s exists and is not empty" % work)
    os.makedirs(work, exist_ok=True)
    man, recs, cor = emu_ops.read_cor(a.cor)
    cid, name, version = man["id"], man["name"], man["version"]
    image = emu_ops.run_image(a.app, man, recs[3], work)
    roms = os.path.join(work, "roms")
    import shutil
    shutil.copytree(a.roms, roms)
    stick = os.path.join(work, "stick")
    core = os.path.join(stick, "cores", cid, "roms")
    os.makedirs(core)
    for src, dst in emu_ops.CORE_ROMS:
        shutil.copyfile(os.path.join(roms, src), os.path.join(core, dst))
    with open(os.path.join(stick, "cores", cid + ".cor"), "wb") as f:
        f.write(cor)
    base = [a.ue2emu, "run"] + emu_ops.BOARD + ["--firmware", image, "--roms", roms, "--c64-roms", roms]
    sd = emu_ops.make_sd(base, work)
    ctl_port, rest_port, mon_port = emu_ops.free_ports(3)
    # --net none: no Ethernet at all (the error paths of a request; REST and `state` are then not reachable)
    net = ["--net", "user", "--hostfwd", "tcp:127.0.0.1:%d:80,tcp:127.0.0.1:%d:4312" % (rest_port, mon_port)] if a.net == "user" else []
    cmd = base + ["--flash", os.path.join(work, "flash.bin"), "--usb-dir", stick, "--usb-dir-work", os.path.join(work, "usbwork"),
                  "--sd", sd, "--headless", "--speed", a.speed, "--control", "127.0.0.1:%d" % ctl_port] + net + [
                  "--handover", "path=/Usb0/cores/%s.cor,core=%s,id=%s,version=%s" % (cid, name, cid, version),
                  "--max-seconds", str(int(a.hours * 3600))]
    log = os.path.join(work, "emu.log")
    with open(log, "w") as f:
        t0 = time.time()
        proc = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=work, start_new_session=True)
    with open(session_file(work), "w") as f:
        json.dump({"pid": proc.pid, "ctl": ctl_port, "rest": rest_port, "mon": mon_port, "log": log, "cmd": cmd}, f, indent=1)
    deadline = t0 + a.timeout
    ctl = emu_ops.Control(ctl_port, deadline, proc)
    emu_ops.wait_ready(ctl, proc, emu_ops.Log(log), name, t0, deadline)
    ctl.sock.close()                       # not ctl.close(): that sends quit
    print("up: pid %d, control %d, REST %d, monitor %d (%s)" % (proc.pid, ctl_port, rest_port, mon_port, session_file(work)))


def do(a):
    s = load(a.work)
    sock = socket.create_connection(("127.0.0.1", s["ctl"]), timeout=600)
    f = sock.makefile("rw", encoding="utf-8", errors="replace", newline="\n")
    rc = 0
    try:
        for line in a.lines:
            f.write(line + "\n")
            f.flush()
            out = []
            while True:
                got = f.readline()
                if not got:
                    raise SystemExit("the control port closed after %r" % line)
                got = got.rstrip("\n")
                if got == "ok" or got.startswith("error"):
                    break
                out.append(got)
            for o in out:
                print(o)
            if got != "ok":
                print("%s -> %s" % (line, got))
                rc = 1
                break
    finally:
        sock.close()
    sys.exit(rc)


def state(a):
    s = load(a.work)
    base = "http://127.0.0.1:%d" % s["rest"]
    tok = s.get("token")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def get():
        url = base + "/v1/ui/state" + ("?t=" + tok if tok else "")
        with opener.open(url, timeout=20) as r:
            return json.loads(r.read().decode("utf-8"))
    try:
        d = get()
    except urllib.error.HTTPError as e:
        if e.code != 401:
            raise
        req = urllib.request.Request(base + "/v1/ui/grant", data=b"", method="POST")
        with opener.open(req, timeout=20) as r:
            tok = json.loads(r.read().decode("utf-8"))["token"]
        s["token"] = tok                     # kept in the session file only (the emulator's own token, a throwaway machine)
        with open(session_file(a.work), "w") as f:
            json.dump(s, f, indent=1)
        d = get()
    if a.keys:
        d = {k: d.get(k) for k in a.keys.split(",")}
    print(json.dumps(d, indent=1))


def stop(a):
    s = load(a.work)
    try:
        sock = socket.create_connection(("127.0.0.1", s["ctl"]), timeout=10)
        sock.sendall(b"quit\n")
        sock.close()
    except OSError:
        pass
    time.sleep(2)
    try:
        os.killpg(s["pid"], signal.SIGTERM)
    except ProcessLookupError:
        pass
    print("stopped")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="what", required=True)
    st = sub.add_parser("start")
    st.add_argument("--cor", required=True)
    st.add_argument("--app", required=True)
    st.add_argument("--work", required=True)
    st.add_argument("--ue2emu", default=emu_ops.UE2EMU)
    st.add_argument("--roms", default=emu_ops.ROMS)
    st.add_argument("--speed", default="realtime", choices=("realtime", "max"))
    st.add_argument("--net", default="user", choices=("user", "none"))
    st.add_argument("--hours", type=float, default=3.0, help="ue2emu ends by itself after this many wall-clock hours")
    st.add_argument("--timeout", type=int, default=300)
    d = sub.add_parser("do")
    d.add_argument("--work", required=True)
    d.add_argument("lines", nargs="+")
    s = sub.add_parser("state")
    s.add_argument("--work", required=True)
    s.add_argument("--keys")
    sp = sub.add_parser("stop")
    sp.add_argument("--work", required=True)
    a = p.parse_args()
    {"start": start, "do": do, "state": state, "stop": stop}[a.what](a)


if __name__ == "__main__":
    main()
