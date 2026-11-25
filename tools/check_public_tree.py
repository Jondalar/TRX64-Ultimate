#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# PROVENANCE: TRX64U-MultiCore tools/check_public_tree.py (t239.3 72e770261, last changed in 6e2807103; Alex Damhuis' code in both repositories), U20 A8.
# Changed here: our path rules (the four ROMs of ci/roms.txt, D14); ci/private.txt is made from the pins of ci/roms.txt (no side repository, so no
# --side and no side lines); two text rules more (a NAS share path, a personal mail address); messages pass the Claude-Session trailer this repository's
# commits carry, --authors (the pre-push hook to GitHub) also checks the author and committer address, --trailers is gone; scrub() rewrites every hit
# into a placeholder for tools/gh_export.py (the replay and the one rewrite of U20 D15), outside the third-party trees of SCRUB_KEEP; GITEA_KEPT lists
# the hits the Gitea runners still need on u18.
"""Public-tree check (U20 A8; MultiCore T239.1 D24, S1, D6). GitHub cannot refuse a push by path or content and a pushed file stays in the
history, so this runs before every push (.githooks/pre-push), in CI on every push and pull request (.github/workflows/public-tree.yml) and in
ci/lint.sh (group tree).

  tools/check_public_tree.py                     tree mode: every tracked file of the working tree
  tools/check_public_tree.py --rev REV           tree mode on the commit REV (what the pre-push hook checks)
  tools/check_public_tree.py --messages "RANGE"  message mode: the commit messages of `git rev-list RANGE` (one quoted argument); with --authors
                                                 also each commit's author and committer address (the pre-push hook to GitHub)
  tools/check_public_tree.py --commits "RANGE"   commits mode: every (path, blob) that a commit of `git rev-list RANGE` adds or changes, so a
                                                 private file added in one commit and removed in a later one is found, too
  tools/check_public_tree.py --files PATH...     files mode: directories, .cor files and the members of .zip and .tar.gz against the file and
                                                 window lines of ci/private.txt and the text rules (tools/release_trx64u.sh --publish)
  tools/check_public_tree.py --selftest          plants one hit per rule with generic examples; a rule that does not fire fails the selftest
  tools/check_public_tree.py --gen-private DIR   write ci/private.txt from the ROMs of ci/roms.txt found at DIR/<pinned path> (e.g. after
                                                 ci/roms.py fetch --root DIR); their bytes stay there

Tree mode refuses a path of the private list (the ROM images of D14, ROM and disk image types, session material under .claude/), a file of a known
size with a known SHA-256 under any name, a 64-byte window of a private file anywhere in a blob, and in every blob without a NUL in its first 8000
bytes a private-network address, a home path, an internal host name, a login on a host without a dot, a NAS share path, a personal mail address or a
claude.ai link. The text rules are generic forms that need no list of values. A symlink counts with its path and its target. Exit 1 on any hit, every
hit printed."""
import argparse, hashlib, io, os, random, re, shlex, subprocess, sys, tarfile, tempfile, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRIVATE = os.path.join(ROOT, "ci", "private.txt")
ROMS = os.path.join(ROOT, "ci", "roms.txt")

# ---- path rules ----
DROP_RE = re.compile(r"^roms/.*\.bin$")                         # D14: the bench ROMs, fetched by ci/roms.py, never tracked
ROM_EXT = re.compile(r"\.(rom|img|adf|adz|d64|d71|d81|g64|crt|tap|tzx|t64|dsk|hdf|nib|po)$", re.I)
CLAUDE_PUBLIC = ("CLAUDE.md", ".claude/agents/", ".claude/skills/")     # MultiCore D5's whitelist; everything else under .claude/ is private

# ---- text rules: generic forms, no list of values ----
# Not private: the documentation examples (192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24 are outside every private range) and the placeholders that
# scrub() writes: ~/ for a home path, <host>.example for a host, <user>@<host> for a login, /srv for a NAS share, <mail> for a personal address.
M32 = 0xFFFFFFFF
_oct = r"(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)"
_EDGE_L, _EDGE_R = rb"(?<![\w.])", rb"(?![\w-])(?!\.\d)"
_MAILERS = r"icloud|me|mac|gmail|googlemail|gmx|web|outlook|hotmail|live|msn|yahoo|aol|t-online|freenet|arcor|posteo|mailbox|proton|protonmail|yandex"
RULES = (   # (name in the report, regex, exclusions: values of the first group that pass)
    ("private address", re.compile(_EDGE_L + (r"(?:10\.%s\.%s\.%s|172\.(?:1[6-9]|2\d|3[01])\.%s\.%s|192\.168\.%s\.%s|169\.254\.%s\.%s)" % (
        (_oct,) * 3 + (_oct,) * 2 + (_oct,) * 2 + (_oct,) * 2)).encode() + _EDGE_R), ()),
    ("home path", re.compile(rb"(?<![\w.-])/(?:Users|home)/([A-Za-z0-9._-]+)/"), (b"x", b"user", b"you", b"name", b"username", b"runner")),
    ("internal host", re.compile(rb"(?<![\w.-])((?:[a-z0-9-]+\.)+(?:box|lan|local|home\.arpa))(?![\w-])(?!\.[A-Za-z0-9])", re.I), ()),
    ("login on an internal host", re.compile(rb"(?<![\w.@-])[a-z][a-z0-9._-]{2,}@(?![0-9a-f]{6,}(?![\w-]))([A-Za-z][A-Za-z0-9-]{2,})(?![\w-])(?!\.[A-Za-z0-9])"),
     (b"host", b"hostname", b"localhost", b"sha", b"sha256", b"address", b"refs", b"example")),
    # a share of a NAS at the root (QNAP: /share/CACHEDEV1_DATA, /share/Container); /usr/share, $(PREFIX)/share and a URL path are no hit
    ("NAS share path", re.compile(rb"(?<![\w.~/})$%-])/share/([A-Za-z0-9_]+)"), ()),
    ("personal mail address", re.compile((r"(?<![\w.+-])[\w.+-]+@(?:%s)\.(?:com|de|net|org|ch|at|me|fr|nl|co\.uk)(?![\w-])" % _MAILERS).encode(), re.I), ()),
    ("claude.ai link", re.compile(rb"\bclaude\.ai/", re.I), ()),
)
# This repository's commits end with the session line of the attribution (U20 A8: GitHub main has carried it since the export of 0.2.0); in a message
# that one line passes, any other claude.ai link is a hit.
SESSION_TRAILER = re.compile(rb"^Claude-Session: https://claude\.ai/code/session_[A-Za-z0-9]+[ \t]*$", re.M)

