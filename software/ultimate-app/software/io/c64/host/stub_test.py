"""The two 6502 stubs that read and write drive A's registers (T66 part 8,
trx_drive.cc), run in a small 6502 interpreter with a fake 6522 pair.

They cannot be tried anywhere else without a drive, and getting them wrong
means a drive that hangs, so every byte of them is checked here: that the save
stub keeps the registers and the VIA images and puts the three patched bytes
back, that it writes no VIA register of its own, that the load stub clears both
IERs first, writes ACR and PCR before the timers start, never writes an IFR,
and ends in an RTI to the saved PC with the saved registers.

Run by the Makefile's "make test"."""
import re, sys

import os
SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "trx_drive.cc")
text = open(SRC).read()

defs = {}
for m in re.finditer(r"#define\s+([A-Z_0-9]+)\s+(0x[0-9A-Fa-f]+|\d+)", text):
    defs[m.group(1)] = int(m.group(2), 0)


def value(tok):
    tok = tok.strip()
    if not tok:
        return None
    m = re.fullmatch(r"([A-Z_0-9]+)\s*\+\s*(\d+)", tok)
    if m:
        return (defs[m.group(1)] + int(m.group(2))) & 0xFF
    if tok in defs:
        return defs[tok] & 0xFF
    return int(tok, 0) & 0xFF


def extract(name):
    i = text.index("static const uint8_t %s[STUB_SIZE] = {" % name)
    j = text.index("};", i)
    body = text[i:j]
    body = re.sub(r"//[^\n]*", "", body)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = body[body.index("{") + 1:]
    out = [value(t) for t in body.split(",")]
    out = [b for b in out if b is not None]
    return out


save = extract("save_stub")
load = extract("load_stub")
size = defs["STUB_SIZE"]
assert len(save) == size, ("save stub is %d bytes, not %d" % (len(save), size))
assert len(load) == size, ("load stub is %d bytes, not %d" % (len(load), size))


class M:
    def __init__(self):
        self.ram = bytearray(0x10000)
        self.via = {}          # address -> value the fake 6522 returns
        self.written = []      # (addr, value) of every write above $1800

    def rd(self, a):
        if 0x1800 <= a < 0x2000:
            return self.via.get(a, 0)
        return self.ram[a]

    def wr(self, a, v):
        if 0x1800 <= a < 0x2000:
            self.written.append((a, v))
            self.via[a] = v
            return
        self.ram[a] = v


def run(m, pc, a=0, x=0, y=0, s=0xFF, p=0x20, limit=200000):
    """A 6502 subset: everything the two stubs use."""
    def setnz(v):
        nonlocal p
        p = (p & ~0x82) | (0x02 if v == 0 else 0) | (v & 0x80)
    steps = 0
    while steps < limit:
        steps += 1
        op = m.rd(pc)
        if op == 0x08:      # PHP
            m.wr(0x100 + s, p | 0x30); s = (s - 1) & 0xFF; pc += 1
        elif op == 0x28:    # PLP
            s = (s + 1) & 0xFF; p = m.rd(0x100 + s) | 0x20; pc += 1
        elif op == 0x48:    # PHA
            m.wr(0x100 + s, a); s = (s - 1) & 0xFF; pc += 1
        elif op == 0x68:    # PLA
            s = (s + 1) & 0xFF; a = m.rd(0x100 + s); setnz(a); pc += 1
        elif op == 0x78:    # SEI
            p |= 0x04; pc += 1
        elif op == 0xBA:    # TSX
            x = s; setnz(x); pc += 1
        elif op == 0x9A:    # TXS
            s = x; pc += 1
        elif op == 0xE8:    # INX
            x = (x + 1) & 0xFF; setnz(x); pc += 1
        elif op == 0xA9:    # LDA #
            a = m.rd(pc + 1); setnz(a); pc += 2
        elif op == 0xA2:    # LDX #
            x = m.rd(pc + 1); setnz(x); pc += 2
        elif op == 0xA0:    # LDY #
            y = m.rd(pc + 1); setnz(y); pc += 2
        elif op == 0xAD:    # LDA abs
            ad = m.rd(pc + 1) | (m.rd(pc + 2) << 8); a = m.rd(ad); setnz(a); pc += 3
        elif op == 0xAE:    # LDX abs
            ad = m.rd(pc + 1) | (m.rd(pc + 2) << 8); x = m.rd(ad); setnz(x); pc += 3
        elif op == 0xAC:    # LDY abs
            ad = m.rd(pc + 1) | (m.rd(pc + 2) << 8); y = m.rd(ad); setnz(y); pc += 3
        elif op == 0xBD:    # LDA abs,X
            ad = (m.rd(pc + 1) | (m.rd(pc + 2) << 8)) + x; a = m.rd(ad & 0xFFFF); setnz(a); pc += 3
        elif op == 0x8D:    # STA abs
            ad = m.rd(pc + 1) | (m.rd(pc + 2) << 8); m.wr(ad, a); pc += 3
        elif op == 0x8E:    # STX abs
            ad = m.rd(pc + 1) | (m.rd(pc + 2) << 8); m.wr(ad, x); pc += 3
        elif op == 0x8C:    # STY abs
            ad = m.rd(pc + 1) | (m.rd(pc + 2) << 8); m.wr(ad, y); pc += 3
        elif op == 0x9D:    # STA abs,X
            ad = (m.rd(pc + 1) | (m.rd(pc + 2) << 8)) + x; m.wr(ad & 0xFFFF, a); pc += 3
        elif op == 0xE0:    # CPX #
            v = m.rd(pc + 1); d = (x - v) & 0xFF
            p = (p & ~0x83) | (0x02 if d == 0 else 0) | (d & 0x80) | (0x01 if x >= v else 0)
            pc += 2
        elif op == 0xF0:    # BEQ
            off = m.rd(pc + 1); off = off - 256 if off > 127 else off
            pc = pc + 2 + (off if (p & 0x02) else 0)
        elif op == 0xD0:    # BNE
            off = m.rd(pc + 1); off = off - 256 if off > 127 else off
            pc = pc + 2 + (0 if (p & 0x02) else off)
        elif op == 0x4C:    # JMP abs
            pc = m.rd(pc + 1) | (m.rd(pc + 2) << 8)
            return ("jmp", pc, a, x, y, s, p, steps)
        elif op == 0x40:    # RTI
            s = (s + 1) & 0xFF; p = m.rd(0x100 + s) | 0x20
            s = (s + 1) & 0xFF; lo = m.rd(0x100 + s)
            s = (s + 1) & 0xFF; hi = m.rd(0x100 + s)
            return ("rti", lo | (hi << 8), a, x, y, s, p, steps)
        elif op == 0xEA:    # NOP
            pc += 1
        else:
            return ("bad opcode %02X at %04X" % (op, pc), pc, a, x, y, s, p, steps)
    return ("limit", pc, a, x, y, s, p, steps)


