#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""T29 turbo sim driver: python tests against tb_turbo (build.sh). run.py [test ...]"""
import os, subprocess, sys, re
HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")
TBBIN = os.path.join(BUILD, os.environ.get("TB", "tb_turbo"))
STATE = os.path.join(BUILD, "boot_%s_v%s.state" % (os.environ.get("TB", "tb_turbo"), os.environ.get("VMODE", "0")))
ASM = ["64tass", "-q", "-C", "-a"]

class TB:
    def __init__(s):
        s.p = subprocess.Popen([TBBIN], cwd=os.path.join(BUILD, "run"), stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, text=True, bufsize=1)
        s.n = 0
    def cmd(s, *lines):
        out = []
        for l in lines:
            s.p.stdin.write(l + "\n")
        s.n += 1
        s.p.stdin.write("echo SYNC%d\n" % s.n); s.p.stdin.flush()
        while True:
            l = s.p.stdout.readline()
            if not l: raise RuntimeError("tb died")
            if l.startswith("> SYNC%d" % s.n) or l.startswith(">SYNC%d" % s.n): break
            if l.startswith(">"): out.append(l[1:].strip())
        return out
    def peek(s, a, n=1):
        r = s.cmd("peek %x %d" % (a, n))[0]
        return [int(x, 16) for x in r.split(":")[1].split()]
    def peek16(s, a):
        v = s.peek(a, 2); return v[0] | v[1] << 8
    def stat(s):
        r = s.cmd("stat")[0]
        m = re.search(r"stat ([0-9A-F]+) on=(\d) cycles=(\d+)", r)
        return int(m.group(1), 16), int(m.group(2)), int(m.group(3))
    def close(s):
        s.p.stdin.write("quit\n"); s.p.stdin.flush(); s.p.wait()

def asm(name):
    src = os.path.join(HERE, "t", name + ".asm"); out = os.path.join(BUILD, name + ".prg")
    subprocess.run(ASM + ["-o", out, src], check=True)
    return out

def asm_text(name, text):
    """assemble generated source text (a bench that varies a parameter)"""
    src = os.path.join(BUILD, name + ".asm"); out = os.path.join(BUILD, name + ".prg")
    open(src, "w").write(text)
    subprocess.run(ASM + ["-o", out, src], check=True)
    return out

def fresh():
    t = TB(); t.cmd("restore " + STATE); return t

def ensure_state():
    if not os.path.exists(STATE) or os.path.getmtime(STATE) < os.path.getmtime(TBBIN):
        t = TB(); print(t.cmd("boot " + STATE)); t.close()

fails = 0
def check(c, msg):
    global fails
    print(("ok   " if c else "FAIL ") + msg)
    if not c: fails += 1

# ---- tests --------------------------------------------------------------------------
def run_loop(t, secs_cycles):
    """loops per PHI2 cycle of the INC/JMP loop: each outer 256 iterations = 5*255+... cycles at 1 MHz"""
    t.cmd("load " + asm("loops"), "sys c000", "run 30000")
    a = t.peek16(0xC100); c0 = t.stat()[2]
    t.cmd("run %d" % secs_cycles)
    b = t.peek16(0xC100); c1 = t.stat()[2]
    return ((b - a) & 0xFFFF) / (c1 - c0)

def test_base(t):
    r = run_loop(t, 100000)
    print("loops/PHI2 at 1 MHz: %.5f" % r)
    check(abs(r - 0.105) < 0.004, "1 MHz INC abs / BNE loop (9 cycles, badline stalls)")

def reads(t):
    """run regs.asm (reads the register list into $C100) and return the 16 values"""
    t.cmd("poke c0ff 0", "load " + asm("regs"), "sys c000")
    r = t.cmd("until c0ff 1 400000")[0]; assert "ok" in r, r
    t.cmd("run 3000")
    return t.peek(0xC100, 16)

def screen(t):
    rows = []
    for r in range(25):
        b = t.peek(0x0400 + r * 40, 40)
        rows.append("".join(chr(c + 64) if 0 < (c & 0x7f) < 27 else (chr(c & 0x7f) if 32 <= (c & 0x7f) < 64 else ".") for c in b).rstrip())
    return "\n".join(r for r in rows if r)