# Narrow, named exceptions: (reason, rule, paths). Each path is one file; nothing is allowed by folder or by pattern. A new hit in any other file is red.
_ADDR = "private address"
_TEXT_ALLOW = (
    ("vendor default of the ESP32 network settings (192.168.2.x) and its documentation, mock-ups of the settings screen", _ADDR, (
        "software/trx64u/base/menu.toml", "docs/ultimate-app-hw-spec.md", "docs/lvgl_ui_project/screens/settings_network.xml",
        "docs/lvgl_ui_terminal/screens/settings_network.xml")),
    ("QEMU/SLIRP gateway address (10.0.2.2) of the emulator, in a bench comment", _ADDR, ("designs/c64_basic/sim/tb_udp_audio.v",)),
    ("the Ultimate's path form cartridge@computer of the U2 REST routes, no login", "login on an internal host", ("docs/specs/T71-robot-tests.md",)),
    ("the workflow lint's selftest plants an internal host form", "internal host", ("tools/check_workflows.py",)),
    ("the workflow lint's selftest plants a private address", _ADDR, ("tools/check_workflows.py",)),
    ("the author's contact address in the header of a third-party file (Gideon's 1541ultimate at 1ba94abd, vendor/gideon/VENDOR.md; the icepi "
     "C64 core's drive and tape files), public upstream", "personal mail address", (
        "cores/icepi/c64_c1541/c1541_drv.v", "cores/icepi/c64_c1541/c1541_gcr.v", "cores/icepi/c64_c1541/iecdrv_via6522.vhd",
        "cores/icepi/c64_tape.v", "vendor/gideon/fpga/1541/vhdl_source/floppy.vhd", "vendor/gideon/fpga/1541/vhdl_source/floppy_mem.vhd",
        "vendor/gideon/fpga/1541/vhdl_source/floppy_param_mem.vhd", "vendor/gideon/fpga/1541/vhdl_source/floppy_stream.vhd",
        "vendor/gideon/fpga/1541/vhdl_source/via6522.vhd", "vendor/gideon/fpga/altera/distributed_stack.vhd",
        "vendor/gideon/fpga/io/rmii/vhdl_source/crc32.vhd", "vendor/gideon/fpga/io/rmii/vhdl_source/eth_filter.vhd",
        "vendor/gideon/fpga/io/rmii/vhdl_source/eth_transmit.vhd", "vendor/gideon/fpga/io/rmii/vhdl_source/ethernet_interface.vhd",
        "vendor/gideon/fpga/io/rmii/vhdl_source/ethernet_rmii.vhd", "vendor/gideon/fpga/io/rmii/vhdl_source/rmii_transceiver.vhd",
        "vendor/gideon/fpga/io/uart_lite/vhdl_source/rx.vhd", "vendor/gideon/fpga/io/uart_lite/vhdl_source/tx.vhd",
        "vendor/gideon/fpga/io/usb2/vhdl_source/data_crc.vhd", "vendor/gideon/fpga/io/usb2/vhdl_source/token_crc.vhd",
        "vendor/gideon/fpga/io/usb2/vhdl_source/token_crc_19.vhd", "vendor/gideon/fpga/io/usb2/vhdl_source/token_crc_check.vhd",
        "vendor/gideon/fpga/ip/async_fifo/vhdl_source/async_fifo_ft.vhd", "vendor/gideon/fpga/ip/async_fifo/vhdl_source/gray_code_pkg.vhd",
        "vendor/gideon/fpga/ip/free_queue/vhdl_source/block_bus_pkg.vhd", "vendor/gideon/fpga/ip/free_queue/vhdl_source/free_queue.vhd",
        "vendor/gideon/fpga/ip/srl_fifo/vhdl_source/srl_fifo-xilinx.vhd", "vendor/gideon/fpga/ip/synchroniser/vhdl_source/synchronizer_gzw.vhd",
        "vendor/gideon/fpga/ip/video/vhdl_source/char_generator_pkg.vhd", "vendor/gideon/fpga/ip/video/vhdl_source/char_generator_regs.vhd",
        "vendor/gideon/fpga/ip/video/vhdl_source/char_generator_slave12.vhd")),
    ("this tool and its test: the planted examples of the selftest (private ranges, made-up accounts, host names, paths) are generic values",
     "*", ("tools/check_public_tree.py", "tools/test_check_public_tree.py")),
    ("the export's tests plant the same generic examples for its scrub and rewrite", "*", ("tools/test_gh_export.py",)),
)
TEXT_ALLOW = {(p, r): why for why, r, ps in _TEXT_ALLOW for p in ps}
# Hits the Gitea runners needed on u18 (the host path of their cache volume) passed here while the replay wrote the placeholder into the public
# copy (U20 D15). Part E emptied this list when GitHub became the home (2026-11-25); the mechanism stays for the selftest.
_GITEA_KEPT = ()
GITEA_KEPT = {(p, r): why for why, r, ps in _GITEA_KEPT for p in ps}


