#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""U20 B: the scripts that read and write the builds repository, run against a fake GitHub API on 127.0.0.1 (no network, no token):
ci/order.py (missing, published, draft), ci/fetch_bits.py (a good entry, the cache record reused, a wrong digest, a SHA256SUMS that lists a missing asset,
a draft), ci/publish_release.py (a ref that publishes nothing, a tag without our prefix, a pull request event, main: draft -> published, the same files
again, other files refused), tools/builds.py fetch (a good asset, a wrong digest).

  python3 -I ci/test_builds.py"""
import hashlib
import http.server
import io
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import threading
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REPO = "owner/builds"
KEY = "c64u-0123456789abcdef"


class Fake:
    def __init__(self):
        self.releases, self.next_id, self.lock = {}, 1, threading.Lock()

    def add(self, tag, files, draft=False, bad_digest=None, extra_sums=None):
        with self.lock:
            rid = self.next_id
            self.next_id += 1
            assets = []
            sums = "".join("%s  %s\n" % (hashlib.sha256(b).hexdigest(), n) for n, b in sorted(files.items()))
            sums += "".join("%s  %s\n" % ("0" * 64, n) for n in extra_sums or [])
            allf = dict(files, SHA256SUMS=sums.encode())
            for n, b in allf.items():
                d = hashlib.sha256(b).hexdigest() if n != bad_digest else "f" * 64
                assets.append({"name": n, "digest": "sha256:" + d, "data": b})
            self.releases[rid] = {"id": rid, "tag_name": tag, "draft": draft, "assets": assets}
            return rid

    def view(self, r, base):
        out = {k: v for k, v in r.items() if k != "assets"}
        out["assets"] = [{"name": a["name"], "digest": a["digest"], "browser_download_url": "%s/dl/%d/%s" % (base, r["id"], a["name"])} for a in r["assets"]]
        out["upload_url"] = "%s/uploads/%d{?name,label}" % (base, r["id"])
        return out


def serve(fake):
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, code, obj=None, raw=None):
            body = raw if raw is not None else (json.dumps(obj).encode() if obj is not None else b"")
            self.send_response(code)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def base(self):
            return "http://127.0.0.1:%d" % self.server.server_port

        def body(self):
            n = int(self.headers.get("Content-Length") or 0)
            return self.rfile.read(n) if n else b""

        def do_GET(self):
            u = urllib.parse.urlparse(self.path)
            m = re.match(r"^/repos/%s/releases/tags/(.+)$" % REPO, u.path)
            if m:
                tag = urllib.parse.unquote(m.group(1))
                for r in fake.releases.values():
                    if r["tag_name"] == tag:
                        return self.send(200, fake.view(r, self.base()))
                return self.send(404, {"message": "Not Found"})
            m = re.match(r"^/repos/%s/releases/(\d+)$" % REPO, u.path)
            if m and int(m.group(1)) in fake.releases:
                return self.send(200, fake.view(fake.releases[int(m.group(1))], self.base()))
            if u.path == "/repos/%s/releases" % REPO:
                return self.send(200, [fake.view(r, self.base()) for r in fake.releases.values()])
            if u.path == "/repos/%s" % REPO:
                return self.send(200, {"default_branch": "main"})
            m = re.match(r"^/dl/(\d+)/(.+)$", u.path)
            if m and int(m.group(1)) in fake.releases:
                for a in fake.releases[int(m.group(1))]["assets"]:
                    if a["name"] == m.group(2):
                        return self.send(200, raw=a["data"])
            return self.send(404, {"message": "Not Found"})

        def do_POST(self):
            u = urllib.parse.urlparse(self.path)
            if u.path == "/repos/%s/releases" % REPO:
                d = json.loads(self.body())
                rid = fake.add(d["tag_name"], {}, draft=d.get("draft", False))
                fake.releases[rid]["assets"] = []
                return self.send(201, fake.view(fake.releases[rid], self.base()))
            m = re.match(r"^/uploads/(\d+)$", u.path)
            if m:
                name = urllib.parse.parse_qs(u.query)["name"][0]
                data = self.body()
                a = {"name": name, "digest": "sha256:" + hashlib.sha256(data).hexdigest(), "data": data}
                fake.releases[int(m.group(1))]["assets"].append(a)
                return self.send(201, {"name": name, "digest": a["digest"]})
            return self.send(404, {})

        def do_PATCH(self):
            m = re.match(r"^/repos/%s/releases/(\d+)$" % REPO, self.path)
            d = json.loads(self.body())
            r = fake.releases[int(m.group(1))]
            r["draft"] = d.get("draft", r["draft"])
            return self.send(200, fake.view(r, self.base()))

        def do_DELETE(self):
            m = re.match(r"^/repos/%s/releases/(\d+)$" % REPO, self.path)
            fake.releases.pop(int(m.group(1)), None)
            return self.send(204)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def run(args, env):
    e = {"PATH": os.environ.get("PATH", ""), "HOME": os.environ.get("HOME", "")}
    e.update(env)
    return subprocess.run([sys.executable, "-I"] + args, capture_output=True, text=True, env=e, cwd=ROOT)


def reports_tar():
    b = io.BytesIO()
    with tarfile.open(fileobj=b, mode="w:gz") as t:
        data = b"| Slice LUTs | 100 |\n"
        ti = tarfile.TarInfo("c64_basic.utilization.rpt")
        ti.size = len(data)
        t.addfile(ti, io.BytesIO(data))
    return b.getvalue()


def main():
    fake = Fake()
    srv = serve(fake)
    api = "http://127.0.0.1:%d" % srv.server_port
    errs = []

    def expect(label, r, rc, text=None):
        ok = r.returncode == rc and (text is None or text in r.stdout + r.stderr)
        print("%s %s" % ("ok  " if ok else "FAIL", label))
        if not ok:
            errs.append(label)
            print("     rc %d, want %d%s\n     %s" % (r.returncode, rc, "" if text is None else ", want text %r" % text, (r.stdout + r.stderr).strip()[-400:]))

    with tempfile.TemporaryDirectory() as td:
        keys = os.path.join(td, "keys")
        open(keys, "w").write(KEY + "\n")
        plan = os.path.join(td, "plan.json")
        json.dump({"version": 1}, open(plan, "w"))
        base = {"MC_BUILDS_REPO": REPO, "BUILDS_TAG_PREFIX": "fw-", "BUILDS_API": api, "PUBLISH_API": api}

        # ci/order.py
        wo = os.path.join(td, "wo.json")
        oenv = dict(base, ORDER_KEYS=keys, PLAN_JSON_FILE=plan, WORK_ORDER_OUT=wo, ORDER_SHA="a" * 40)
        r = run(["ci/order.py"], oenv)
        expect("order: no release -> missing", r, 0, "1 of 1 needed entries are missing: " + KEY)
        if r.returncode == 0 and json.load(open(wo)) != {"version": 1, "sha": "a" * 40, "lanes": ["bits:c64_basic"], "entries": [KEY]}:
            errs.append("order: work-order.json"); print("FAIL order: work-order.json %s" % open(wo).read())
        rid = fake.add("fw-bit-" + KEY, {"c64_basic.bit": b"BIT", "from": b"abcd1234\n"}, draft=True)
        expect("order: a draft -> missing", run(["ci/order.py"], oenv), 0, "1 of 1 needed entries are missing")
        fake.releases[rid]["draft"] = False
        expect("order: published -> none missing", run(["ci/order.py"], oenv), 0, "0 of 1 needed entries are missing")
        expect("order: another prefix -> missing", run(["ci/order.py"], dict(oenv, BUILDS_TAG_PREFIX="xx-")), 0, "1 of 1 needed")
        open(os.path.join(td, "badkeys"), "w").write("host-0123456789abcdef\n")
        expect("order: a name that is not ours", run(["ci/order.py"], dict(oenv, ORDER_KEYS=os.path.join(td, "badkeys"))), 1, "not an entry name")

        # ci/fetch_bits.py
        fake.releases.pop(rid)
        fake.add("fw-bit-" + KEY, {"c64_basic.bit": b"BIT", "from": b"abcd1234\n", "reports.tar.gz": reports_tar()})
        cache = os.path.join(td, "bit")
        fenv = dict(base, FETCH_KEYS=keys, CI_BIT_CACHE=cache, CI_HANDOFF=os.path.join(td, "ho"))
        r = run(["ci/fetch_bits.py"], fenv)
        expect("fetch: a good entry", r, 0, "fetched fw-bit-" + KEY)
        e = os.path.join(cache, KEY)
        if not (os.path.isfile(os.path.join(e, "DONE")) and open(os.path.join(e, "c64_basic.bit"), "rb").read() == b"BIT"
                and os.path.isfile(os.path.join(e, "c64_basic.utilization.rpt"))):
            errs.append("fetch: the entry on disk"); print("FAIL fetch: the entry on disk: %s" % (os.listdir(e) if os.path.isdir(e) else "none"))
        expect("fetch: the cache record is used again", run(["ci/fetch_bits.py"], fenv), 0, "cached  fw-bit-" + KEY)
        open(os.path.join(e, "c64_basic.bit"), "wb").write(b"OTHER")
        expect("fetch: a changed cached file is fetched again", run(["ci/fetch_bits.py"], fenv), 0, "fetched fw-bit-" + KEY)
        expect("fetch: --names", run(["ci/fetch_bits.py", "--names"], fenv), 0, KEY)
        for label, kw, text in (("a wrong digest", {"bad_digest": "c64_basic.bit"}, "GitHub lists"),
                                ("SHA256SUMS lists a missing asset", {"extra_sums": ["gone.bit"]}, "SHA256SUMS lists"),
                                ("a draft", {"draft": True}, "is a draft")):
            fake.releases.clear()
            fake.add("fw-bit-" + KEY, {"c64_basic.bit": b"BIT"}, **kw)
            c2 = os.path.join(td, "bit-" + label.replace(" ", "_"))
            r = run(["ci/fetch_bits.py"], dict(fenv, CI_BIT_CACHE=c2))
            expect("fetch: %s is refused" % label, r, 1, text)
            if os.path.isdir(os.path.join(c2, KEY)):
                errs.append("fetch: %s left an entry" % label); print("FAIL fetch: %s left an entry" % label)

        # tools/builds.py fetch
        fake.releases.clear()
        fake.add("sdk-0.0.1-00000000", {"trx64-sdk-0.0.1.tar.gz": b"TAR"})
        dest = os.path.join(td, "sdk.tgz")
        expect("builds: fetch a good asset", run(["tools/builds.py", "fetch", "sdk-0.0.1-00000000", "trx64-sdk-0.0.1.tar.gz", dest], base), 0, "fetched")
        fake.releases.clear()
        fake.add("sdk-0.0.1-00000000", {"trx64-sdk-0.0.1.tar.gz": b"TAR"}, bad_digest="trx64-sdk-0.0.1.tar.gz")
        expect("builds: a wrong digest is refused", run(["tools/builds.py", "fetch", "sdk-0.0.1-00000000", "trx64-sdk-0.0.1.tar.gz", os.path.join(td, "x")], base),
               1, "refused")
        if os.path.exists(os.path.join(td, "x")):
            errs.append("builds: the refused file was written"); print("FAIL builds: the refused file was written")

        # ci/publish_release.py
        fake.releases.clear()
        st = os.path.join(td, "stage")
        os.makedirs(st)
        open(os.path.join(st, "trx64u.cor"), "wb").write(b"COR")
        open(os.path.join(st, "manifest.json"), "w").write("{}\n")
        with open(os.path.join(st, "SHA256SUMS"), "w") as f:
            for n in ("manifest.json", "trx64u.cor"):
                f.write("%s  %s\n" % (hashlib.sha256(open(os.path.join(st, n), "rb").read()).hexdigest(), n))
        sha = "b" * 40
        penv = dict(base, MC_SRC_REPO="owner/src", BUILDS_TOKEN="t", GITHUB_ACTIONS="true", GITHUB_EVENT_NAME="push", GITHUB_REPOSITORY="owner/src",
                    GITHUB_SHA=sha, CI_BRANCH="main", CI_REF_TYPE="branch", GITHUB_REF="refs/heads/main")
        pub = ["ci/publish_release.py", "--dir", st, "--sha", sha, "--tag"]
        expect("publish: a feature branch publishes nothing", run(pub + ["fw-t1-bbbbbbbb"], dict(penv, CI_BRANCH="t1", GITHUB_REF="refs/heads/t1")), 0,
               "nothing is published")
        expect("publish: a tag without our prefix", run(pub + ["main-bbbbbbbb"], penv), 1, "prefix")
        expect("publish: a pull request event", run(pub + ["fw-main-bbbbbbbb"], dict(penv, GITHUB_EVENT_NAME="pull_request")), 1, "push only")
        expect("publish: another repository's run", run(pub + ["fw-main-bbbbbbbb"], dict(penv, GITHUB_REPOSITORY="fork/src")), 1, "belongs to")
        expect("publish: main without a token", run(pub + ["fw-main-bbbbbbbb"], dict(penv, BUILDS_TOKEN="")), 1, "BUILDS_TOKEN")
        r = run(pub + ["fw-main-bbbbbbbb"], penv)
        expect("publish: main", r, 0, "published fw-main-bbbbbbbb")
        rel = [x for x in fake.releases.values() if x["tag_name"] == "fw-main-bbbbbbbb"]
        if len(rel) != 1 or rel[0]["draft"] or sorted(a["name"] for a in rel[0]["assets"]) != ["SHA256SUMS", "manifest.json", "trx64u.cor"]:
            errs.append("publish: the release"); print("FAIL publish: the release %s" % rel)
        expect("publish: the same files again", run(pub + ["fw-main-bbbbbbbb"], penv), 0, "already published with exactly these files")
        open(os.path.join(st, "trx64u.cor"), "wb").write(b"COR2")
        expect("publish: a changed SHA256SUMS is refused", run(pub + ["fw-main-bbbbbbbb"], penv), 1, "SHA256SUMS lists")
        with open(os.path.join(st, "SHA256SUMS"), "w") as f:
            for n in ("manifest.json", "trx64u.cor"):
                f.write("%s  %s\n" % (hashlib.sha256(open(os.path.join(st, n), "rb").read()).hexdigest(), n))
        expect("publish: other files under a published tag are refused", run(pub + ["fw-main-bbbbbbbb"], penv), 1, "immutable")
        expect("publish: a v* tag", run(pub + ["fw-v0.4.0-bbbbbbbb"], dict(penv, CI_BRANCH="v0.4.0", CI_REF_TYPE="tag", GITHUB_REF="refs/tags/v0.4.0")), 0,
               "published fw-v0.4.0-bbbbbbbb")
    srv.shutdown()
    print("test_builds: %s" % ("%d failed: %s" % (len(errs), "; ".join(errs)) if errs else "all passed"))
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