def write(t, addr, val):
    t.cmd("poke c0fe 0", "poke c0f1 %x" % val, "poke c0f2 %x" % (addr & 255), "poke c0f3 %x" % (addr >> 8),
          "load " + asm("wr"), "sys c000")
    r = t.cmd("until c0fe 1 400000")[0]
    t.cmd("run 3000")
    if t.peek(0xC0FE)[0] != 1:
        print("mem", [hex(x) for x in t.peek(0xC000, 20)], [hex(x) for x in t.peek(0xC0F0, 16)]); print("stat %08X" % t.stat()[0], "kbbuf", t.peek(0x277, 10), t.peek(0xC6))
        raise AssertionError("write program did not finish")

ADDRS = [0xD030,0xD031,0xD0BC,0xD070,0xD071,0xD0B0,0xD0B1,0xD0F0,0xD0F1,0xD03C,0xD07C,0xD0FC,0xD0BD,0xD07A,0xD07B,0xD0FA]

def test_regs(t):
    t.cmd("run 20000")          # the boot state is long past the (shortened) hold
    r = reads(t)
    check(r[:3] == [0xFF]*3 and r[9:12] == [0xFF]*3, "enable word 0: $D030 $D031 $D0BC read FF: %s" % r)
    # registers mode, menu: 1 MHz with badline timing (prefer $80): d031 = {~1, 0} = $00
    t.cmd("cfg 01 80", "run 2000")
    r = reads(t)
    check(r[0] == 0xFF and r[1] == 0x00 and r[4] == 0x00 and r[6] == 0x00 and r[8] == 0x00, "registers mode: $D030 FF, $D031 and mirrors 00: %s" % r)
    write(t, 0xD031, 0x85)
    r = reads(t)
    check(r[1] == 0x85 and r[4] == 0x85 and r[6] == 0x85 and r[8] == 0x85, "$D031 write 85 reads back, mirrors: %s" % r)
    st = t.stat()
    print("stat %08X on=%d" % (st[0], st[1]))
    check(st[1] == 1 and ((st[0] >> 16) & 0x7F) == 8, "engine on at N=8 after $D031=85")
    write(t, 0xD07A, 0)         # sets d031 := 0
    r = reads(t)
    check(r[1] == 0x00, "$D07A: d031 00: %s" % r)
    write(t, 0xD0FB, 0)         # menu value
    r = reads(t)
    check(r[1] == 0x00, "$D0FB: menu value 00: %s" % r)
    t.cmd("cfg 01 89", "run 2000")   # 16 MHz, badline timing on: d031 = {~1,..}= 09
    r = reads(t)
    check(r[1] == 0x09, "UPDATE prefer 89: d031 09: %s" % r)
    t.cmd("cfg 01 09", "run 2000")   # badline timing off: d031 = 89
    r = reads(t)
    check(r[1] == 0x89, "UPDATE prefer 09: d031 89: %s" % r)
    st = t.stat(); check(((st[0] >> 16) & 0x7F) == 16 and st[1] == 1, "N=16")
    # SuperCPU detect
    t.cmd("cfg 03 80", "run 2000")
    r = reads(t)
    check(r[2] == 1 and r[9] == 1 and r[10] == 1 and r[11] == 1 and r[12] == 0xFF, "$D0BC reads 01, mirrors 01, $D0BD FF: %s" % r)
    # TurboEnable bit mode
    t.cmd("cfg 05 89", "run 2000")   # menu: 16 MHz, stalls: d031 = 09, d030_b0 = 1
    r = reads(t)
    check(r[0] == 0xFF and r[1] == 0x09, "TurboEnable: UPDATE sets bit: $D030 FF, $D031 09: %s" % r)
    write(t, 0xD030, 0)
    r = reads(t)
    check(r[0] == 0xFE and r[1] == 0x00, "$D030=0: reads FE, $D031 00: %s" % r)
    st = t.stat(); check(st[1] == 0, "engine off")
    write(t, 0xD030, 1)
    r = reads(t)
    check(r[0] == 0xFF and r[1] == 0x09, "$D030=1: FF, $D031 menu 09: %s" % r)
    t.cmd("cfg 00 80", "run 2000")
    r = reads(t)
    check(r[0] == 0xFF and r[1] == 0xFF, "Off: FF FF: %s" % r)

