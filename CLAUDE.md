# ADAPTOID

Reverse engineering the Adaptoid N64-to-USB controller driver + configurator,
with the goal of writing a clean-room replacement that runs on modern 64-bit
Windows.

## What the original does (and why it matters)

The Adaptoid is unusual among joy-to-key adapters: remapping happens **in the
driver**, not in a user-mode hook. The driver registers via
`HidRegisterMinidriver` and reports a **composite HID descriptor** (keyboard +
mouse + gamepad in one device), so any part of the N64 controller can be
presented to the OS as real key or mouse input. Every application sees genuine
HID input, which is why it works in games and software that ignore user-level
key injection.

Preserving this property - composite HID, remapping below the application layer
- is the core requirement of the replacement driver.

## Artifacts

| Path | What it is |
| --- | --- |
| `decomp/wishk201.sys` | Kernel driver. x86 32-bit, image base `0x10000`, 187 functions. HID minidriver over USBD. |
| `decomp/wishd201.exe` | Configurator / scripting UI. x86 32-bit, image base `0x400000`, 619 functions. |
| `decomp/wishh201.dll` | System-wide `WH_SHELL` hook DLL for game-launch detection. x86 32-bit, image base `0x10000000`, 112 functions. |
| `decomp/adaptoid.gpr` + `adaptoid.rep/` | Ghidra project holding both binaries. |
| `docs/` | Findings, as RFC-style ASCII `.txt`. |
| `tools/` | Ghidra scripts and the documentation formatters. |

Both binaries are dated 2001. They are **primary evidence - never modify, move,
rename, or rebuild them.** All analysis output goes into new files or into
Ghidra annotations.

## Ghidra access  -  ALWAYS NAME THE BINARY

The Ghidra project `adaptoid` is live over MCP (TCP `127.0.0.1:8089`) with
**both programs open simultaneously**. This is the single easiest way to corrupt
this project's work, so the rule is absolute:

> **Every program-scoped Ghidra MCP call must name its target program
> explicitly.
> No exceptions, reads included.**

Selector parameters, depending on the tool:

| Selector | Used by |
| --- | --- |
| `program=` | the vast majority of tools |
| `source_program=` / `target_program=` | transfer/merge tools |
| `program_a=` / `program_b=` | diff and compare tools |

Valid values are exactly `"wishk201.sys"` (driver), `"wishd201.exe"`
(configurator) and `"wishh201.dll"` (launch-detection hook).

### Why this is strict

Upstream, the selector defaults to the empty string and the bridge strips empty
values before dispatch, so an omitted selector does **not** error - it silently
targets whichever program is *active* and returns a plausible-looking result
from the wrong binary.

The local bridge has been patched to refuse that. Program-scoped calls that omit
their selector now raise a hard error, gated to the Ghidra project named
`adaptoid` so other single-binary projects are unaffected. It fails **open** if
the project name cannot be determined, so treat the guard as a safety net, not a
licence to omit the selector.

Do not use `switch_program` to change the active program as a workaround. It
makes the meaning of an omitted selector vary over the session, which turns a
consistent bug into an intermittent one. Pass the selector instead.

### Self-check: the address ranges do not overlap

| Program | Ranges |
| --- | --- |
| `wishk201.sys` | `00010000`-`0001adff`, plus `ffdff000`-`ffdfffff` (`tdb`/KPCR block) |
| `wishd201.exe` | `00400000`-`004a1ba4` |
| `wishh201.dll` | `10000000`-`1000b5ff`, plus `ffdff000`-`ffdfffff` (`tdb` block) |

Entry points are `000115f0` (driver), `00416540` (configurator) and
`10001240` (hook DLL).

Note that `wishk201.sys` and `wishh201.dll` both carry a `tdb` block at
`ffdff000`; that range alone does not identify the binary.

Use this as a free verification after every call: if addresses come back in the
`004xxxxx` range from a driver query, or `0001xxxx` from a configurator query,
the call was misrouted - discard the result and reissue it. Treat any surprising
result (function not found, unexpected count, unfamiliar names) as a suspected
misroute *first*, before concluding anything about the binary.

### Writes are the sharp edge

Reads that misroute waste time. Writes that misroute leave permanent wrong data
in the wrong project database. Before any renaming, prototype change, comment,
or label batch, state in prose which binary is being modified, then pass the
selector on every call in the batch.

Annotating Ghidra (renaming functions/variables, setting prototypes, adding
comments and plate comments) **is** the deliverable of analysis work - do it as
you go rather than keeping findings only in chat. Name things after what they
do, and record the evidence for a name in a comment. Since the two binaries talk
to each other, prefix shared-contract names so the side is unambiguous on sight
(e.g. `drv_` / `cfg_`).

### Declare the target up front

