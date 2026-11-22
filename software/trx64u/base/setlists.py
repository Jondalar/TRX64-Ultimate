#!/usr/bin/env python3
"""setlists.h for setting.cc: every choice setting (core_setting) of menu.toml with its values, so that setting.cc knows which keys the menu has (and keeps
their indexes in the base's settings), their number of values, a range's low end and base, and the labels. Written by the Makefile from the menu.toml the
library carries as its record; never edited by hand.

    python3 setlists.py <SDK tools dir> menu.toml setlists.h

The TOML is read by the SDK's descrec.py (its reader runs on the build image's Python 3.10). A setting with `range` keeps its low end and count, not its
labels ("%d<unit>" each); a text setting is left out (its state is the text)."""
import sys


def c_string(s):
    out = []
    for b in s.encode("utf-8"):
        c = chr(b)
        if 32 <= b < 127 and c not in '"\\?':
            out.append(c)
        else:
            out.append("\\%03o" % b)
    return '"%s"' % "".join(out)


def main(argv):
    if len(argv) != 3:
        sys.exit("setlists.py <SDK tools dir> menu.toml setlists.h")
    sys.path.insert(0, argv[0])
    import descrec
    doc = descrec.toml_loads(open(argv[1], encoding="utf-8").read())
    lists = doc.get("lists", {})
    rows = []
    for cs in doc.get("core_setting", []):
        if cs.get("text") or cs.get("secret"):
            continue
        key = cs["key"]
        if "range" in cs:
            lo, hi = cs["range"]
            rows.append((key, None, hi - lo + 1, lo, cs.get("base", lo)))
        else:
            vals = lists[cs["list"]] if "list" in cs else cs["values"]
            rows.append((key, vals, len(vals), 0, cs.get("base", 0)))
    rows.sort(key=lambda r: r[0].encode("utf-8"))                     # strcmp's order: setting.cc searches by halves
    with open(argv[2], "w", encoding="utf-8") as f:
        f.write("// Generated from menu.toml by setlists.py (base/Makefile). Do not edit.\n")
        f.write("// key, the labels (each ends in NUL; null: a range), the number of values, a range's low end, the base.\n")
        f.write("static const SetDef set_defs[] = {\n")
        for key, vals, n, lo, base in rows:
            labels = " ".join(c_string(v)[:-1] + '\\000"' for v in vals) if vals else "0"
            f.write("    { %s, %s, %d, %d, %d },\n" % (c_string(key), labels, n, lo, base))
        f.write("};\n")


if __name__ == "__main__":
    main(sys.argv[1:])