def rate(t, prog, cycles, warm=30000, setup=()):
    t.cmd(*setup)
    t.cmd("load " + asm(prog), "sys c000", "run %d" % warm)
    a = t.peek16(0xC100); c0 = t.stat()[2]
    t.cmd("run %d" % cycles)
    b = t.peek16(0xC100); c1 = t.stat()[2]
    return ((b - a) & 0xFFFF) / (c1 - c0)

IDX_N = {0:1,1:2,2:3,3:4,4:6,5:8,6:10,7:12,8:14,9:16,10:20,11:24,12:32,13:40,14:47,15:63}

def test_dexloop(t):
    """cycles per PHI2 of the board's DEX/BNE measuring loop (turbomeas): the board read 59.8 at N=63"""
    for idx in (9, 12, 14, 15):
        for bl in (1, 0):
            tt = fresh()
            tt.cmd("run 20000", "cfg 00 %02x" % (idx | bl << 7), "run 2000")
            n = IDX_N[idx]
            r = rate(tt, "dexloop", 200000)
            tt.close()
            tt = fresh()
            tt.cmd("run 20000", "cfg 00 %02x" % (idx | bl << 7), "run 2000")
            r2 = rate(tt, "dexloop_cia", 200000)       # + one CIA 1 read per pass, as turbomeas does
            tt.close()
            print("idx %2d N %2d bl %d: CPU cycles per PHI2 %.2f, with one CIA read per pass %.2f (pass 1292 / 1296)" % (idx, n, bl, r * 1292, r2 * 1296))

def test_speed(t):
    base = None
    for idx in (0, 1, 3, 5, 9, 12, 15):
        for bl in (1, 0):
            tt = fresh()
            tt.cmd("run 20000", "cfg 00 %02x" % (idx | bl << 7), "run 2000")
            n = IDX_N[idx]
            cyc = 9.0 + (n / 64.0 if n > 32 else 0.0)        # INC abs: a late byte, +1 slot when the next edge is a slot (N > 32)
            exp = n / cyc * (0.945 if bl else 1.0)
            r = rate(tt, "loops", int(min(60000, 25000 / exp)))
            tt.close()
            if idx == 0 and bl == 0: exp = 1/9.0
            print("idx %2d N %2d bl %d: loops/PHI2 %.4f expected %.4f ratio %.3f" % (idx, n, bl, r, exp, r / exp))
            check(abs(r / exp - 1) < 0.06, "idx %d bl %d speed" % (idx, bl))

def test_io(t):
    for what, name in ((0, "CIA read"), (1, "SID write"), (2, "VIC write")):
        for idx in (0, 9, 15):
            tt = fresh()
            tt.cmd("run 20000", "cfg 00 %02x" % (idx | 0x80), "run 2000", "poke c0f0 %x" % what)
            r = rate(tt, "io", 8000, warm=30000)
            tt.close()
            print("%s idx %d: iterations per PHI2 %.4f" % (name, idx, r))

def test_phase(t):
    for idx, w in ((4, 20000), (6, 20000), (2, 20000), (15, 20000), (9, 20000)):
        tt = fresh()
        tt.cmd("run %d" % w, "cfg 00 %02x" % (idx | 0x80), "run 2000", "poke c100 0", "load " + asm("phase"), "sys c000")
        r = tt.cmd("until c100 1 600000")[0]
        bad = tt.peek(0xC200, 64)
        print("last mismatch: read/expected/d", tt.peek(0xC120, 3))
        tt.close()
        print("idx %d: %s; mismatches per delay: %s" % (idx, r, bad))
        check("ok" in r and sum(bad) == 0, "CIA write/read-back at all phases, idx %d" % idx)

def test_icr(t):
    for idx, w in ((15, 20000), (9, 20000), (4, 20000)):
        tt = fresh()
        tt.cmd("run %d" % w, "cfg 00 %02x" % (idx | 0x80), "run 2000", "poke c100 0", "load " + asm("icr"), "sys c000")
        r = tt.cmd("until c100 1 900000")[0]
        bad = tt.peek(0xC200, 64)
        tt.close()
        print("idx %d: %s; ICR read mismatches per delay: %s" % (idx, r, bad))
        check("ok" in r and sum(bad) == 0, "CIA 1 ICR read returns the flag and clears it at all phases, idx %d" % idx)