Open each analysis task by naming the binary under examination, and say so again
when switching. "Which binary?" should never need to be inferred from context.

## Git  -  read only

The agent has **read-only** access to git. Allowed: `git log`, `git show`, `git
diff`, `git status`, `git blame`, and reading files.

Do **not** run: `commit`, `add`, `push`, `pull`, `fetch`, `merge`, `rebase`,
`reset`, `checkout`/`switch`, `branch`, `tag`, `stash`, `clean`, or anything
that writes to `.git/`. The user drives all git operations.

Creating and editing files in the working tree is fine - that is not a git
operation. Note the repo currently has **no commits**; `git log` will fail until
the user makes one.

## Ghidra is the record, not the transcript

Analysis that lives only in a chat reply is lost work. The moment something is
determined, it goes into the Ghidra database in the same session:

- A function's purpose is known -> **rename it**. Not `FUN_00011a20`, a real
  name.
- Arguments or return type are known -> **set the prototype**, including calling
  convention for the driver's dispatch routines.
- A structure is understood -> **create the struct** and apply it to the
  variables and parameters that use it, rather than leaving raw offsets.
- Any non-obvious conclusion -> **plate comment** on the function, stating what
  it does and the evidence for that claim. Field-level and inline comments for
  the rest.
- Names for the two sides of a shared contract get `drv_` / `cfg_` prefixes.

Never guess at an address, offset, field, or signature. Read it out of the
Ghidra DB. If the MCP is down, stop and say so rather than substituting
recollection for a lookup.

The database is live and shared with the user's open Ghidra GUI - edits through
MCP are real edits to their project. Batch them deliberately; don't churn.
Partial knowledge still gets written down: a name like `drv_hid_report_unk_flag`
plus a comment saying what is and is not established beats leaving a default
name and a note in chat.

## Documentation: plain ASCII, RFC style

Documents live in `docs/` and `tools/` as `.txt`. `CLAUDE.md` is the single
exception - Claude Code requires that name, and markdown is correct here because
the harness renders it.

**Plain ASCII only.** Every byte in every document, script, and source comment
must be `<= 0x7F`. No em dashes, curly quotes, arrows, ellipsis glyphs, accented
letters, or box-drawing characters. Write `->` not an arrow, `-` not an em dash,
`...` not an ellipsis, and use `+` `-` `|` for diagrams.

**Laid out like an old RFC, not like markdown:**

```
Adaptoid Reverse Engineering                               Analysis Note


                        HID REPORT DESCRIPTOR


Abstract

   Body text indented three spaces, wrapped at 80 columns.

1.  First Section

   +--------------+-------+
   | Column       | Count |
   +==============+=======+
   | aligned with | 2542  |
   +--------------+-------+
```

- No markdown syntax in a `.txt` file: no `#` headings, no `**bold**`, no
  backticks, no `[text](link)`, no `|`-delimited markdown tables. If it needs a
  renderer, it is wrong.
- Tables are drawn with `+---+` borders and space-aligned columns, `+===+` under
  the header row. Every `|` row must be exactly as wide as the border above it.
- Numbered sections (`1.`, `1.1.`) with a Table of Contents once a document
  warrants one.
- **Body text indented three spaces, wrapped at 80 columns. Tables may run to
  120** including their indent.
- Blocks indented four or more spaces are preformatted - register maps,
  descriptor hex dumps, diagrams - and are never rewrapped.

Two tools from the MotoRacer project enforce this, copied into `tools/`:

```
python tools/asciify.py --check    ASCII, line width, table alignment
python tools/asciify.py --fix      transliterate and rewrap in place
python tools/rfcfmt.py  --fix P    render a markdown-ish draft as RFC text
```

`rfcfmt.py` is a one-way converter, not a round-trip formatter - it refuses a
file already in RFC form. Author new documents in RFC form directly. Run
`asciify.py --check` before treating any document as finished.

Record facts, not the journey. One fact has one home; everything else points at
it rather than restating it. Correct a document in place instead of appending a
correction.

## Working rules

- **Clean-room discipline.** Findings get written as behavioral specs -
  descriptor layouts, IOCTL codes, protocol/timing, config file format. Do not
  paste decompiler output into replacement-driver source.
- **State evidence, not guesses.** Back a claim with an address, a function
  name, a string, or an import. Mark inference as inference.
- **Prefer the two ends first.** The USB report descriptor and the IOCTL surface
  between `wishd201.exe` and `wishk201.sys` define the contract the replacement
  must satisfy; they are worth pinning down before the middle of either binary.
- **32-bit -> 64-bit.** The originals are x86 WDM-era code. Pointer-width,
  alignment, and structure packing assumptions in the config format and IOCTL
  buffers will not carry over unchanged - call these out when found.