fails = []
checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


# ---- the save stub ---------------------------------------------------------
SV = {k: defs[k] for k in defs if k.startswith("SV")}
m = M()
m.ram[0x100:0x100 + size] = bytes(save)
NEXT = 0x0400
# the three bytes the JMP covered, as the app patches them in
stub = bytearray(save)
stub[defs["SVP_B0"]] = 0xA9
stub[defs["SVP_B1"]] = 0x11
stub[defs["SVP_B2"]] = 0x22
for off, ad in ((defs["SVP_A0"], NEXT), (defs["SVP_A1"], NEXT + 1), (defs["SVP_A2"], NEXT + 2),
                (defs["SVP_JMP"], NEXT)):
    stub[off] = ad & 0xFF
    stub[off + 1] = ad >> 8
m.ram[0x100:0x100 + size] = bytes(stub)
for i in range(16):
    m.via[0x1800 + i] = 0x10 + i
    m.via[0x1C00 + i] = 0x20 + i
r = run(m, 0x100, a=0x3C, x=0x5E, y=0x7F, s=0xF0, p=0xA5 | 0x20)
check(r[0] == "limit", "the save stub should park, it did %s" % r[0])
check(m.ram[0x100 + defs["SV_MAGIC"]] == defs["SV_MARK"], "the save stub set no magic")
check(m.ram[0x100 + defs["SV_A"]] == 0x3C, "A came back as %02X" % m.ram[0x100 + defs["SV_A"]])
check(m.ram[0x100 + defs["SV_X"]] == 0x5E, "X came back as %02X" % m.ram[0x100 + defs["SV_X"]])
check(m.ram[0x100 + defs["SV_Y"]] == 0x7F, "Y came back as %02X" % m.ram[0x100 + defs["SV_Y"]])
check(m.ram[0x100 + defs["SV_S"]] == 0xF0, "S came back as %02X" % m.ram[0x100 + defs["SV_S"]])
check(m.ram[0x100 + defs["SV_P"]] & 0xCF == 0xA5 & 0xCF,
      "P came back as %02X" % m.ram[0x100 + defs["SV_P"]])
for i in range(16):
    check(m.ram[0x100 + defs["SV_V1"] + i] == 0x10 + i, "VIA1[%d] is %02X" % (i, m.ram[0x100 + defs["SV_V1"] + i]))
    check(m.ram[0x100 + defs["SV_V2"] + i] == 0x20 + i, "VIA2[%d] is %02X" % (i, m.ram[0x100 + defs["SV_V2"] + i]))
