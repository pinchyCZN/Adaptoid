#!/usr/bin/env python3
"""
rfcfmt.py -- render project documents as RFC-style plain ASCII text.

Takes the loose markdown-ish source we tend to write and emits a document
laid out like an old RFC: numbered sections, three-space indented body
text wrapped at 72 columns, a generated table of contents, and tables
drawn with +---+ borders and space-aligned columns.

All inline markup is removed: **bold**, `code`, *emphasis*, and
[text](link) all collapse to their plain text.

Usage:
    python tools/rfcfmt.py --check  [PATH ...]   is the file already RFC form?
    python tools/rfcfmt.py --fix    [PATH ...]   rewrite in place
    python tools/rfcfmt.py --stdout PATH         preview one file

With no PATH, processes docs/ recursively.

Source conventions it understands:
    # Title            document title (first one only)
    ## Section         numbered 1., 2., ...
    ### Subsection     numbered 1.1., 1.2., ...
    #### Sub-sub       numbered 1.1.1., ...
    | a | b |          markdown table, redrawn with borders
    ```                fenced block, emitted verbatim, indented
    - item             bullet list, rendered with 'o'
    1. item            numbered list
    > quote            indented note block
"""

import argparse
import os
import re
import sys
import textwrap

BODY_COLS = 72          # RFC body text width
INDENT = "   "          # RFC body indent
TABLE_MAX = 116         # max table width incl. indent (limit is 120)
MIN_COL = 9             # never shrink a column below this


# ---------------------------------------------------------------- inline

def strip_inline(s):
    """Remove markdown inline markup, leaving plain text."""
    s = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s)   # [text](link)
    s = re.sub(r"~~([^~]+)~~", r"\1", s)             # strikethrough
    s = re.sub(r"\*\*([^*]+)\*\*", r"\1", s)         # bold
    s = re.sub(r"(?<!\w)\*([^*\n]+)\*(?!\w)", r"\1", s)
    s = re.sub(r"`([^`]*)`", r"\1", s)               # code span
    s = s.replace("**", "").replace("`", "")
    s = re.sub(r"  +", " ", s)   # collapse runs left by transliteration
    return s.strip()


# ---------------------------------------------------------------- parsing

def parse(text):
    """Split source into a list of (kind, payload) blocks."""
    lines = text.split("\n")
    blocks = []
    i = 0
    para = []

    def flush_para():
        if para:
            blocks.append(("para", " ".join(x.strip() for x in para)))
            del para[:]

    while i < len(lines):
        ln = lines[i]
        s = ln.strip()

        if s.startswith("```"):
            flush_para()
            i += 1
            buf = []
            while i < len(lines) and not lines[i].strip().startswith("```"):
                buf.append(lines[i])
                i += 1
            i += 1
            blocks.append(("code", buf))
            continue

        if s.startswith("#"):
            flush_para()
            level = len(s) - len(s.lstrip("#"))
            blocks.append(("head", (level, strip_inline(s.lstrip("#")))))
            i += 1
            continue

        # A block indented 4+ spaces is preformatted: address maps,
        # listings, ASCII diagrams. Emit verbatim, never rewrapped.
        if ln.startswith("    ") and s and not para:
            buf = []
            while i < len(lines):
                cur = lines[i]
                if cur.strip() and not cur.startswith("    "):
                    break
                buf.append(cur)
                i += 1
            while buf and not buf[-1].strip():
                buf.pop()
            strip_n = min(len(x) - len(x.lstrip())
                          for x in buf if x.strip())
            blocks.append(("code", [x[strip_n:] if x.strip() else ""
                                    for x in buf]))
            continue

        if s.startswith("|"):
            flush_para()
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                raw = lines[i].strip().strip("|")
                cells = [strip_inline(c) for c in raw.split("|")]
                if not all(re.fullmatch(r":?-{2,}:?", c.strip() or "-")
                           for c in cells):
                    rows.append(cells)
                i += 1
            if rows:
                blocks.append(("table", rows))
            continue

        if s.startswith("> "):
            flush_para()
            buf = []
            while i < len(lines) and lines[i].strip().startswith(">"):
                buf.append(strip_inline(lines[i].strip().lstrip(">")))
                i += 1
            blocks.append(("note", " ".join(buf)))
            continue

        m = re.match(r"^(\s*)([-*+]|\d+\.)\s+(.*)$", ln)
        if m:
            flush_para()
            items = []
            while i < len(lines):
                m2 = re.match(r"^(\s*)([-*+]|\d+\.)\s+(.*)$", lines[i])
                if m2:
                    items.append([len(m2.group(1)), m2.group(2),
                                  strip_inline(m2.group(3))])
                    i += 1
                elif (lines[i].strip() and lines[i].startswith(" ")
                      and items):
                    items[-1][2] += " " + strip_inline(lines[i])
                    i += 1
                else:
                    break
            blocks.append(("list", items))
            continue

        if not s:
            flush_para()
            i += 1
            continue

        if set(s) <= set("-=*_") and len(s) >= 3:
            flush_para()
            i += 1
            continue

        para.append(strip_inline(ln))
        i += 1

    flush_para()
    return blocks