def _matches(rule, data):
    name, rx, allowed = rule
    return [m for m in rx.finditer(data) if not (rx.groups and m.group(1).lower() in allowed)]


def rule_fires(rule, data, path=None, kept=True):
    """Whether the rule matches data. rule[2] lists the values of the rule's first group that are placeholders or the GitHub runner account.
    kept=False: the GITEA_KEPT pairs fire, too (what the replay must rewrite)."""
    name = rule[0]
    if (path, name) in TEXT_ALLOW or (path, "*") in TEXT_ALLOW or (kept and (path, name) in GITEA_KEPT):
        return False
    return bool(_matches(rule, data))


def path_hits(path):
    hits = []
    if DROP_RE.match(path) or ROM_EXT.search(path):
        hits.append("private file type (ROM image or medium)")
    if path == "CLAUDE.local.md":
        hits.append("private file (the maintainer's local session notes)")
    if path.startswith(".claude/") and not any(path.startswith(w) for w in CLAUDE_PUBLIC):
        hits.append("session material outside the whitelist (.claude/agents, .claude/skills)")
    hits += ["%s in the path" % r[0] for r in RULES if rule_fires(r, path.encode("utf-8", "surrogateescape"), path)]
    return hits


def text_hits(data, path=None, rules=RULES):
    if b"\0" in data[:8000]:
        return []
    return [r[0] for r in rules if rule_fires(r, data, path)]


def kept_hits(data, path):
    """The GITEA_KEPT rules that match in this file (reported as kept, never red)."""
    if b"\0" in data[:8000]:
        return []
    return [r[0] for r in RULES if (path, r[0]) in GITEA_KEPT and _matches(r, data)]


# ---- scrub: every hit into its placeholder (tools/gh_export.py: the replay and the one rewrite, U20 D15) ----
def _addr(m):
    return b"192.0.2." + m.group(0).rsplit(b".", 1)[1]           # the documentation range of RFC 5737, the last octet kept (MultiCore D6's form)


def _share(m):
    return b"/srv" if re.fullmatch(rb"CACHEDEV\d+_DATA", m.group(1)) else b"/srv/" + m.group(1)


_CLAUDE_URL = re.compile(rb"(?:https?://)?(?:www\.)?claude\.ai/[^\s)\]>\"'`,;]*", re.I)
SCRUB = {
    "private address": _addr,
    "home path": lambda m: b"~/",
    "internal host": lambda m: m.group(1).split(b".", 1)[0] + b".example",
    "login on an internal host": lambda m: b"<user>@<host>",
    "NAS share path": _share,
    "personal mail address": lambda m: b"<mail>",
    "claude.ai link": lambda m: b"(a private page)",
}


# Third-party trees the scrub never rewrites: what the rules find there is upstream's own public text (an author's address, an example address,
# a member like threading.local() or self.local, the token cartridge@computer, the upstream author's home path in a comment), and a placeholder
# would change upstream's file, even break its code. The check still reports such a hit; it needs an exact TEXT_ALLOW entry or a decision.
# software/ultimate-app/ is the firmware fork the export flattened before U20 A5 (it exists in the history only).
SCRUB_KEEP = ("vendor/", "cores/icepi/", "software/ultimate-app/")


def scrub(data, path=None, message=False):
    """(new bytes, names of the rules that rewrote something). A binary (a NUL in the first 8000 bytes), the TEXT_ALLOW pairs and the files of
    SCRUB_KEEP stay as they are; GITEA_KEPT pairs are rewritten (the public copy needs the placeholder). message=True keeps the Claude-Session
    trailer."""
    if b"\0" in data[:8000] or (path and path.startswith(SCRUB_KEEP)):
        return data, []
    keep = {}
    if message:
        def hold(m):
            keep[b"\x00%d\x00" % len(keep)] = m.group(0)
            return b"\x00%d\x00" % (len(keep) - 1)
        data = SESSION_TRAILER.sub(hold, data)
    done = []
    for rule in RULES:
        name, rx, allowed = rule
        if (path, name) in TEXT_ALLOW or (path, "*") in TEXT_ALLOW:
            continue
        if name == "claude.ai link":
            new = _CLAUDE_URL.sub(SCRUB[name], data)
        else:
            new = rx.sub(lambda m: m.group(0) if rx.groups and m.group(1).lower() in allowed else SCRUB[name](m), data)
        if new != data:
            done.append(name)
            data = new
    for k, v in keep.items():
        data = data.replace(k, v)
    return data, done


