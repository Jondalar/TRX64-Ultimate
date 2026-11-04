#!/usr/bin/env python3
"""
mk_u2a.py - build a .u2a app file (format version 2).

Usage:
    mk_u2a.py --variant nios2=<app.elf> [--variant riscv32=<app.elf>]
              [--manifest <manifest.json>] --name <name> --out <file.u2a>

Every variant ELF is linked for the app area at 0x03A00000. Its allocated
sections form the payload (what objcopy -O binary produces), _app_start gives
the entry offset, and the area from the payload end up to _bss_end is zeroed by
the loader.

File layout (doc/AppLoader_Spec.md section 1, little-endian):
    header (64 bytes) | variant table (24 bytes each) | manifest (JSON, NUL) | payloads

The manifest is validated against the rules of section 2, minified and
embedded with a terminating NUL.
"""

import argparse
import json
import re
import struct
import sys
from pathlib import Path


U2A_MAGIC             = 0x50413255   # "U2AP"
U2A_HEADER_SIZE       = 64
U2A_VARIANT_SIZE      = 24
U2A_FORMAT_VERSION    = 2
U2A_MAX_VARIANTS      = 4
U2A_MANIFEST_MAX_SIZE = 4096
U2A_API_MIN           = 2
U2A_API_MAX           = 2
U2A_LOAD_ADDR         = 0x03A00000
U2A_AREA_SIZE         = 0x00200000
U2A_NAME_LEN          = 27           # name[28] keeps a terminating NUL

EM_NIOS2 = 113
EM_RISCV = 243

# variant name -> (U2A arch id, ELF machine)
ARCHS = {
    "nios2":   (1, EM_NIOS2),
    "riscv32": (2, EM_RISCV),
}

SHT_SYMTAB = 2
SHT_NOBITS = 8
SHF_ALLOC  = 0x2

IDENT_RE = re.compile(r"[a-z0-9_]+")


class BuildError(Exception):
    pass


# ---------------------------------------------------------------------------
# ELF
# ---------------------------------------------------------------------------

class Elf32:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
            raise BuildError(f"{path}: not a 32-bit little-endian ELF file")
        (_, self.machine, _, _, _, shoff, _, _, _, _,
         shentsize, shnum, _) = struct.unpack_from("<HHIIIIIHHHHHH", d, 16)
        # (name, type, flags, addr, offset, size, link, info, addralign, entsize)
        self.sections = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize)
                         for i in range(shnum)]

    def payload(self) -> bytes:
        """Allocated sections with contents, from U2A_LOAD_ADDR to the last byte."""
        chunks = []
        for _, stype, flags, addr, offset, size, _, _, _, _ in self.sections:
            if (flags & SHF_ALLOC) and stype != SHT_NOBITS and size > 0:
                chunks.append((addr, self.data[offset:offset + size]))
        if not chunks:
            raise BuildError(f"{self.path}: no loadable sections")
        start = min(addr for addr, _ in chunks)
        if start != U2A_LOAD_ADDR:
            raise BuildError(f"{self.path}: first section at 0x{start:08x}, "
                             f"expected 0x{U2A_LOAD_ADDR:08x}")
        end = max(addr + len(blob) for addr, blob in chunks)
        image = bytearray(end - start)
        for addr, blob in chunks:
            image[addr - start:addr - start + len(blob)] = blob
        return bytes(image)

    def symbols(self) -> dict:
        syms = {}
        for _, stype, _, _, offset, size, link, _, _, entsize in self.sections:
            if stype != SHT_SYMTAB:
                continue
            str_offset = self.sections[link][4]
            for i in range(size // (entsize or 16)):
                st_name, st_value = struct.unpack_from("<II", self.data, offset + i * 16)
                end = self.data.index(b"\0", str_offset + st_name)
                name = self.data[str_offset + st_name:end].decode("ascii", "replace")
                if name:
                    syms[name] = st_value
        return syms


def build_variant(spec: str):
    """Parse 'arch=elf' and return (arch_id, entry_offset, payload, bss_size)."""
    arch_name, sep, elf_name = spec.partition("=")
    if not sep or arch_name not in ARCHS or not elf_name:
        raise BuildError(f"--variant {spec}: expected <arch>=<elf> with arch in "
                         f"{', '.join(ARCHS)}")
    arch_id, machine = ARCHS[arch_name]
    elf = Elf32(Path(elf_name))
    if elf.machine != machine:
        raise BuildError(f"{elf_name}: ELF machine {elf.machine} does not match {arch_name}")

    payload = elf.payload()
    syms = elf.symbols()
    if "_app_start" not in syms:
        raise BuildError(f"{elf_name}: _app_start not found")
    entry_offset = syms["_app_start"] - U2A_LOAD_ADDR
    if not 0 <= entry_offset < len(payload):
        raise BuildError(f"{elf_name}: _app_start outside the payload")

    # Zero from the payload end (which may be unaligned) up to _bss_end.
    payload_end = U2A_LOAD_ADDR + len(payload)
    bss_size = max(0, syms["_bss_end"] - payload_end) if "_bss_end" in syms else 0
    if len(payload) + bss_size > U2A_AREA_SIZE:
        raise BuildError(f"{elf_name}: payload + bss ({len(payload) + bss_size} bytes) "
                         f"exceed the app area ({U2A_AREA_SIZE} bytes)")
    return arch_name, arch_id, entry_offset, payload, bss_size


# ---------------------------------------------------------------------------
# Manifest
# ---------------------------------------------------------------------------

def no_duplicate_keys(pairs):
    obj = {}
    for key, value in pairs:
        if key in obj:
            raise BuildError(f"manifest: duplicate key '{key}'")
        obj[key] = value
    return obj


def check_text(obj: dict, key: str, max_len: int, where: str, ident: bool = False) -> None:
    value = obj.get(key)
    if not isinstance(value, str):
        raise BuildError(f"manifest: {where}'{key}' is required and must be a string")
    if not 1 <= len(value.encode("utf-8")) <= max_len:
        raise BuildError(f"manifest: {where}'{key}' must be 1..{max_len} bytes")
    if ident and not IDENT_RE.fullmatch(value):
        raise BuildError(f"manifest: {where}'{key}' may only contain a-z, 0-9 and _")
    # The firmware JSON parser does not decode escape sequences.
    if any(c in '"\\' or ord(c) < 0x20 for c in value):
        raise BuildError(f"manifest: {where}'{key}' must not contain quotes, "
                         f"backslashes or control characters")


def check_list(obj: dict, key: str, max_entries: int) -> list:
    value = obj.get(key, [])
    if not isinstance(value, list) or len(value) > max_entries:
        raise BuildError(f"manifest: '{key}' must be a list of at most {max_entries} entries")
    for i, entry in enumerate(value):
        if not isinstance(entry, dict):
            raise BuildError(f"manifest: {key}[{i}] must be an object")
    return value


def build_manifest(path: Path) -> bytes:
    try:
        obj = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=no_duplicate_keys)
    except json.JSONDecodeError as e:
        raise BuildError(f"{path}: {e}")
    if not isinstance(obj, dict):
        raise BuildError(f"{path}: the manifest must be a JSON object")

    check_text(obj, "id", 31, "", ident=True)
    check_text(obj, "name", 27, "")
    check_text(obj, "version", 15, "")
    for i, ft in enumerate(check_list(obj, "filetypes", 8)):
        where = f"filetypes[{i}]."
        check_text(ft, "ext", 7, where)
        if "." in ft["ext"]:
            raise BuildError(f"manifest: {where}'ext' must be given without dot")
        check_text(ft, "label", 31, where)
        check_text(ft, "action", 15, where, ident=True)
    for i, task in enumerate(check_list(obj, "tasks", 8)):
        where = f"tasks[{i}]."
        check_text(task, "category", 31, where)
        check_text(task, "label", 31, where)
        check_text(task, "action", 15, where, ident=True)
    rest = obj.get("rest", [])
    if not isinstance(rest, list) or len(rest) > 8:
        raise BuildError("manifest: 'rest' must be a list of at most 8 actions")
    for i, action in enumerate(rest):
        check_text({"action": action}, "action", 15, f"rest[{i}].", ident=True)

    blob = json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8") + b"\0"
    if len(blob) > U2A_MANIFEST_MAX_SIZE:
        raise BuildError(f"{path}: minified manifest is {len(blob)} bytes, "
                         f"max {U2A_MANIFEST_MAX_SIZE}")
    return blob


