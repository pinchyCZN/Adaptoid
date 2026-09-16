"""kdmcp.py -- an MCP server that drives a live kernel-debug session.

Wraps kd.exe talking to a VirtualBox guest over a named pipe, and exposes
the session as MCP tools so an agent can break in, set breakpoints, read
memory and walk the stack.

    python tools/kdmcp.py --pipe adaptoid-dbg

It is a KERNEL debugger, which is the whole point: the Ghidra bridge's own
debugger tools proxy to a separate server and attach to a user-mode process
by name or PID, so they cannot reach a driver in a VM at all.

THE ADDRESS MAP IS THE USEFUL PART. wishk201.sys has no symbols and loads
wherever the kernel puts it, so a live address means nothing on its own.
kd_sync() reads the load base out of `lm`, and kd_to_live()/kd_to_static()
convert between that base and the Ghidra image base -- which is what lets a
breakpoint be set on a function named in the Ghidra database.

Requires the `mcp` package, and kd.exe from the Windows 10 SDK; the WDK 7.1
one works too but is older. See docs/kernel-debugging.txt for the VM and
guest setup this expects to already be done.
"""

import argparse
import os
import queue
import re
import subprocess
import sys
import threading
import time

from mcp.server.fastmcp import FastMCP

mcp = FastMCP("kd")

# kd prints this and waits. Finding it is how a command is known to be done.
PROMPT = re.compile(rb"\r?\n?(?:\d+:\s*(?:kd|kernel)>|kd>)\s*$")

# How long to wait for a prompt before giving up on a command.
DEFAULT_TIMEOUT = 30.0

# Everything the session owns. One kd, one pipe, one map.
_kd = None
_out = queue.Queue()
_pending = bytearray()
_modules = {}          # lowercase module name -> live base address
_static = {}           # lowercase module name -> Ghidra image base


def _pump(stream, sink):
    """Move kd's output into a queue. kd never closes stdout while it
    lives, so this runs until the process does."""
    while True:
        chunk = stream.read(1)
        if not chunk:
            break
        sink.put(chunk)


def _drain(timeout):
    """Collect output until kd prompts again, or time runs out.

    RETURNS WHAT IT HAS EITHER WAY. A timeout here usually means the target
    is running rather than that anything is wrong - `g` never prompts until
    something breaks in - so the partial output is the answer.
    """
    global _pending
    deadline = time.time() + timeout
    buf = bytearray(_pending)
    _pending = bytearray()

    while time.time() < deadline:
        try:
            buf += _out.get(timeout=0.05)
        except queue.Empty:
            if PROMPT.search(bytes(buf)):
                break
            continue
        if PROMPT.search(bytes(buf)):
            break

    text = buf.decode("utf-8", "replace")
    return text


def _send(command, timeout=DEFAULT_TIMEOUT):
    """Write one command to kd and return everything it says back."""
    if _kd is None or _kd.poll() is not None:
        return "kd is not running. Call kd_start first."
    _kd.stdin.write((command + "\r\n").encode())
    _kd.stdin.flush()
    return _drain(timeout)


@mcp.tool()
def kd_start(pipe: str = "adaptoid-dbg", kd_path: str = "",
             resets: int = 0, reconnect: bool = True) -> str:
    """Launch kd.exe against the VM's named pipe and connect.

    The VM must already be configured and booted with the debug stub on;
    see docs/kernel-debugging.txt. THIS DOES NOT BREAK IN - the target
    keeps running. Call kd_break() when you want a prompt.

    Args:
        pipe: the pipe name, without the \\\\.\\pipe\\ prefix.
        kd_path: full path to kd.exe. Found automatically if empty.
        resets: com: resets= parameter. 0 is right for a named pipe.
        reconnect: reopen the pipe if the VM restarts.
    """
    global _kd
    if _kd is not None and _kd.poll() is None:
        return "kd is already running. Call kd_stop first."

    exe = kd_path or _find_kd()
    if not exe:
        return ("No kd.exe found. Pass kd_path, or install the Windows 10 "
                "SDK Debugging Tools.")

    target = "com:pipe,port=\\\\.\\pipe\\%s,resets=%d%s" % (
        pipe, resets, ",reconnect" if reconnect else "")

    _kd = subprocess.Popen(
        [exe, "-k", target],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0)

    threading.Thread(target=_pump, args=(_kd.stdout, _out),
                     daemon=True).start()

    banner = _drain(10.0)
    return "kd: %s\ntarget: %s\n\n%s" % (exe, target, banner)