def message_text(msg):
    """The part of a commit message the rules read: the Claude-Session trailer line taken out."""
    return SESSION_TRAILER.sub(b"", msg)


# ---- ci/private.txt: size + SHA-256 per private binary, windows of 64 bytes as hashes only (no file bytes) ----
# A window line is "win <rolling hash, 8 hex> <SHA-256 of the 64 bytes> <file>". The rolling hash is a 32-bit polynomial hash over every offset of
# a scanned blob (prefilter), the SHA-256 of the 64 bytes at a candidate offset confirms it.
WIN, NWIN = 64, 4
RP = 0x9E3779B1                      # odd, so it has an inverse modulo 2**32
RPINV, RPW = pow(RP, -1, 1 << 32), pow(RP, WIN - 1, 1 << 32)
RPOWS = [pow(RP, k, 1 << 32) for k in range(WIN)]


def roll_of(w):
    return sum(b * RPOWS[k] for k, b in enumerate(w)) & M32


def load_private(path=PRIVATE):
    files, wins = {}, {}          # files: size -> [(sha, name)], wins: rolling hash -> [(sha, name)]
    if not os.path.exists(path):
        return files, wins
    for ln in open(path):
        f = ln.split()
        if not f or f[0].startswith("#"):
            continue
        if f[0] == "file" and len(f) == 4:
            files.setdefault(int(f[1]), []).append((f[2], f[3]))
        elif f[0] == "win" and len(f) == 4:
            wins.setdefault(int(f[1], 16), []).append((f[2], f[3]))
        else:
            raise SystemExit("%s: bad line: %s" % (path, ln.strip()))
    return files, wins


def windows_of(data, taken=lambda w: False):
    """NWIN windows of 64 bytes spread over the file, each with at least 16 distinct byte values and not taken(w) (a window that a public file
    holds, too, would flag that file)."""
    out, n = [], len(data)
    if n < WIN:
        return out
    for k in range(NWIN):
        start = (n - WIN) * (2 * k + 1) // (2 * NWIN)
        for off in range(start, n - WIN + 1):
            w = data[off:off + WIN]
            if len(set(w)) >= 16 and not taken(w) and all(off != o for o, _ in out):
                out.append((off, w))
                break
    return out


def pinned_roms(roms=ROMS):
    """(sha256, size, path) of every pin of ci/roms.txt."""
    out = []
    for ln in open(roms, encoding="utf-8"):
        f = ln.split()
        if f and not f[0].startswith("#") and len(f) == 5:
            out.append((f[0], int(f[1]), f[2]))
    return out


def public_blobs(src):
    ls = subprocess.run(["git", "-C", src, "ls-files", "-z"], capture_output=True, check=True).stdout.decode().split("\0")
    for p in sorted(x for x in ls if x):
        fp = os.path.join(src, p)
        if os.path.isfile(fp) and not os.path.islink(fp):
            yield open(fp, "rb").read()


def gen_private(rom_dir, dest, roms=ROMS, tree=ROOT):
    rows = []
    for sha, size, p in pinned_roms(roms):
        d = open(os.path.join(rom_dir, p), "rb").read()
        if len(d) != size or hashlib.sha256(d).hexdigest() != sha:
            raise SystemExit("%s: not the file ci/roms.txt pins (size %d, sha256 %s)" % (p, len(d), hashlib.sha256(d).hexdigest()))
        rows.append((p, d))
    others = list(public_blobs(tree))
    taken = lambda w: any(w in b for b in others)
    out = ["# The private binary files (the ROM images of ci/roms.txt, U20 D14): size and SHA-256 per file, and per file a few windows of 64 bytes",
           "# as hashes only (a 32-bit rolling hash the scan finds at any offset, and the SHA-256 of the 64 bytes that confirms it). No file bytes are",
           "# stored here. Generated by tools/check_public_tree.py --gen-private DIR (DIR/<pinned path>, e.g. after ci/roms.py fetch --root DIR)."]
    for p, d in rows:
        out.append("file %d %s %s" % (len(d), hashlib.sha256(d).hexdigest(), p))
        for off, w in windows_of(d, taken):
            out.append("win %08x %s %s" % (roll_of(w), hashlib.sha256(w).hexdigest(), p))
    open(dest, "w").write("\n".join(out) + "\n")
    return len(rows)


def known_file(data, files):
    return [name for sha, name in files.get(len(data), []) if hashlib.sha256(data).hexdigest() == sha]


def roll_candidates(data, wins):
    """Offsets whose 64-byte rolling hash is one of the windows' (numpy when it is there, else a plain loop)."""
    n = len(data)
    if n < WIN or not wins:
        return []
    try:
        import numpy as np
    except ImportError:
        np = None
    if np is not None:
        h = np.frombuffer(data, dtype=np.uint8).astype(np.uint32)
        m = 1
        while m < WIN:                                   # h_2m[i] = h_m[i] + RP**m * h_m[i+m]
            h = h[:len(h) - m] + np.uint32(pow(RP, m, 1 << 32)) * h[m:]
            m *= 2
        return np.nonzero(np.isin(h, np.fromiter(wins, dtype=np.uint32, count=len(wins))))[0].tolist()
    out, cur = [], roll_of(data[:WIN])
    for i in range(n - WIN + 1):
        if cur in wins:
            out.append(i)
        if i + WIN < n:
            cur = ((cur - data[i]) * RPINV + data[i + WIN] * RPW) & M32
    return out