def test_phaseio(t):
    """phase.asm with another register: IO_ADDR (default the REU's C64 base address low, $DF02)"""
    addr = os.environ.get("IO_ADDR", "$DF02")
    src = open(os.path.join(HERE, "t", "phase.asm")).read().replace("$DD03", addr)
    for idx in (15, 9, 4):
        tt = fresh()
        tt.cmd("run 20000", "cfg 00 %02x" % (idx | 0x80), "run 2000", "poke c100 0", "load " + asm_text("phaseio", src), "sys c000")
        r = tt.cmd("until c100 1 900000")[0]
        bad = tt.peek(0xC200, 64)
        tt.close()
        print("%s idx %d: %s; mismatches per delay: %s" % (addr, idx, r, bad))
        check("ok" in r and sum(bad) == 0, "%s write/read-back at all phases, idx %d" % (addr, idx))

def test_ciaseq(t):
    for idx in (15, 12, 9, 4):
        tt = fresh()
        tt.cmd("run 20000", "cfg 00 %02x" % (idx | 0x80), "run 2000", "load " + asm("ciaseq"), "sys c000")
        r = tt.cmd("until c100 1 900000")[0]
        e = tt.peek(0xC101)[0]; n = tt.peek16(0xC102)
        tt.close()
        print("idx %d: %s, %d passes, %d errors" % (idx, r, n, e))
        check("ok" in r and e == 0 and n >= 300, "CIA write / RMW / read ordering, idx %d" % idx)

def test_vicrd(t):
    """VIC register reads in turbo return what a 1 MHz read returns (the side effect free registers)"""
    tabs = {}
    for idx, prefer in ((0, 0x80), (15, 0x8F), (9, 0x89), (12, 0x0C)):
        tt = fresh()
        tt.cmd("run 20000", "cfg 00 %02x" % prefer, "run 2000", "poke c100 0", "load " + asm("vicrd"), "sys c000")
        r = tt.cmd("until c100 1 900000")[0]
        tabs[idx] = tt.peek(0xC300, 64) + tt.peek(0xC340, 64)
        tt.close()
        check("ok" in r, "idx %d: vicrd finished (%s)" % (idx, r))
    for idx in (15, 9, 12):
        bad = [i for i in range(128) if tabs[idx][i] != tabs[0][i]]
        print("idx %d: mismatches against 1 MHz at table entries %s" % (idx, [(i, hex(tabs[0][i]), hex(tabs[idx][i])) for i in bad]))
        check(not bad, "idx %d: VIC register reads equal the 1 MHz reads" % idx)

def test_colr(t):
    """colour RAM from the CPU in turbo: fill, read back, scroll, read-modify-write, against a model in the program;
    the upper nibble of a read must be what 1 MHz gives"""
    up = {}
    for idx, prefer in ((0, 0x80), (15, 0x8F), (9, 0x89), (12, 0x8C), (4, 0x84)):
        tt = fresh()
        tt.cmd("run 20000", "cfg 00 %02x" % prefer, "run 2000", "poke c100 0", "load " + asm("colr"), "sys c000")
        r = tt.cmd("until c100 1 900000")[0]
        e = tt.peek(0xC101)[0]; up[idx] = tt.peek(0xC102)[0]
        tt.close()
        print("idx %d: %s, %d errors, upper nibble of a read %02X" % (idx, r, e, up[idx]))
        check("ok" in r and e == 0, "idx %d: colour RAM fill, read back, scroll and read-modify-write" % idx)
    check(len(set(up.values())) == 1, "the upper nibble of a colour RAM read is the same at every speed: %s" % up)