# ---------------------------------------------------------------------------
# .u2a
# ---------------------------------------------------------------------------

def build_u2a(name: str, variant_specs: list, manifest_path) -> tuple:
    name_bytes = name.encode("ascii", errors="replace")
    if not 1 <= len(name_bytes) <= U2A_NAME_LEN:
        raise BuildError(f"--name must be 1..{U2A_NAME_LEN} characters")
    if not 1 <= len(variant_specs) <= U2A_MAX_VARIANTS:
        raise BuildError(f"give 1..{U2A_MAX_VARIANTS} --variant options")

    variants = [build_variant(spec) for spec in variant_specs]
    arch_names = [v[0] for v in variants]
    if len(set(arch_names)) != len(arch_names):
        raise BuildError("each architecture may appear only once")

    manifest = build_manifest(manifest_path) if manifest_path else b""
    offset = U2A_HEADER_SIZE + U2A_VARIANT_SIZE * len(variants)
    manifest_offset = offset if manifest else 0
    offset += len(manifest)

    table = b""
    payloads = b""
    for _, arch_id, entry_offset, payload, bss_size in variants:
        table += struct.pack("<HHIIIII", arch_id, 0, U2A_LOAD_ADDR, entry_offset,
                             offset + len(payloads), len(payload), bss_size)
        payloads += payload

    header = struct.pack("<IHHHHHHIII28s8s",
                         U2A_MAGIC, U2A_HEADER_SIZE, U2A_FORMAT_VERSION,
                         U2A_API_MIN, U2A_API_MAX,
                         len(variants), len(manifest),
                         U2A_HEADER_SIZE, manifest_offset, 0,
                         name_bytes.ljust(28, b"\0"), bytes(8))
    assert len(header) == U2A_HEADER_SIZE
    assert len(table) == U2A_VARIANT_SIZE * len(variants)
    return header + table + manifest + payloads, variants, len(manifest)


def main() -> int:
    ap = argparse.ArgumentParser(description="Build a .u2a app file (format version 2).")
    ap.add_argument("--variant", action="append", default=[], metavar="ARCH=ELF",
                    help="architecture build to include (nios2 or riscv32); repeatable")
    ap.add_argument("--manifest", type=Path, default=None,
                    help="manifest JSON to validate and embed")
    ap.add_argument("--name", required=True, help=f"display name (<= {U2A_NAME_LEN} chars)")
    ap.add_argument("--out", required=True, type=Path, help="output .u2a file")
    args = ap.parse_args()

    try:
        blob, variants, manifest_size = build_u2a(args.name, args.variant, args.manifest)
    except (BuildError, OSError) as e:
        print(f"mk_u2a: error: {e}", file=sys.stderr)
        return 1

    args.out.write_bytes(blob)
    parts = ", ".join(f"{arch} {len(payload)} bytes entry=0x{entry:x} bss={bss}"
                      for arch, _, entry, payload, bss in variants)
    print(f"wrote {args.out} ({len(blob)} bytes; manifest {manifest_size} bytes; {parts})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
