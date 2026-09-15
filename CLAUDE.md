# ADAPTOID

Reverse engineering the Adaptoid N64-to-USB controller driver + configurator, with the
goal of writing a clean-room replacement that runs on modern 64-bit Windows.

## What the original does (and why it matters)

The Adaptoid is unusual among joy-to-key adapters: remapping happens **in the driver**,
not in a user-mode hook. The driver registers via `HidRegisterMinidriver` and reports a
**composite HID descriptor** (keyboard + mouse + gamepad in one device), so any part of the
N64 controller can be presented to the OS as real key or mouse input. Every application
sees genuine HID input, which is why it works in games and software that ignore user-level
key injection.

Preserving this property — composite HID, remapping below the application layer — is the
core requirement of the replacement driver.

## Artifacts

| Path | What it is |
| --- | --- |
| `decomp/wishk201.sys` | Kernel driver. x86 32-bit, image base `0x10000`, 163 functions. HID minidriver over USBD. |
| `decomp/wishd201.exe` | Configurator / scripting UI. x86 32-bit, image base `0x400000`, 610 functions. |
| `decomp/adaptoid.gpr` + `adaptoid.rep/` | Ghidra project holding both binaries. |

Both binaries are dated 2001. They are **primary evidence — never modify, move, rename, or
rebuild them.** All analysis output goes into new files or into Ghidra annotations.

## Ghidra access

The Ghidra project `adaptoid` is live over MCP (TCP `127.0.0.1:8089`) with **both programs
open at once**.

**Always pass `program=` explicitly on every Ghidra MCP call.**

```
program="wishk201.sys"   # the driver
program="wishd201.exe"   # the configurator
```

There is no prefix syntax and no server-side enforcement for this — the `program` parameter
defaults to the empty string, which silently targets whichever program is *active*
(currently `wishk201.sys`). A call that omits `program` will appear to succeed while
reading or writing the wrong binary. Treat an omitted `program` as a bug, not a shortcut.

Annotating Ghidra (renaming functions/variables, setting prototypes, adding comments and
plate comments) **is** the deliverable of analysis work — do it as you go rather than
keeping findings only in chat. Name things after what they do, and record the evidence for
a name in a comment.

## Git — read only

The agent has **read-only** access to git. Allowed: `git log`, `git show`, `git diff`,
`git status`, `git blame`, and reading files.

Do **not** run: `commit`, `add`, `push`, `pull`, `fetch`, `merge`, `rebase`, `reset`,
`checkout`/`switch`, `branch`, `tag`, `stash`, `clean`, or anything that writes to `.git/`.
The user drives all git operations.

Creating and editing files in the working tree is fine — that is not a git operation.
Note the repo currently has **no commits**; `git log` will fail until the user makes one.

## Working rules

- **Clean-room discipline.** Findings get written as behavioral specs — descriptor layouts,
  IOCTL codes, protocol/timing, config file format. Do not paste decompiler output into
  replacement-driver source.
- **State evidence, not guesses.** Back a claim with an address, a function name, a string,
  or an import. Mark inference as inference.
- **Prefer the two ends first.** The USB report descriptor and the IOCTL surface between
  `wishd201.exe` and `wishk201.sys` define the contract the replacement must satisfy; they
  are worth pinning down before the middle of either binary.
- **32-bit → 64-bit.** The originals are x86 WDM-era code. Pointer-width, alignment, and
  structure packing assumptions in the config format and IOCTL buffers will not carry over
  unchanged — call these out when found.