def window_hits(data, wins):
    hits = []
    for i in roll_candidates(data, wins):
        w = data[i:i + WIN]
        sha = hashlib.sha256(w).hexdigest()
        for wsha, name in wins.get(roll_of(w), ()):
            if wsha == sha:
                hits.append("%s at offset %d" % (name, i))
    return hits


# ---- tree mode ----
def tracked(rev, root=None):
    root = root or ROOT
    if rev:
        out = subprocess.run(["git", "-C", root, "ls-tree", "-r", "-z", rev], capture_output=True, check=True).stdout.decode("utf-8", "surrogateescape")
        return [(r.split("\t", 1)[1], r.split()[2], r.split()[0]) for r in out.split("\0") if r and r.split()[1] == "blob"]
    out = subprocess.run(["git", "-C", root, "ls-files", "-z"], capture_output=True, check=True).stdout.decode("utf-8", "surrogateescape")
    return [(p, None, None) for p in out.split("\0") if p]


def tree_items(rev, root=None):
    """(path, bytes[, link target]) of every tracked file; with --rev from the git objects through one cat-file process. A symlink gives its
    target as the bytes and as the third member, so its path and its target are both checked."""
    root = root or ROOT
    ents = tracked(rev, root)
    if not rev:
        for p, _, _ in ents:
            fp = os.path.join(root, p)
            if os.path.islink(fp):
                t = os.readlink(fp)
                yield p, t.encode("utf-8", "surrogateescape"), t
            elif os.path.isfile(fp):
                yield p, open(fp, "rb").read()
        return
    pr = subprocess.Popen(["git", "-C", root, "cat-file", "--batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    for p, sha, mode in ents:
        pr.stdin.write(sha.encode() + b"\n"); pr.stdin.flush()
        h = pr.stdout.readline().split()
        d = pr.stdout.read(int(h[2])); pr.stdout.read(1)
        yield (p, d, d.decode("utf-8", "surrogateescape")) if mode == "120000" else (p, d)
    pr.stdin.close(); pr.wait()


def link_path(p, target):
    """The path a symlink at p points to, relative to the tree root when the target is relative."""
    return os.path.normpath(target if os.path.isabs(target) else os.path.join(os.path.dirname(p), target))


def check_items(items, files, wins=(), kept=None):
    """The hits of the items; kept (a list) collects the GITEA_KEPT hits, which are no hits."""
    bad = []
    for it in items:
        p, d = it[0], it[1]
        if len(it) > 2:                                  # a symlink: its target is a path of the tree, too
            bad += ["%s: symlink target %s: %s" % (p, it[2], h) for h in path_hits(link_path(p, it[2])) + path_hits(it[2])]
        bad += ["%s: %s" % (p, h) for h in path_hits(p)]
        bad += ["%s: %s" % (p, h) for h in text_hits(d, p)]
        bad += ["%s: is a copy of the private file %s (size and SHA-256)" % (p, n) for n in known_file(d, files)]
        bad += ["%s: holds a window of %s" % (p, h) for h in (window_hits(d, wins) if wins else [])]
        if kept is not None:
            kept += ["%s: %s" % (p, h) for h in kept_hits(d, p)]
    return bad


# ---- message mode ----
def check_messages(rng, authors=False, root=None):
    root = root or ROOT
    revs = subprocess.run(["git", "-C", root, "rev-list"] + rng, capture_output=True, check=True).stdout.decode().split()
    bad = []
    for r in revs:
        m = subprocess.run(["git", "-C", root, "log", "-1", "--format=%B", r], capture_output=True, check=True).stdout
        bad += ["commit %s: %s in the message" % (r[:10], rule[0]) for rule in RULES if rule_fires(rule, message_text(m))]
        if authors:
            who = subprocess.run(["git", "-C", root, "log", "-1", "--format=%ae%n%ce", r], capture_output=True, check=True).stdout
            bad += ["commit %s: %s as author or committer" % (r[:10], rule[0]) for rule in RULES if rule_fires(rule, who)]
    return bad, len(revs)


# ---- commits mode: every (path, blob) a commit of the range adds or changes, also when a later commit removes it again ----
def commit_blobs(root, c):
    out = subprocess.run(["git", "-C", root, "diff-tree", "-r", "--root", "-m", "-z", "--no-commit-id", "--no-renames", "--diff-filter=AMRT", c],
                         capture_output=True, check=True).stdout.decode("utf-8", "surrogateescape").split("\0")
    ents = []
    for meta, path in zip(out[0::2], out[1::2]):
        f = meta.lstrip(":").split()
        if f[1] != "160000":
            ents.append((path, f[3], f[1] == "120000"))
    return ents


def check_commits(rng, files, wins, root=None):
    root = root or ROOT
    revs = subprocess.run(["git", "-C", root, "rev-list", "--reverse"] + rng, capture_output=True, check=True).stdout.decode().split()
    seen, todo = set(), []
    for c in revs:
        for path, blob, link in commit_blobs(root, c):
            if (path, blob) not in seen:
                seen.add((path, blob)); todo.append((c, path, blob, link))
    bad = []
    pr = subprocess.Popen(["git", "-C", root, "cat-file", "--batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    for c, path, blob, link in todo:
        pr.stdin.write(blob.encode() + b"\n"); pr.stdin.flush()
        h = pr.stdout.readline().split()
        d = pr.stdout.read(int(h[2])); pr.stdout.read(1)
        item = (path, d, d.decode("utf-8", "surrogateescape")) if link else (path, d)
        bad += ["commit %s: %s" % (c[:10], b) for b in check_items([item], files, wins)]
    pr.stdin.close(); pr.wait()
    return bad, len(revs), len(todo)


# ---- files mode: directories, .cor, .zip, .tar.gz members ----
def members(path):
    low = path.lower()
    if os.path.isdir(path):
        for d, _, fs in os.walk(path):
            for f in sorted(fs):
                yield from members(os.path.join(d, f))
    elif low.endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            for i in z.infolist():
                if not i.is_dir():
                    yield "%s!%s" % (path, i.filename), z.read(i)
    elif low.endswith((".tar.gz", ".tgz")):
        with tarfile.open(path, "r:gz") as t:
            for m in t:
                if m.isfile():
                    yield "%s!%s" % (path, m.name), t.extractfile(m).read()
    else:
        yield path, open(path, "rb").read()


def check_files(paths, files, wins):
    bad = []
    for p in paths:
        for name, d in members(p):
            bad += ["%s: is a copy of the private file %s" % (name, n) for n in known_file(d, files)]
            bad += ["%s: holds %s" % (name, h) for h in window_hits(d, wins)]
            bad += ["%s: %s" % (name, h) for h in text_hits(d, name)]
    return bad


# ---- selftest ----
# The planted values are generic examples of the rules' forms (private ranges of RFC 1918 and 3927, made-up accounts and host names, QNAP's share
# form), never a value of this project. The documentation ranges and the placeholders of scrub() must pass.
SYNTH = (("private address", "see 10.0.0.5\n"), ("private address", "http://172.16.4.4:8080/x\n"),
         ("private address", "ssh 192.168.1.20\n"), ("private address", "link 169.254.10.1.\n"),
         ("home path", "at /Users/jdoe/src/x\n"), ("home path", "at /home/jdoe/src/x\n"),
         ("internal host", "see devhost.lan\n"), ("internal host", "see nas.box/share\n"), ("internal host", "ping printer.local.\n"),
         ("internal host", "see ci.home.arpa\n"), ("login on an internal host", "ssh jdoe@devhost\n"), ("login on an internal host", "ssh someone@builder\n"),
         ("NAS share path", "-v /share/CACHEDEV9_DATA/x/cache:/cache\n"), ("NAS share path", "in (/share/Public/x)\n"),
         ("personal mail address", "mail jdoe@gmail.com\n"), ("personal mail address", "<J.Doe+x@iCloud.com>\n"),
         ("claude.ai link", "see https://claude.ai/code/session_x\n"))
SYNTH_CLEAN = (b"docs 192.0.2.7 198.51.100.7 203.0.113.7\n", b"public 11.1.2.3 172.15.0.1 172.32.0.1 192.169.1.1 1.10.0.0.1\n",
               b"version 10.1.2.3.4 v10.0.0.1\n", b"cp /path/to/file /Users/x/y /home/runner/work ~/Development\n", b"a@example.org git@github.com ssh builder\n",
               b"gitea.example/owner/repo settings.local.json CLAUDE.local.md x.local.y\n", b"https://claude.com/x https://docs.claude.com/\n",
               b"npm i @types/node image@sha256:abc\n", b"/usr/share/fonts $(PREFIX)/share/x ${p}/share/y https://x.org/share/z ~/share/a /srv/x\n",
               b"noreply@anthropic.com dkl@trx64.org jdoe@gmail.company.net <user>@<host> <mail>\n")
SYNTH_LINKS = (("a.dat", "/Users/jdoe/x"), ("docs/l", "../roms/k.bin"), ("docs/l", "/home/jdoe/x"))


def selftest():
    errs = []

    def want(label, cond):
        if not cond:
            errs.append(label)

    rom = random.Random(7).randbytes(8192)
    sha = hashlib.sha256(rom).hexdigest()
    win = windows_of(rom)
    want("windows_of gives %d windows" % NWIN, len(win) == NWIN)
    files = {len(rom): [(sha, "roms/fake.bin")]}
    wins = {}
    for _, w in win:
        wins.setdefault(roll_of(w), []).append((hashlib.sha256(w).hexdigest(), "roms/fake.bin"))

    clean = [("README.md", b"hello 1.2.3.4 v1.2 a@b.c\n"), ("software/a.cc", b"int x;\n"), (".claude/skills/a/SKILL.md", b"x\n"),
             ("CLAUDE.md", b"rules\n"), ("docs/img/p.png", b"\x89PNG\0\0" + os.urandom(100))]
    want("clean tree passes", check_items(clean, files, wins) == [])
    for path in ("roms/k.bin", "roms/sub/x.bin", "x/y/disk.d81", "tests/a.crt", ".claude/context.md", ".claude/handover-1.md",
                 ".claude/settings.local.json", ".claude/worktrees/a/README.md", "CLAUDE.local.md"):
        want("path " + path, check_items([(path, b"x")], files, wins) != [])
    for name, txt in SYNTH:
        want("text " + txt.strip(), name in text_hits(txt.encode(), "docs/a.md"))
        out, done = scrub(txt.encode(), "docs/a.md")
        want("scrub " + txt.strip(), name in done and text_hits(out, "docs/a.md") == [])
    for txt in SYNTH_CLEAN:
        want("clean text " + txt.decode().strip(), text_hits(txt, "docs/a.md") == [])
        want("scrub leaves clean text " + txt.decode().strip(), scrub(txt, "docs/a.md") == (txt, []))
    want("scrub: the address keeps its last octet", scrub(b"at 192.168.7.189:80\n")[0] == b"at 192.0.2.189:80\n")
    want("scrub: host, home, share", scrub(b"http://gitea.corp.lan/a /Users/jdoe/x /share/CACHEDEV2_DATA/b\n")[0]
         == b"http://gitea.example/a ~/x /srv/b\n")
    want("scrub: a whole claude.ai URL", scrub(b"page (https://claude.ai/artifact/abc12, v1)\n")[0] == b"page ((a private page), v1)\n")
    want("scrub: an allowed file stays", scrub(b"default 192.168.2.64\n", "software/trx64u/base/menu.toml") == (b"default 192.168.2.64\n", []))
    want("scrub: a binary stays", scrub(b"\0" + SYNTH[0][1].encode())[1] == [])
    want("scrub: a third-party file stays and its hit is still reported",
         scrub(b"self.local = threading.local()\n", "software/ultimate-app/x.py")[1] == []
         and text_hits(b"self.local = threading.local()\n", "software/ultimate-app/x.py") != [])
    trailer = b"x\n\nCo-Authored-By: Claude <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_01AbC\n"
    want("message: the session trailer passes", not any(rule_fires(r, message_text(trailer)) for r in RULES))
    want("message: another claude.ai link is a hit", any(rule_fires(r, message_text(b"see claude.ai/artifact/x\n" + trailer)) for r in RULES))
    want("message: the session trailer stays in a scrubbed message", scrub(b"at 10.0.0.5\n" + trailer, message=True)[0] == b"at 192.0.2.5\n" + trailer)
    want("message: the trailer form is exact", any(rule_fires(r, message_text(b"Claude-Session: https://claude.ai/code/session_1 x\n")) for r in RULES))
    want("host in a path", path_hits("docs/devhost.lan") != [])
    want("CLAUDE.local.md at the root", path_hits("CLAUDE.local.md") != [] and path_hits("docs/CLAUDE.local.md") == [])
    want("binary skipped by the text rules", check_items([("a.dat", b"\0\0" + SYNTH[0][1].encode())], files, wins) == [])
    for path, target in SYNTH_LINKS:
        want("symlink %s -> %s" % (path, target), check_items([(path, target.encode(), target)], files, wins) != [])
    want("clean symlink", check_items([("docs/l", b"../docs/b.md", "../docs/b.md")], files, wins) == [])
    want("symlink target is a private file path", any("symlink target" in b for b in check_items([("docs/l", b"../roms/x.bin", "../roms/x.bin")], files, wins)))
    want("renamed copy (size and SHA-256)", check_items([("docs/img/blob.dat", rom)], files) != [])
    want("window in a larger blob", check_items([("a.dat", os.urandom(300) + rom[1000:5000] + os.urandom(99))], files, wins) != [])
    want("unrelated binary passes", check_items([("a.dat", os.urandom(5000))], files, wins) == [])
    gk = (".gitea/workflows/selftest.yml", "NAS share path")       # a planted pair: the list itself may be empty (part E deletes it)
    share = next(t for n, t in SYNTH if n == gk[1]).encode()
    GITEA_KEPT[gk] = "selftest"
    try:
        kept = []
        want("a GITEA_KEPT hit passes and is reported as kept", check_items([(gk[0], share)], files, wins, kept) == [] and kept != [])
        want("a GITEA_KEPT file is red for any other rule", check_items([(gk[0], SYNTH[0][1].encode())], files, wins) != [])
        want("scrub rewrites a GITEA_KEPT hit", scrub(share, gk[0])[1] == [gk[1]])
    finally:
        del GITEA_KEPT[gk]
    want("the same hit outside the list is red", check_items([(gk[0], share)], files, wins) != [])
    big = os.urandom(1000) + rom[2048:6144] + os.urandom(1000)
    cands = roll_candidates(big, wins)
    want("rolling hash finds a window at any offset", any(roll_of(big[i:i + WIN]) in wins for i in cands) and len(cands) >= 1)
    try:
        import numpy  # noqa: F401
        saved_np, sys.modules["numpy"] = sys.modules["numpy"], None
        try:
            want("rolling hash without numpy agrees", roll_candidates(big, wins) == cands)
        finally:
            sys.modules["numpy"] = saved_np
    except ImportError:
        pass
    with tempfile.TemporaryDirectory() as td:
        pk = os.path.join(td, "pkg"); os.makedirs(pk)
        open(os.path.join(pk, "a.cor"), "wb").write(big)
        open(os.path.join(pk, "ok.bin"), "wb").write(os.urandom(500))
        want("directory member", check_files([pk], files, wins) != [])
        want("clean file", check_files([os.path.join(pk, "ok.bin")], files, wins) == [])
        z = os.path.join(td, "p.zip")
        with zipfile.ZipFile(z, "w") as zf:
            zf.writestr("sub/inner.cor", big)
        want("zip member", check_files([z], files, wins) != [])
        tg = os.path.join(td, "p.tar.gz")
        with tarfile.open(tg, "w:gz") as tf:
            ti = tarfile.TarInfo("x/inner.cor"); ti.size = len(big); tf.addfile(ti, io.BytesIO(big))
        want("tar.gz member", check_files([tg], files, wins) != [])
        mf = os.path.join(td, "manifest.json")
        open(mf, "w").write('{"url": "http://devhost.lan/x"}\n')
        want("a text rule in a package file", check_files([mf], files, wins) != [])
        fp = os.path.join(td, "trx64u.cor"); open(fp, "wb").write(rom)
        want("a whole private file in a .cor", check_files([fp], files, wins) != [])
    with tempfile.TemporaryDirectory() as td:        # gen-private from a folder of pinned files, against a scratch tree
        gt = lambda d, *a: subprocess.run(("git", "-C", d, "-c", "user.name=t", "-c", "user.email=t@example.org") + a, capture_output=True, check=True)
        tree, rd = os.path.join(td, "tree"), os.path.join(td, "roms")
        os.makedirs(tree); os.makedirs(os.path.join(rd, "roms"))
        open(os.path.join(rd, "roms", "k.bin"), "wb").write(rom)
        open(os.path.join(tree, "a.txt"), "w").write("public\n")
        gt(tree, "init", "-q"); gt(tree, "add", "-A"); gt(tree, "commit", "-q", "-m", "s")
        pins = os.path.join(td, "roms.txt")
        open(pins, "w").write("# pins\n%s %d roms/k.bin bench https://example.org/k.bin\n" % (sha, len(rom)))
        lst = os.path.join(td, "private.txt")
        gen_private(rd, lst, pins, tree)
        sf, sw = load_private(lst)
        want("gen-private: the file line", sf == {len(rom): [(sha, "roms/k.bin")]})
        want("gen-private: windows", sum(len(v) for v in sw.values()) == NWIN)
        want("gen-private: no file bytes in the list", rom[:16].hex() not in open(lst).read())
        open(os.path.join(rd, "roms", "k.bin"), "wb").write(rom[:-1] + b"\0")
        try:
            gen_private(rd, lst, pins, tree)
            want("gen-private refuses a file that is not the pinned one", False)
        except SystemExit:
            pass
    with tempfile.TemporaryDirectory() as td:        # message and commits mode against a scratch repository
        g = lambda *a, mail="t@example.org": subprocess.run(("git", "-C", td, "-c", "user.name=t", "-c", "user.email=" + mail) + a,
                                                            capture_output=True, check=True)
        g("init", "-q")
        for msg in ("fine", "see " + SYNTH[0][1], "x\n\nCo-Authored-By: Claude <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_01x\n"):
            open(os.path.join(td, "f"), "w").write(msg); g("add", "f"); g("commit", "-q", "-m", msg)
        bad = check_messages(["HEAD"], False, td)[0]
        want("message: address", any("private address" in b for b in bad))
        want("message: the trailer commit passes", len(bad) == 1)
        open(os.path.join(td, "f"), "w").write("y"); g("add", "f"); g("commit", "-q", "-m", "plain", mail="jdoe@gmail.com")
        want("message: a personal address as author is a hit with --authors", any("as author" in b for b in check_messages(["HEAD"], True, td)[0]))
        want("message: authors are not read without --authors", len(check_messages(["HEAD"], False, td)[0]) == 1)
        first = g("rev-list", "--max-parents=0", "HEAD").stdout.decode().strip()
        open(os.path.join(td, "leak.dat"), "wb").write(rom); g("add", "leak.dat"); g("commit", "-q", "-m", "adds a private copy")
        g("rm", "-q", "leak.dat"); g("commit", "-q", "-m", "removes it again")
        bad, nc, nb = check_commits([first + "..HEAD"], files, wins, td)
        want("commits mode: a file added and removed later is found", any("leak.dat" in b and "copy of the private file" in b for b in bad))
        want("commits mode: counts", nc == 5 and nb >= 1)
    if errs:
        print("selftest FAILED: " + "; ".join(errs))
        return 1
    print("selftest ok: every rule fires on its planted hit and scrub() clears it, the clean input passes unchanged")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--rev"); ap.add_argument("--messages", metavar="RANGE"); ap.add_argument("--authors", action="store_true")
    ap.add_argument("--files", nargs="+"); ap.add_argument("--commits", metavar="RANGE")
    ap.add_argument("--selftest", action="store_true"); ap.add_argument("--gen-private", metavar="DIR")
    ap.add_argument("--private", default=PRIVATE)
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.gen_private:
        print("wrote %s: %d private binary files" % (a.private, gen_private(a.gen_private, a.private)))
        return 0
    files, wins = load_private(a.private)
    if not files:
        print("public tree: %s is missing or empty, the known-file check cannot run" % a.private)
        return 1
    kept = []
    if a.messages:
        bad, n = check_messages(shlex.split(a.messages), a.authors)
        what = "%d commit messages" % n
    elif a.commits:
        bad, n, nb = check_commits(shlex.split(a.commits), files, wins)
        what = "%d commits, %d added or changed files" % (n, nb)
    elif a.files:
        bad, what = check_files(a.files, files, wins), "package files"
    else:
        items = list(tree_items(a.rev))
        bad, what = check_items(items, files, wins, kept), "%d files" % len(items)
    for k in kept:
        print("kept for Gitea (U20 A8, the replay writes the placeholder): " + k)
    for b in bad:
        print("PUBLIC-TREE: " + b)
    print("public tree check (%s): %s" % (what, "%d hits" % len(bad) if bad else "clean"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