# ---------------------------------------------------------------- tables

def size_columns(rows):
    """Pick a width for each column so the table fits TABLE_MAX."""
    ncols = max(len(r) for r in rows)
    rows[:] = [r + [""] * (ncols - len(r)) for r in rows]
    nat = [max(len(r[c]) for r in rows) for c in range(ncols)]

    def total(ws):
        return sum(ws) + 3 * len(ws) + 1

    widths = list(nat)
    budget = TABLE_MAX - len(INDENT)
    while total(widths) > budget:
        widest = max(range(ncols), key=lambda c: widths[c])
        if widths[widest] <= MIN_COL:
            break
        widths[widest] -= 1
    return widths


def render_table(rows):
    widths = size_columns(rows)
    sep = INDENT + "+" + "+".join("-" * (w + 2) for w in widths) + "+"
    out = [sep]
    for ri, row in enumerate(rows):
        wrapped = []
        for c, cell in enumerate(row):
            w = textwrap.wrap(cell, widths[c]) or [""]
            wrapped.append([x[:widths[c]] for x in w])
        height = max(len(w) for w in wrapped)
        for line in range(height):
            parts = []
            for c in range(len(widths)):
                txt = wrapped[c][line] if line < len(wrapped[c]) else ""
                parts.append(" " + txt.ljust(widths[c]) + " ")
            out.append(INDENT + "|" + "|".join(parts) + "|")
        out.append(sep)
        if ri == 0:
            out[-1] = (INDENT + "+"
                       + "+".join("=" * (w + 2) for w in widths) + "+")
    return out


# --------------------------------------------------------------- render

def number_headings(blocks):
    """Assign RFC section numbers; return (title, numbered_blocks)."""
    title = None
    counters = [0, 0, 0]
    out = []
    for kind, payload in blocks:
        if kind != "head":
            out.append((kind, payload))
            continue
        level, text = payload
        if level == 1 and title is None:
            title = text
            continue
        depth = max(1, min(3, level - 1))
        counters[depth - 1] += 1
        for d in range(depth, 3):
            counters[d] = 0
        num = ".".join(str(counters[d]) for d in range(depth))
        out.append(("section", (depth, num + ".", text)))
    return title or "Untitled", out


