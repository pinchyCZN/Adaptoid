#!/usr/bin/env python3
"""
retab.py -- tabs for indentation, spaces for alignment.

The project convention: one indent level is one TAB, and any whitespace
beyond the structural indent - lining a continuation line up under an
open paren, the ' * ' of a block comment, an ASCII diagram inside a
comment - stays SPACES. That is what makes the source render correctly
at any tab width instead of only at four.

Usage:
    python tools/retab.py --check [PATH ...]    report, exit 1 if any file
                                                would change
    python tools/retab.py --fix   [PATH ...]    rewrite in place

With no PATH, walks src_drv/ for .c, .h, .cpp and .inc.

HOW THE SPLIT IS DECIDED

    tabs   = min(structural_depth, leading_spaces // 4)
    spaces = leading_spaces - 4 * tabs

structural_depth is the brace nesting depth at the start of the line,
counted with strings, character literals and comments masked out. The
min() is what separates the two cases:

  - a line indented to its own block depth converts entirely to tabs;
  - a continuation line indented FURTHER than its depth keeps the
    excess as spaces, so it stays aligned with whatever it was lined up
    under;
  - a line indented LESS than its depth - a case label, a goto label -
    converts only as far as it actually goes.

Because tabs*4 + spaces always equals the original leading space count,
the rendered result at a four-column tab stop is byte-for-byte what it
was. --check verifies exactly that by expanding the output back, so a
bug in the scanner is caught rather than committed.

WHAT IS DELIBERATELY LEFT ALONE

  - docs/ and tools/*.py. The RFC documents in docs/ are specified in
    spaces (three-space bodies, four-space preformatted blocks) and
    asciify.py measures their columns; tabs there would be wrong. The
    Python is PEP 8.
  - lines whose leading whitespace is inside a string literal, which
    happens when a string is continued with a trailing backslash.
    Retabbing those would change the string.
"""

import argparse
import io
import os
import sys

TAB_WIDTH = 4
SRC_EXT = (".c", ".h", ".cpp", ".hpp", ".inc")
DEFAULT_ROOT = "src_drv"


def scan_line(line, state):
    """
    Walk one line, masking strings/chars/comments, and return
    (delta_depth, new_state).

    state is (in_block_comment, in_string, in_char). A string can only
    survive to the next line via a trailing backslash, which is exactly
    the case whose indentation must not be touched.
    """
    in_comment, in_string, in_char = state
    events = []                         # +1 / -1 per brace, in order
    i = 0
    n = len(line)
    code = []                           # the line with literals masked

    while i < n:
        c = line[i]

        if not (in_comment or in_string or in_char):
            code.append(c)

        if in_comment:
            if c == "*" and i + 1 < n and line[i + 1] == "/":
                in_comment = False
                i += 2
                continue
            i += 1
            continue

        if in_string:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_string = False
            i += 1
            continue

        if in_char:
            if c == "\\":
                i += 2
                continue
            if c == "'":
                in_char = False
            i += 1
            continue

        if c == "/" and i + 1 < n and line[i + 1] == "*":
            in_comment = True
            i += 2
            continue
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            break                       # rest of the line is a comment
        if c == '"':
            in_string = True
            i += 1
            continue
        if c == "'":
            in_char = True
            i += 1
            continue
        if c == "{":
            events.append(1)
        elif c == "}":
            events.append(-1)
        i += 1

    # A string or char literal only continues to the next line if the
    # line ended with a backslash; otherwise it was unterminated and we
    # close it rather than poison every following line.
    if (in_string or in_char) and not line.rstrip("\n").endswith("\\"):
        in_string = False
        in_char = False

    return events, (in_comment, in_string, in_char), "".join(code).strip()


def apply_braces(kinds, events, code):
    """
    Fold this line's braces into the open-brace stack and return the
    resulting INDENT depth.

    Not every brace is an indent level. `extern "C" {` wraps a whole
    header without indenting a line of it, so counting it would give
    every declaration inside a depth of 1 - harmless for the
    declarations themselves, which sit at column 0, but it would put a
    spurious tab on each continuation line and break their alignment at
    any tab width but four. Braces opened by such a line are pushed as
    non-indenting and their matching close pops them again.
    """
    # NB: code has had its string literals masked out, so the "C" of
    # `extern "C" {` is gone by the time we see it - match on the
    # keyword, not on the whole phrase.
    transparent = code.startswith("extern") or code.startswith("namespace ")
    for e in events:
        if e > 0:
            kinds.append("x" if transparent else "n")
        elif kinds:
            kinds.pop()
    return kinds.count("n")


CONTROL = ("if", "for", "while", "else", "do", "switch")


