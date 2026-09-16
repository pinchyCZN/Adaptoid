#!/usr/bin/env python3
"""
originmap.py -- keep src_drv/origin.txt honest.

origin.txt records, for every function in the replacement driver, which
function in which original binary it came from. It exists so that when
the reconstruction misbehaves you can get back to the evidence in Ghidra
without guessing, and it is a FILE rather than a tag in a comment so it
stays out of the source and can be diffed, grepped and checked.

    python tools/originmap.py --check    report drift, exit 1 if any
    python tools/originmap.py --fix      rewrite the table aligned

WHAT --check ACTUALLY VERIFIES

  - every function defined in src_drv/*.c has a row;
  - every row names a function that still exists;
  - every row is well formed: a known kind, a plausible address, and a
    binary this project actually has;
  - a row whose kind is new carries no address, and one whose kind is
    port or part carries one.

WHAT IT CANNOT VERIFY

  That an address still names the function the row claims. The Ghidra
  database is not reachable from here and decomp/ is not in git. The
  addresses were read out of the database by hand when the row was
  written. They do not rot - the binaries are immutable primary evidence
  and are never rebuilt - so the risk is a typo at entry, not decay.
  Check a row against Ghidra when you rely on it.

WHY THE TABLE IS SPACE-ALIGNED AND NOT A TSV

  A .tsv is not in the extension set asciify.py walks, so it would get
  no ASCII check and no column check at all. As an RFC table in a .txt
  it gets both, and asciify.py additionally verifies that every row is
  exactly as wide as its border - which is a free check that --fix did
  its job.
"""

import argparse
import io
import os
import re
import sys

MAP_PATH = os.path.join("src_drv", "origin.txt")
SRC_DIR = "src_drv"

# (heading, inner width). One leading space is part of the width.
COLS = [
    ("Replacement", 26),
    ("File", 11),
    ("Kind", 6),
    ("Binary", 14),
    ("Addr", 10),
    ("Original", 35),
]

KINDS = ("port", "part", "new")
BINARIES = ("wishk201.sys", "wishd201.exe", "wishh201.dll", "-")

INDENT = "   "

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


def cell(text, width):
    s = " " + text
    if len(s) > width:
        raise SystemExit("originmap: cell %r needs %d columns, have %d"
                         % (text, len(s), width))
    return s.ljust(width)


def border(fill):
    return INDENT + "+" + "+".join(fill * w for _, w in COLS) + "+"


def render(rows):
    out = [border("-"),
           INDENT + "|" + "|".join(cell(h, w) for h, w in COLS) + "|",
           border("=")]
    for r in rows:
        out.append(INDENT + "|"
                   + "|".join(cell(v, w) for v, (_, w) in zip(r, COLS))
                   + "|")
        out.append(border("-"))
    widths = set(len(l) for l in out)
    assert len(widths) == 1, "ragged table: %s" % sorted(widths)
    return out


def is_table_line(line):
    s = line.strip()
    return (s.startswith("+") and s.endswith("+")) or \
           (s.startswith("|") and s.endswith("|"))


def parse_map(text):
    """
    Return (rows, first_line_index, last_line_index) for THE MAP TABLE.

    The document contains more than one table - section 2 explains the
    kinds in a two-column one - so the table is located by its header
    row rather than by being the first thing that looks like a table,
    and the span is then walked outward over contiguous table lines.
    """
    lines = text.split("\n")

    head = None
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith("|") and s.endswith("|"):
            if s[1:-1].split("|")[0].strip() == COLS[0][0]:
                head = i
                break
    if head is None:
        return [], None, None

    lo = head
    while lo > 0 and is_table_line(lines[lo - 1]):
        lo -= 1
    hi = head
    while hi + 1 < len(lines) and is_table_line(lines[hi + 1]):
        hi += 1

    rows = []
    for i in range(lo, hi + 1):
        s = lines[i].strip()
        if not (s.startswith("|") and s.endswith("|")):
            continue
        cells = [c.strip() for c in s[1:-1].split("|")]
        if len(cells) != len(COLS):
            raise SystemExit("originmap: %s line %d has %d cells, expected %d"
                             % (MAP_PATH, i + 1, len(cells), len(COLS)))
        if cells[0] in (COLS[0][0], ""):
            continue
        rows.append(cells)

    return rows, lo, hi


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
    bad = 0
    seen = {}
    for cells in rows:
        fn, fl, kind, binary, addr, origin = cells

        if kind not in KINDS:
            print("%s: unknown kind %r" % (fn, kind))
            bad += 1
        if binary not in BINARIES:
            print("%s: unknown binary %r" % (fn, binary))
            bad += 1

        if kind == "new":
            if addr != "-" or origin != "-":
                print("%s: kind new must have no address or origin" % fn)
                bad += 1
        else:
            if not re.match(r"^[0-9a-f]{8}$", addr):
                print("%s: address %r is not 8 lowercase hex digits"
                      % (fn, addr))
                bad += 1
            if not origin or origin == "-":
                print("%s: kind %s needs an original function name"
                      % (fn, kind))
                bad += 1

        if fn not in funcs:
            print("%s: row names a function that is not defined in %s/"
                  % (fn, SRC_DIR))
            bad += 1
        elif funcs[fn] != fl:
            print("%s: row says %s, definition is in %s"
                  % (fn, fl, funcs[fn]))
            bad += 1

        seen.setdefault(fn, 0)
        seen[fn] += 1

    for fn in sorted(funcs):
        if fn not in seen:
            print("%s (%s): defined but has no row in %s"
                  % (fn, funcs[fn], MAP_PATH))
            bad += 1

    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--fix", action="store_true")
    args = ap.parse_args()
    if not (args.check or args.fix):
        ap.error("one of --check or --fix is required")

    text = io.open(MAP_PATH, encoding="ascii").read()
    rows, lo, hi = parse_map(text)
    if lo is None:
        raise SystemExit("originmap: no table found in %s" % MAP_PATH)

    funcs = source_functions()

    if args.fix:
        lines = text.split("\n")
        new = lines[:lo] + render(rows) + lines[hi + 1:]
        out = "\n".join(new)
        if out != text:
            io.open(MAP_PATH, "w", encoding="ascii", newline="\n").write(out)
            print("rewrote %s (%d rows)" % (MAP_PATH, len(rows)))
        else:
            print("%s already aligned (%d rows)" % (MAP_PATH, len(rows)))
        return 0

    bad = check(rows, funcs)
    print("\n%d row(s), %d function(s) in %s/, %d problem(s)."
          % (len(rows), len(funcs), SRC_DIR, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
