#!/usr/bin/env python3
"""
originmap.py -- keep src_drv/origin.tsv honest.

origin.tsv records, for every function in the replacement driver, which
function in which original binary it came from. It exists so that when
the reconstruction misbehaves you can get back to the evidence in Ghidra
without guessing, and it is a FILE rather than a tag in a comment so it
stays out of the source and can be diffed, grepped and checked.

    python tools/originmap.py --check    report drift, exit 1 if any
    python tools/originmap.py --fix      sort and normalise

WHY TAB-SEPARATED AND NOT AN RFC TABLE

    It used to be an aligned table inside origin.txt. That cost about
    four bytes on disk per byte of content - half the table was padding
    and the +---+ separators outweighed the data - and, worse, the fixed
    column widths had exactly one character of headroom. Any new name a
    character longer than the current longest would have aborted the
    tool, and widening a column rewrites every row, so each such name
    churned the whole file. At a few hundred functions that is
    unworkable. A row here touches exactly one line and column widths do
    not exist.

    The prose that used to sit around the table stays in origin.txt:
    what the kinds mean, the entries that are not one to one, and how
    the addresses were established.

WHAT --check VERIFIES

  - every function defined in src_drv/*.c has a row, and every row names
    a function that still exists, in the file the row claims;
  - the schema: six fields, no empty or space-padded field, a known kind
    and binary, and an address that is eight lowercase hex digits;
  - a row whose kind is new carries no address or origin, and one whose
    kind is port or part carries both;
  - no duplicate rows;
  - ASCII only. asciify.py does not walk .tsv, so that check lives here
    now rather than being silently lost in the move from .txt.

WHAT IT CANNOT VERIFY

    That an address still names the function the row claims. The Ghidra
    database is not reachable from here and decomp/ is not in git. The
    addresses were read out of the database by hand when the row was
    written. They do not rot - the binaries are immutable primary
    evidence and are never rebuilt - so the risk is a typo at entry, not
    decay. Check a row against Ghidra when you rely on it.
"""

import argparse
import io
import os
import re
import sys

MAP_PATH = os.path.join("src_drv", "origin.tsv")
DOC_PATH = os.path.join("src_drv", "origin.txt")
SRC_DIR = "src_drv"

FIELDS = ("function", "file", "kind", "binary", "addr", "original")
KINDS = ("port", "part", "new")
BINARIES = ("wishk201.sys", "wishd201.exe", "wishh201.dll", "-")

HEADER = """# origin.tsv - where each function in the replacement came from.
#
# One tab-separated row per correspondence. A function may have several rows
# when it covers more than one original, or when one original splits across
# the core seam. See origin.txt for what the kinds mean and for the entries
# that are not one to one.
#
# THE ADDRESS IS THE DURABLE KEY, not the name: the binaries are immutable
# primary evidence and are never rebuilt, so an address cannot rot, while a
# Ghidra name can still be changed by later analysis.
#
# Checked and sorted by tools/originmap.py --check / --fix.
#
# %s
""" % "\t".join(FIELDS)

# A function DEFINITION in this tree starts at column 0 - the source is
# retabbed, so anything nested is indented - names an identifier before
# an open paren, and does not end in a semicolon, which is what
# separates it from a prototype.
DEF_RE = re.compile(
    r"^[A-Za-z_][A-Za-z0-9_ \t\*]*?([A-Za-z_][A-Za-z0-9_]*)\s*\(")
# NB: do NOT skip "static const" or "const " here. Both are legitimate
# starts for a function definition - report_name in harness.c returns a
# const char * - and skipping them silently loses rows. A const global
# with an initialiser is not a false positive, because DEF_RE requires an
# identifier immediately before an open paren and an array initialiser
# has none.
SKIP_PREFIX = ("#", "/", "*", "}", "typedef", "extern", "struct ", "union ",
               "enum ")