def test_colvic(t):
    """the VIC's colour RAM reads are not disturbed by the CPU's colour RAM accesses in turbo: the picture of a colour
    RAM rewritten for ever equals the picture at 1 MHz"""
    pics = {}
    for idx, prefer in ((0, 0x80), (15, 0x8F), (15, 0x0F), (9, 0x89)):
        tt = fresh()
        tt.cmd("run 20000", "cfg 00 %02x" % prefer, "run 2000", "load " + asm("colvic"), "sys c000", "run 40000")
        f = "/tmp/colvic_%d_%02x.txt" % (idx, prefer)
        tt.cmd("grab " + f)
        tt.close()
        d = {}
        for l in open(f):
            x, y, c = map(int, l.split()); d[(x, y)] = c
        pics[(idx, prefer)] = d
        print("idx %d prefer %02x: %d pixels, colours %s" % (idx, prefer, len(d), sorted(set(d.values()))))
    ref = pics[(0, 0x80)]
    for k, d in pics.items():
        if k == (0, 0x80): continue
        bad = [p for p in ref if d.get(p) != ref[p]]
        print("%s: %d differing pixels of %d" % (k, len(bad), len(ref)))
        check(not bad, "idx %d prefer %02x: the picture equals the 1 MHz picture" % k)

def test_sidw(t):
    """SID writes through the op FIFO: order, a burst and read after write give the engine the same state at every speed"""
    for idx, prefer in ((0, 0x80), (15, 0x8F), (9, 0x89), (12, 0x8C), (4, 0x84)):
        tt = fresh()
        tt.cmd("run 20000", "cfg 00 %02x" % prefer, "run 2000", "poke c100 0", "load " + asm("sidw"), "sys c000")
        r = tt.cmd("until c100 1 900000")[0]
        e = tt.peek(0xC101)[0]; v = tt.peek(0xC110, 3)
        tt.close()
        print("idx %d: %s, %d errors %s" % (idx, r, e, v))
        check("ok" in r and e == 0, "idx %d: SID write order, burst and read after write" % idx)

def test_ciaport(t):
    for idx in (15, 12, 9, 4):
        tt = fresh()
        tt.cmd("run 20000", "cfg 00 %02x" % (idx | 0x80), "run 2000", "load " + asm("ciaport"), "sys c000")
        r = tt.cmd("until c100 1 900000")[0]
        e = tt.peek(0xC101)[0]; n = tt.peek16(0xC102)
        tt.close()
        print("idx %d: %s, %d passes, %d errors" % (idx, r, n, e))
        check("ok" in r and e == 0 and n >= 400, "CIA port written and read back at once, idx %d" % idx)

def test_irq(t):
    for idx in (0, 5, 9, 15):
        for bl in (1, 0):
            tt = fresh()
            tt.cmd("run 20000", "cfg 00 %02x" % (idx | bl << 7), "run 2000", "load " + asm("irq"), "sys c000", "run 40000")
            c0 = tt.stat()[2]; v0 = tt.peek(0xC100)[0]; a0 = tt.peek(0xC102)[0]; m0 = tt.peek16(0xC104)
            tt.cmd("run 196560")
            c1 = tt.stat()[2]; v1 = tt.peek(0xC100)[0]; a1 = tt.peek(0xC102)[0]; m1 = tt.peek16(0xC104)
            tt.close()
            dv = (v1 - v0) & 255; da = (a1 - a0) & 255
            fr = (c1 - c0) / (65 * 263 if os.environ.get("VMODE", "0") in ("1", "2") else 63 * 312.0)
            print("idx %d bl %d: %d cycles: raster IRQs %d (%.1f frames), CIA IRQs %d (%.1f expected), main loop %d" % (idx, bl, c1 - c0, dv, fr, da, (c1 - c0) / 2001.0, (m1 - m0) & 0xFFFF))
            check(abs(dv - fr) <= 1, "idx %d bl %d raster IRQs: one per frame" % (idx, bl))
            check(abs(da - (c1 - c0) / 2001.0) <= 1.5, "idx %d bl %d CIA timer IRQs in PHI2 time" % (idx, bl))

def test_stress(t):
    t.cmd("run 20000", "cfg 01 80", "run 2000", "load " + asm("stress"), "sys c000", "run 30000")
    st = []
    for i in range(8):
        t.cmd("run 20000")
        e = t.peek(0xC100)[0]; n = t.peek16(0xC102); s = t.stat()[0]
        st.append((n, e, (s >> 16) & 0x7F, s >> 30 & 1))
    print("iterations/errors/N/on:", st, "last kind", t.peek(0xC104))
    check(st[-1][1] == 0 and st[-1][0] > 100, "no errors in %d iterations with random speed changes" % st[-1][0])