def render(text, project="MotoRacer PC Reverse Engineering"):
    blocks = parse(text)
    title, blocks = number_headings(blocks)

    out = []
    left = project
    right = "Analysis Note"
    out.append(left + " " * max(1, BODY_COLS - len(left) - len(right)) + right)
    out.append("")
    out.append("")
    t = title.upper()
    out.append(" " * max(0, (BODY_COLS - len(t)) // 2) + t)
    out.append("")
    out.append("")

    # Anything before the first numbered section is the abstract.
    first = next((i for i, (k, _) in enumerate(blocks)
                  if k == "section"), len(blocks))
    preamble, body = blocks[:first], blocks[first:]

    if preamble:
        out.append("Abstract")
        out.append("")
        out.extend(emit(preamble))
        out.append("")

    toc = [p for k, p in body if k == "section"]
    if toc:
        out.append("Table of Contents")
        out.append("")
        for depth, num, text_ in toc:
            lead = INDENT + "   " * (depth - 1)
            out.append(lead + num + " " + text_)
        out.append("")
        out.append("")

    out.extend(emit(body))

    while out and not out[-1].strip():
        out.pop()
    return "\n".join(out) + "\n"


def emit(blocks):
    out = []
    for kind, payload in blocks:
        if kind == "section":
            depth, num, text_ = payload
            out.append(num + "  " + text_)
            out.append("")
        elif kind == "para":
            out.extend(textwrap.wrap(payload, BODY_COLS,
                                     initial_indent=INDENT,
                                     subsequent_indent=INDENT) or [""])
            out.append("")
        elif kind == "note":
            body = textwrap.wrap(payload, BODY_COLS - 3,
                                 initial_indent=INDENT + "   ",
                                 subsequent_indent=INDENT + "   ")
            out.append(INDENT + "NOTE:")
            out.extend(body)
            out.append("")
        elif kind == "list":
            for ind, marker, txt in payload:
                extra = "   " * (1 if ind >= 2 else 0)
                bullet = "o " if marker in "-*+" else marker + " "
                out.extend(textwrap.wrap(
                    txt, BODY_COLS,
                    initial_indent=INDENT + extra + bullet,
                    subsequent_indent=INDENT + extra + " " * len(bullet)))
            out.append("")
        elif kind == "table":
            out.extend(render_table(payload))
            out.append("")
        elif kind == "code":
            for ln in payload:
                out.append((INDENT + ln).rstrip())
            out.append("")

    while out and not out[-1].strip():
        out.pop()
    return out


# ----------------------------------------------------------------- main

def already_rfc(text):
    """True if this file has already been rendered by rfcfmt.

    rfcfmt is a ONE-WAY converter, not a round-trip formatter. Running it
    on its own output would flatten the RFC layout back into paragraphs,
    so refuse unless --force is given.
    """
    head = text.split("\n")[:12]
    return any(x.startswith("Table of Contents") or x.startswith("Abstract")
               for x in head) and "Analysis Note" in (head[0] if head else "")


def targets(paths):
    if not paths:
        paths = ["docs"]
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                for fn in sorted(files):
                    if fn.endswith(".txt"):
                        yield os.path.join(root, fn)
        elif p.endswith(".txt"):
            yield p


def main():
    ap = argparse.ArgumentParser(description="Render docs as RFC text.")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--fix", action="store_true")
    ap.add_argument("--stdout", action="store_true")
    ap.add_argument("--force", action="store_true",
                    help="re-render a file that is already in RFC form")
    ap.add_argument("paths", nargs="*")
    a = ap.parse_args()

    done = skipped = 0
    for p in targets(a.paths):
        src = open(p, encoding="utf-8").read()
        rel = p.replace("\\", "/")

        if already_rfc(src) and not a.force:
            if not a.stdout:
                print("skip %s (already RFC form)" % rel)
                skipped += 1
            continue

        new = render(src)
        if a.stdout:
            sys.stdout.write(new)
            return 0
        if a.fix:
            open(p, "w", encoding="ascii", newline="\n").write(new)
            print("formatted %s" % rel)
            done += 1
        else:
            print("%s: would be reformatted" % rel)
            done += 1

    print("\n%d file(s) processed, %d already formatted." % (done, skipped))
    return 0


if __name__ == "__main__":
    sys.exit(main())