def load(path):
    """Return (rows, problems) for the tsv."""
    rows = []
    bad = []
    with io.open(path, "r", encoding="utf-8", newline="") as f:
        raw = f.read()

    for n, line in enumerate(raw.split("\n"), 1):
        if not line or line.startswith("#"):
            continue
        try:
            line.encode("ascii")
        except UnicodeEncodeError:
            bad.append("%s:%d: non-ASCII byte" % (path, n))
            continue
        parts = line.split("\t")
        if len(parts) != len(FIELDS):
            bad.append("%s:%d: %d fields, expected %d"
                       % (path, n, len(parts), len(FIELDS)))
            continue
        for i, p in enumerate(parts):
            if p != p.strip() or p == "":
                bad.append("%s:%d: field %s is empty or space-padded: %r"
                           % (path, n, FIELDS[i], p))
        rows.append((n, parts))
    return rows, bad


def source_functions():
    """{function name: file} for every definition in src_drv/*.c."""
    found = {}
    for name in sorted(os.listdir(SRC_DIR)):
        if not name.endswith(".c"):
            continue
        path = os.path.join(SRC_DIR, name)
        text = io.open(path, encoding="ascii").read()
        for line in text.split("\n"):
            if not line or line[0] in " \t":
                continue
            if line.rstrip().endswith(";"):
                continue
            stripped = line.strip()
            if any(stripped.startswith(p) for p in SKIP_PREFIX):
                continue
            m = DEF_RE.match(line)
            if m:
                found[m.group(1)] = name
    return found


def check(rows, funcs):
    bad = []
    seen = set()
    covered = set()

    for n, (fn, fl, kind, binary, addr, origin) in rows:
        key = (fn, fl, kind, binary, addr, origin)
        if key in seen:
            bad.append("%s:%d: duplicate row" % (MAP_PATH, n))
        seen.add(key)
        covered.add(fn)

        if kind not in KINDS:
            bad.append("%s:%d: %s: unknown kind %r" % (MAP_PATH, n, fn, kind))
        if binary not in BINARIES:
            bad.append("%s:%d: %s: unknown binary %r"
                       % (MAP_PATH, n, fn, binary))

        if kind == "new":
            if addr != "-" or origin != "-":
                bad.append("%s:%d: %s: kind new must have no address or origin"
                           % (MAP_PATH, n, fn))
        else:
            if not re.match(r"^[0-9a-f]{8}$", addr):
                bad.append("%s:%d: %s: address %r is not 8 lowercase hex digits"
                           % (MAP_PATH, n, fn, addr))
            if origin == "-":
                bad.append("%s:%d: %s: kind %s needs an original function name"
                           % (MAP_PATH, n, fn, kind))

        if fn not in funcs:
            bad.append("%s:%d: %s: not defined in %s/"
                       % (MAP_PATH, n, fn, SRC_DIR))
        elif funcs[fn] != fl:
            bad.append("%s:%d: %s: row says %s, definition is in %s"
                       % (MAP_PATH, n, fn, fl, funcs[fn]))

    for fn in sorted(funcs):
        if fn not in covered:
            bad.append("%s (%s): defined but has no row in %s"
                       % (fn, funcs[fn], MAP_PATH))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--fix", action="store_true")
    args = ap.parse_args()
    if not (args.check or args.fix):
        ap.error("one of --check or --fix is required")

    rows, problems = load(MAP_PATH)
    funcs = source_functions()

    if args.fix:
        if problems:
            for p in problems:
                print(p)
            print("\nrefusing to sort a file with malformed rows.")
            return 1
        data = [r for _, r in rows]
        data.sort(key=lambda r: (r[1], r[0], r[4]))
        out = HEADER + "".join("\t".join(r) + "\n" for r in data)
        old = io.open(MAP_PATH, encoding="ascii").read()
        if out != old:
            io.open(MAP_PATH, "w", encoding="ascii", newline="\n").write(out)
            print("sorted %s (%d rows)" % (MAP_PATH, len(data)))
        else:
            print("%s already sorted (%d rows)" % (MAP_PATH, len(data)))
        return 0

    problems += check(rows, funcs)
    for p in problems:
        print(p)
    print("\n%d row(s), %d function(s) in %s/, %d problem(s)."
          % (len(rows), len(funcs), SRC_DIR, len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