def test_halt(t):
    t.cmd("run 20000", "cfg 01 80", "run 2000", "load " + asm("stress"), "sys c000", "run 30000")
    import random
    random.seed(5)
    seen = set()
    for i in range(40):
        t.cmd("run %d" % random.randint(1, 900))
        t.cmd("halt", "run 200")
        s, on, c = t.stat()
        n0 = t.peek16(0xC102)
        t.cmd("run 400")
        n1 = t.peek16(0xC102)
        s2 = t.stat()
        check(n0 == n1, "halt %d: the program stands still while halted (%d -> %d), turbo on=%d" % (i, n0, n1, on))
        seen.add((s >> 16) & 0x7F)
        t.cmd("go", "run 300")
    e = t.peek(0xC100)[0]; n = t.peek16(0xC102)
    print("N seen at halts", sorted(seen), "iterations", n, "errors", e)
    check(e == 0 and n > 50, "no errors after 40 halts and resumes")
    t.cmd("run 100000")
    check(t.peek(0xC100)[0] == 0, "still none")

def runs(path, y):
    rows = {}
    for l in open(path):
        x, yy, c = map(int, l.split())
        rows.setdefault(yy, {})[x] = c
    out = []
    for yy, d in sorted(rows.items()):
        xs = sorted(d)
        seq = []
        for x in xs:
            if not seq or seq[-1][0] != d[x]: seq.append([d[x], 1, x])
            else: seq[-1][1] += 1
        out.append((yy, seq))
    return out

def test_upic(t):
    for idx, bl in ((15, 0), (15, 1), (9, 0), (12, 0)):
        tt = fresh()
        tt.cmd("run 20000", "cfg 00 %02x" % (idx | bl << 7), "run 2000", "load " + asm("upic"), "sys c000", "run 60000")
        tt.cmd("grab /tmp/upic_v%s_%d_%d.txt" % (os.environ.get("VMODE", "0"), idx, bl))
        tt.close()
        best = None
        for yy, seq in runs("/tmp/upic_v%s_%d_%d.txt" % (os.environ.get("VMODE", "0"), idx, bl), 0):
            cols = [s[0] for s in seq]
            if all(c in cols for c in range(1, 8)):
                best = (yy, seq); break
        print("idx %d bl %d:" % (idx, bl), best)
        if best:
            seq = best[1]
            one = [s for s in seq if 1 <= s[0] <= 8]
            lim = 2 if idx == 15 else 4
            check(all(s[1] <= lim for s in one if s[0] in range(1, 8)) and sum(s[1] for s in one if s[0] in range(1, 8)) <= (8 if idx == 15 else 28),
                  "idx %d bl %d: colours 1-7 one pixel each (7.875 CPU cycles per pixel), the stores land in different pixels" % (idx, bl))
        else:
            check(False, "idx %d bl %d: seven colours on one line" % (idx, bl))

def hexw(l): return int(l.split("=")[1], 16)

def test_state(t):
    t.cmd("run 20000", "cfg 01 89", "run 2000")
    write(t, 0xD031, 0x83)               # 4 MHz, badline timing on
    t.cmd("run 5000", "halt", "run 50000")
    s = t.stat()[0]
    w1 = hexw(t.cmd("sbr 001")[0])
    print("stat %08X word %08X" % (s, w1))
    # {hold[22:8] 31:17, d031 16:9, d030_b0 8, regs_en 7:5, prefer7 4, idx 3:0}
    check((w1 >> 9) & 0xFF == 0x83 and (w1 >> 5) & 7 == 1 and (w1 >> 8) & 1 == 1 and (w1 >> 4) & 1 == 1 and w1 & 15 == 9 and w1 >> 17 == 0,
          "state word: d031 83, d030_b0 1 (set by the UPDATE), regs_en 1, prefer 89, hold 0")
    # restore: other values through the staging, loaded at the next opening edge
    pack = (0x8C << 9) | (1 << 8) | (5 << 5) | (0 << 4) | 9
    t.cmd("sbw 001 %x" % pack, "sbld 1", "go", "run 40000")
    r = reads(t)
    check(r[1] == 0x8C and r[0] == 0xFF, "restored: $D031 reads 8C, $D030 FF (TurboEnable bit set): %s" % r)
    st = t.stat()[0]
    check(((st >> 16) & 0x7F) == 32, "restored: 32 CPU cycles per PHI2 (stat %08X)" % st)