def _find_kd():
    """Newest kd.exe on the machine, preferring the Windows 10 SDK."""
    roots = [
        r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x86",
        r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64",
        r"C:\Program Files\Windows Kits\10\Debuggers\x64",
        r"E:\DEV\WinDDK\Debuggers",
    ]
    for r in roots:
        p = os.path.join(r, "kd.exe")
        if os.path.exists(p):
            return p
    return ""


@mcp.tool()
def kd_stop() -> str:
    """Detach and shut kd down. The guest keeps running."""
    global _kd
    if _kd is None or _kd.poll() is not None:
        _kd = None
        return "kd was not running."
    try:
        _kd.stdin.write(b"qd\r\n")     # quit and DETACH, leaving the guest up
        _kd.stdin.flush()
        time.sleep(0.5)
    except Exception:
        pass
    _kd.terminate()
    _kd = None
    return "kd stopped, guest detached."


@mcp.tool()
def kd_break(timeout: float = 15.0) -> str:
    """Break into the target and get a prompt.

    kd takes Ctrl-C on its stdin as the break request. Until this returns,
    the guest is running and no other command will answer.
    """
    if _kd is None or _kd.poll() is not None:
        return "kd is not running."
    _kd.stdin.write(b"\x03")
    _kd.stdin.flush()
    return _drain(timeout)


@mcp.tool()
def kd_cmd(command: str, timeout: float = DEFAULT_TIMEOUT) -> str:
    """Run any debugger command and return its output.

    THE ESCAPE HATCH, and the tool to reach for when nothing else fits -
    kernel work lives in extension commands (!process, !irp, !devobj,
    !devstack, !drvobj) that no wrapper can usefully enumerate.

    The target must be broken in; see kd_break.
    """
    return _send(command, timeout)


@mcp.tool()
def kd_go(timeout: float = 2.0) -> str:
    """Let the target run.

    Returns almost immediately with whatever kd printed. The target is
    running after this; use kd_break or wait for a breakpoint.
    """
    return _send("g", timeout)


@mcp.tool()
def kd_registers() -> str:
    """The register file at the current break."""
    return _send("r")


@mcp.tool()
def kd_stack(depth: int = 20) -> str:
    """Call stack of the current thread.

    EXPECT IT TO BE POOR INSIDE THE DRIVER. wishk201.sys has no symbols
    and 2001 code is frame-pointer-light, so kd guesses. Frames inside the
    kernel are accurate; frames inside the driver need kd_to_static to mean
    anything.
    """
    return _send("kb %d" % depth)


@mcp.tool()
def kd_modules(filter: str = "") -> str:
    """Loaded modules and their base addresses.

    Args:
        filter: a module name to narrow to, e.g. "wishk201".
    """
    return _send("lm m %s" % filter if filter else "lm")


@mcp.tool()
def kd_sync(module: str = "wishk201", static_base: int = 0x10000) -> str:
    """Learn where a module is loaded, so addresses can be translated.

    CALL THIS BEFORE ANY BREAKPOINT ON DRIVER CODE. Without it a Ghidra
    address is meaningless to the live target: the database is based at
    0x10000 and the kernel loads the driver wherever it likes.

    Args:
        module: module name without extension.
        static_base: the image base in the Ghidra database. 0x10000 for
            wishk201.sys, 0x400000 for wishd201.exe, 0x10000000 for
            wishh201.dll.
    """
    text = _send("lm m %s" % module)
    # "start    end        module name" - the first hex column is the base.
    m = re.search(r"^\s*([0-9a-fA-F`]{8,})\s+([0-9a-fA-F`]{8,})\s+(\S+)",
                  text, re.M)
    if not m:
        return ("Module %s not found in the target.\n\n%s\n"
                "If the driver is not loaded yet, break at load time with:\n"
                "  sxe ld %s" % (module, text, module))
    base = int(m.group(1).replace("`", ""), 16)
    name = m.group(3).lower()
    _modules[name] = base
    _static[name] = static_base
    return ("%s: live base 0x%X, Ghidra base 0x%X, slide 0x%X\n"
            "kd_to_live and kd_to_static now work for it."
            % (name, base, static_base, base - static_base))