# the IFR must have been read before the registers that clear flags
first = m.written  # writes to the VIA area: none expected from the save stub
check(not first, "the save stub wrote to a VIA: %s" % first)

# let it out of the park loop
m.ram[0x100 + defs["SV_FLAG"]] = 1
r = run(m, 0x100 + 0x3B, a=0, x=0, y=0, s=0xF0, p=0x20)
check(r[0] == "jmp" and r[1] == NEXT, "the save stub left with %s to %04X" % (r[0], r[1]))
check(r[2] == 0x3C and r[3] == 0x5E and r[4] == 0x7F, "the registers were not put back: %s" % (r[2:5],))
check(r[5] == 0xF0, "S is %02X after the stub" % r[5])
# bits 4 and 5 are not real flags: PHP always pushes them set
check(r[6] & 0xCF == 0xA5 & 0xCF, "P is %02X after the stub" % r[6])
check(m.ram[NEXT] == 0xA9 and m.ram[NEXT + 1] == 0x11 and m.ram[NEXT + 2] == 0x22,
      "the three bytes were not put back: %02X %02X %02X" % (m.ram[NEXT], m.ram[NEXT + 1], m.ram[NEXT + 2]))
check(m.ram[0x100 + defs["SV_MAGIC2"]] == defs["SV_MARK2"], "the second magic is not set")

# ---- the load stub ---------------------------------------------------------
m = M()
stub = bytearray(load)
for i in range(16):
    stub[defs["LD_V1"] + i] = 0x30 + i
    stub[defs["LD_V2"] + i] = 0x40 + i
fix = [(0x0500, 0xAA), (0x0501, 0xBB), (0x0502, 0xCC), (0x0600, 0xDD), (0x0601, 0xEE), (0x0602, 0xFF)]
for i, (ad, v) in enumerate(fix):
    o = defs["LDP_SLOT0"] + 5 * i
    stub[o + 1] = v
    stub[o + 3] = ad & 0xFF
    stub[o + 4] = ad >> 8
stub[defs["LDP_SP"]] = 0xE8
stub[defs["LDP_PCH"]] = 0x12
stub[defs["LDP_PCL"]] = 0x34
stub[defs["LDP_P"]] = 0x65
stub[defs["LDP_X"]] = 0x77
stub[defs["LDP_Y"]] = 0x88
stub[defs["LDP_A"]] = 0x99
m.ram[0x100:0x100 + size] = bytes(stub)
r = run(m, 0x100, s=0x40)
check(r[0] == "rti", "the load stub ended with %s" % r[0])
check(r[1] == 0x1234, "it returned to %04X" % r[1])
check(r[2] == 0x99 and r[3] == 0x77 and r[4] == 0x88, "A/X/Y are %s" % (r[2:5],))
check(r[5] == 0xE8, "S is %02X after the RTI" % r[5])
check(r[6] & 0xCF == 0x65 & 0xCF, "P is %02X" % r[6])
for ad, v in fix:
    check(m.ram[ad] == v, "the byte at %04X is %02X, not %02X" % (ad, m.ram[ad], v))
check(m.ram[0x100 + defs["LD_MAGIC"]] == defs["SV_MARK2"], "the load stub set no magic")
# the VIA writes: IER cleared first, then every register but the IFR
w = m.written
check(w[0] == (0x180E, 0x7F) and w[1] == (0x1C0E, 0x7F), "the IERs were not cleared first: %s" % w[:2])
wrote = {a for a, _ in w}
for i in range(16):
    if i == 13:
        check(0x1800 + i not in wrote, "the stub wrote the VIA1 IFR")
        check(0x1C00 + i not in wrote, "the stub wrote the VIA2 IFR")
    else:
        check(m.via[0x1800 + i] == 0x30 + i, "VIA1[%d] was set to %02X" % (i, m.via.get(0x1800 + i)))
        check(m.via[0x1C00 + i] == 0x40 + i, "VIA2[%d] was set to %02X" % (i, m.via.get(0x1C00 + i)))
# ACR and PCR before the timers are started
def first_write(ad):
    for k, (a, _) in enumerate(w):
        if a == ad:
            return k
    return 10 ** 6
check(first_write(0x180B) < first_write(0x1805), "ACR is written after T1 is started")
check(first_write(0x180C) < first_write(0x1805), "PCR is written after T1 is started")
check(first_write(0x1C0B) < first_write(0x1C05), "VIA2 ACR is written after T1 is started")

print("stubs: %d checks, %d failed" % (checks, len(fails)))
for f in fails:
    print("  FAIL:", f)
sys.exit(1 if fails else 0)