def test_hold(t):
    t.cmd("run 20000", "cfg 00 8f", "run 2000")        # manual, 63 cycles, badline timing off ($8F)
    s0 = t.stat()[0]
    check((s0 >> 30) & 1 == 1 and ((s0 >> 16) & 0x7F) == 63, "before the reset: engine on at 63 (stat %08X)" % s0)
    t.cmd("reset")
    s1 = t.stat()[0]
    check((s1 >> 31) & 1 == 1 and (s1 >> 30) & 1 == 0, "after the reset: hold active, engine off (stat %08X)" % s1)
    t.cmd("run 4000")
    s2 = t.stat()[0]
    check((s2 >> 31) & 1 == 1 and (s2 >> 30) & 1 == 0, "4000 PHI2 cycles later: still at 1 MHz (stat %08X)" % s2)
    t.cmd("run 1500")
    s3 = t.stat()[0]
    check((s3 >> 31) & 1 == 0 and (s3 >> 30) & 1 == 1, "after the 5000-cycle test hold: engine on (stat %08X)" % s3)
    # registers mode: $D031 reads 00 after the reset and the CPU stays at 1 MHz until it is written
    t.cmd("cfg 01 8f", "run 2000", "reset", "run 7000")
    s4 = t.stat()[0]
    check((s4 >> 31) & 1 == 0 and (s4 >> 30) & 1 == 0, "registers mode after reset and hold: still 1 MHz until $D031 is written (stat %08X)" % s4)

def test_cpupause(t):
    t.cmd("run 20000", "cfg 00 0f", "run 2000", "load " + asm("stress"), "sys c000", "run 30000")
    s = t.stat()[0]
    check((s >> 30) & 1 == 1, "running in turbo (stat %08X)" % s)
    for i in range(5):
        t.cmd("pause 1", "run 300")
        n0 = t.peek16(0xC102); t.cmd("run 500"); n1 = t.peek16(0xC102)
        s = t.stat()[0]
        check(n0 == n1 and (s >> 30) & 1 == 0, "CPU-only pause %d: program stands still (%d), engine off (stat %08X)" % (i, n0, s))
        t.cmd("pause 0", "run 3000")
        n2 = t.peek16(0xC102)
        check(n2 != n1, "pause %d released: program goes on (%d -> %d)" % (i, n1, n2))
    t.cmd("run 20000")
    s = t.stat()[0]
    check((s >> 30) & 1 == 1 and t.peek(0xC100)[0] == 0, "turbo again, no errors (stat %08X)" % s)

SLOW_ACC = {          # name: (access instruction, cycles)
    "RAM read":   ("lda $C0F0", 4),
    "RAM write":  ("sta $C0F0", 4),
    "CIA1 read":  ("lda $DC04", 4),
    "CIA1 write": ("sta $DC05", 4),
    "SID read":   ("lda $D41B", 4),
    "SID write":  ("sta $D418", 4),
    "IO1 read":   ("lda $DE00", 4),
    "IO1 write":  ("sta $DE00", 4),
    "VIC read":   ("lda $D019", 4),
    "VIC write":  ("sta $D015", 4),
    "col read":   ("lda $D800", 4),
    "col write":  ("sta $D800", 4),
}

def slowloop(acc, nops, lenx=0):
    """one access, nops NOPs, a 16-bit counter and a JMP: PHI2 cycles per access as measured by the counter
    (loop length 14 + 2 * nops CPU cycles)"""
    text = "*=$C000\n sei\n lda #0\n sta $F0\n sta $F1\nloop %s\n" % acc
    text += " nop\n" * nops
    text += " inc $F0\n bne l1\n inc $F1\nl1 jmp loop\n"
    return text