def _slide(module):
    key = module.lower()
    if key not in _modules:
        return None
    return _modules[key] - _static[key]


@mcp.tool()
def kd_to_live(static_address: str, module: str = "wishk201") -> str:
    """Convert a Ghidra address to the live one. Needs kd_sync first.

    Args:
        static_address: hex, with or without 0x, as Ghidra shows it.
    """
    s = _slide(module)
    if s is None:
        return "Run kd_sync('%s') first." % module
    a = int(str(static_address).replace("0x", "").replace("`", ""), 16)
    return "0x%X" % (a + s)


@mcp.tool()
def kd_to_static(live_address: str, module: str = "wishk201") -> str:
    """Convert a live address back to its Ghidra address.

    This is what turns a crash address or a stack frame into something the
    annotated database can be searched for.
    """
    s = _slide(module)
    if s is None:
        return "Run kd_sync('%s') first." % module
    a = int(str(live_address).replace("0x", "").replace("`", ""), 16)
    return "0x%08X" % (a - s)


@mcp.tool()
def kd_breakpoint(static_address: str, module: str = "wishk201",
                  command: str = "") -> str:
    """Set a breakpoint on a Ghidra address inside a driver.

    Args:
        static_address: the address as Ghidra shows it, e.g. 00015150.
        command: optional debugger command to run when it hits, e.g.
            "r;kb;g" to log registers and a stack and carry on.
    """
    s = _slide(module)
    if s is None:
        return "Run kd_sync('%s') first." % module
    a = int(str(static_address).replace("0x", "").replace("`", ""), 16) + s
    if command:
        return _send('bp 0x%X "%s"' % (a, command.replace('"', '\\"')))
    return _send("bp 0x%X" % a)


@mcp.tool()
def kd_breakpoints() -> str:
    """List breakpoints."""
    return _send("bl")


@mcp.tool()
def kd_clear(index: str = "*") -> str:
    """Clear a breakpoint by index, or all of them with "*"."""
    return _send("bc %s" % index)


@mcp.tool()
def kd_read(address: str, count: int = 32, width: str = "b") -> str:
    """Read memory.

    Args:
        address: live address, hex.
        width: b, w, d or q for byte, word, dword, qword.
    """
    return _send("d%s %s L%X" % (width, address, count))


@mcp.tool()
def kd_disasm(address: str, count: int = 16) -> str:
    """Disassemble at a live address."""
    return _send("u %s L%X" % (address, count))


@mcp.tool()
def kd_devices(driver: str = "wishk201") -> str:
    """The driver object and its device objects.

    !drvobj with a detail level of 7 lists the dispatch table, which is
    the fastest way to confirm the triage wrappers took effect - the entry
    points should be the driver's own, not hidclass's.
    """
    return _send("!drvobj \\Driver\\%s 7" % driver, 20.0)


@mcp.tool()
def kd_status() -> str:
    """Whether kd is running, and what has been synced."""
    if _kd is None or _kd.poll() is not None:
        return "kd is not running."
    lines = ["kd is running (pid %d)" % _kd.pid]
    for name, base in _modules.items():
        lines.append("  %s live 0x%X ghidra 0x%X slide 0x%X"
                     % (name, base, _static[name], base - _static[name]))
    if not _modules:
        lines.append("  no module synced; call kd_sync")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pipe", default="",
                    help="connect to this pipe at startup")
    ap.add_argument("--kd", default="", help="path to kd.exe")
    args = ap.parse_args()

    if args.pipe:
        sys.stderr.write(kd_start(args.pipe, args.kd) + "\n")
    mcp.run()


if __name__ == "__main__":
    main()