def opens_braceless_body(code):
    """
    True when this line is a control header whose body is NOT braced, so
    the following line is one indent level deeper than the brace count
    knows about:

        if (!s_pool)
            free(s_pool);       <- depth 1 by braces, 2 by eye

    Without this such a body comes out as one tab plus four spaces,
    which still reads as indented but only at a four-column tab stop.
    """
    if not code or code.endswith("{") or code.endswith(";"):
        return False
    if code == "else" or code == "do":
        return True
    head = code.split("(")[0].strip()
    if head.startswith("else"):
        head = head[4:].strip()
    return head in CONTROL and code.endswith(")")


def lead_columns(line):
    """
    (display columns of the leading whitespace, rest of the line).

    Tabs already present advance to the next multiple of TAB_WIDTH, so a
    file that is already partly converted measures the same as one that
    is not. Without this a single pre-existing tab makes the round-trip
    check fail on an otherwise correct file.
    """
    col = 0
    i = 0
    while i < len(line):
        if line[i] == " ":
            col += 1
        elif line[i] == "\t":
            col += TAB_WIDTH - (col % TAB_WIDTH)
        else:
            break
        i += 1
    return col, line[i:]


def convert(text):
    """Return the retabbed text."""
    out = []
    kinds = []                          # one entry per open brace
    depth = 0
    virtual = 0                         # unbraced control-statement bodies
    state = (False, False, False)       # comment, string, char

    for raw in text.split("\n"):
        in_comment, in_string, in_char = state
        continued_literal = in_string or in_char

        lead, stripped = lead_columns(raw)

        if continued_literal or not stripped or lead == 0:
            # Inside a continued literal, blank, or already at column 0:
            # nothing to do. Blank lines with trailing spaces are left
            # exactly as found - that is a separate concern.
            out.append(raw)
            events, state, code = scan_line(raw, state)
            depth = apply_braces(kinds, events, code)
            if code:
                virtual = virtual + 1 if opens_braceless_body(code) else 0
            continue

        # The depth this line SITS AT. A line opening with a closing
        # brace belongs to the enclosing block, not the one it closes.
        d = depth + virtual
        if stripped.startswith("}"):
            d = depth - 1
        if d < 0:
            d = 0

        tabs = min(d, lead // TAB_WIDTH)
        spaces = lead - TAB_WIDTH * tabs
        out.append("\t" * tabs + " " * spaces + stripped)

        events, state, code = scan_line(raw, state)
        depth = apply_braces(kinds, events, code)
        if code:
            # A new unbraced header stacks; anything else ends the body
            # the previous one opened.
            virtual = virtual + 1 if opens_braceless_body(code) else 0

    return "\n".join(out)


def expand(text):
    """
    Leading whitespace rendered as spaces at a four-column tab stop.

    Applied to BOTH sides of the round-trip check, so what is being
    asserted is "this file renders identically", not "this file was
    previously all spaces".
    """
    out = []
    for line in text.split("\n"):
        col, rest = lead_columns(line)
        out.append(" " * col + rest if rest else line)
    return "\n".join(out)


def gather(paths):
    files = []
    roots = paths if paths else [DEFAULT_ROOT]
    for root in roots:
        if os.path.isfile(root):
            files.append(root)
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames
                           if d not in (".git", "__pycache__", ".vs")]
            for name in sorted(filenames):
                if name.endswith(SRC_EXT):
                    files.append(os.path.join(dirpath, name))
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--fix", action="store_true")
    ap.add_argument("paths", nargs="*")
    args = ap.parse_args()
    if not (args.check or args.fix):
        ap.error("one of --check or --fix is required")

    changed = 0
    failed = 0

    for path in gather(args.paths):
        with io.open(path, "r", encoding="ascii", newline="") as f:
            original = f.read()

        converted = convert(original)

        # THE SAFETY NET. Expanding the result must reproduce the
        # input exactly. If it does not, the scanner mis-read
        # something and the file is left alone.
        if expand(converted) != expand(original):
            print("%s: ROUND TRIP FAILED - not touched" % path)
            failed += 1
            continue

        if converted == original:
            continue

        changed += 1
        if args.fix:
            with io.open(path, "w", encoding="ascii", newline="") as f:
                f.write(converted)
            print("retabbed %s" % path)
        else:
            print("would retab %s" % path)

    if args.check:
        print("\n%d file(s) would change, %d round-trip failure(s)."
              % (changed, failed))
    else:
        print("\n%d file(s) retabbed, %d round-trip failure(s)."
              % (changed, failed))

    return 1 if (failed or (args.check and changed)) else 0


if __name__ == "__main__":
    sys.exit(main())