def slow_cost(idx, name, nops, cycles=12000, warm=20000):
    acc, _ = SLOW_ACC[name]
    tt = fresh()
    tt.cmd("run 20000", "cfg 00 %02x" % (idx | (0x80 if os.environ.get("BL", "1") == "1" else 0)), "run 2000")
    tt.cmd("load " + asm_text("slowloop", slowloop(acc, nops)), "sys c000", "run %d" % warm)
    a = tt.peek16(0xF0); c0 = tt.stat()[2]
    tt.cmd("run %d" % cycles)
    b = tt.peek16(0xF0); c1 = tt.stat()[2]
    tt.close()
    n = (b - a) & 0xFFFF
    return (c1 - c0) / n if n else float("inf")

def test_slotwin(t):
    """PHI2 cycles per access of a loop with one slow access and nops NOPs of work (loop length 14 + 2 nops CPU
    cycles). Gideon's core (docs/turbo-measurements-gideon.md round 3): at 16 MHz 1.000-1.03 up to 16 cycles."""
    sel = os.environ.get("ACC", "CIA1 read,CIA1 write,IO1 read").split(",")
    idxs = [int(x) for x in os.environ.get("IDX", "9,15").split(",")]
    nl = [int(x) for x in os.environ.get("NOPS", "0,1,2,4,8,12,16,20,24").split(",")]
    for idx in idxs:
        for name in sel:
            row = []
            for k in nl:
                row.append("%d:%.3f" % (14 + 2 * k, slow_cost(idx, name, k)))
            print("idx %2d %-10s loop length:PHI2 per access  %s" % (idx, name, " ".join(row)))

def openbus(t, target, pre, turbo, what=None):
    """256 reads of target (after a write of $A5 to it if pre); returns {value: count}. The REU is off (it
    answers $DF00-$DFFF, mirrored, when on)"""
    t.cmd("reu 0")
    if turbo: t.cmd("cfg 00 8f", "run 2000")
    src = open(os.path.join(HERE, "t", "openbus.asm.tpl")).read()
    src = src.replace("TARGET", "$%04X" % target)
    src = "PRE = %d\n" % pre + src
    t.cmd("poke c0ff 0", "load " + asm_text("openbus", src), "sys c000", "run 20000")
    for _ in range(40):
        if t.peek(0xC0FF)[0]: break
        t.cmd("run 5000")
    vals = t.peek(0xC200, 256)
    h = {}
    for v in vals: h[v] = h.get(v, 0) + 1
    return h

def test_openbus(t):
    """T74: an unanswered read returns the VIC's phi1 byte (idle fetch $3FFF = $5C, refresh $3Fxx = $11, sprite
    pointer $07F8+n = $22), never the CPU's own write ($A5), at 1 MHz and in turbo. Colour RAM's upper nibble
    follows the VIC byte at 1 MHz; in turbo it reads 0 (the fast path of c64_turbo, not changed by T74: printed)."""
    vic = {0x5C, 0x11, 0x22}
    for turbo in (0, 1):
        for target, pre in ((0xDE00, 0), (0xDF80, 0), (0xDE00, 1), (0xDF80, 1), (0xDFF0, 1)):
            tt = fresh()
            h = openbus(tt, target, pre, turbo)
            tt.close()
            print("turbo %d $%04X pre %d: %s" % (turbo, target, pre, " ".join("%02X:%d" % kv for kv in sorted(h.items()))))
            check(set(h) <= vic and 0x5C in h, "turbo %d $%04X pre %d returns the VIC byte" % (turbo, target, pre))
    for turbo in (0, 1):
        tt = fresh()
        h = openbus(tt, 0xD800, 1, turbo)
        tt.close()
        print("turbo %d $D800 pre 1: %s" % (turbo, " ".join("%02X:%d" % kv for kv in sorted(h.items()))))
        check(all((v & 0xF) == 0x5 for v in h), "turbo %d colour RAM low nibble = the write" % turbo)
        if not turbo:
            check(all(v >> 4 in {x >> 4 for x in vic} for v in h), "colour RAM upper nibble = VIC byte")

TESTS = {k[5:]: v for k, v in globals().items() if k.startswith("test_")}
if __name__ == "__main__":
    ensure_state()
    names = sys.argv[1:] or list(TESTS)
    for n in names:
        print("==", n)
        t = fresh()
        try: TESTS[n](t)
        finally: t.close()
    print("FAILED %d" % fails if fails else "PASS")
    sys.exit(1 if fails else 0)
