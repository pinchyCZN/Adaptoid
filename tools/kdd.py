"""kdd.py -- a daemon that owns one long-lived kd.exe session.

    python tools/kdd.py --pipe adaptoid-dbg

Start it once, in its own console window, and leave it. It connects kd.exe
to the VM's named pipe and NEVER disconnects. Clients (tools/kdc.py) talk to
it over a local TCP socket and get debugger output back, so the pipe is
opened exactly once per VM boot no matter how many queries are run.

WHY A DAEMON AND NOT ONE kd PER QUERY. Two separate failures, both of which
leave the guest hung and unrecoverable without a VM restart:

  1.  QUITTING WHILE THE TARGET IS HALTED LEAVES IT HALTED. `qd` detaches the
      debugger but does not resume a broken-in target. The guest sits at the
      int 3 forever with nothing to release it, which looks exactly like a
      frozen VM. A script that breaks in with -b and ends with `qd` hangs the
      guest EVERY time. Resume with `g` first, and only then let go.

  2.  VirtualBox's pipe server does not always accept a second connection
      after a client disconnects, so the pipe appears wedged even with no kd
      running. Never disconnecting sidesteps this entirely.

THE TARGET RUNS BY DEFAULT. A batch breaks in, runs its commands, and
resumes, so the guest is frozen only for the few milliseconds the commands
take. Nothing here ever sends `q` or `qd`.

See docs/kernel-debugging.txt for the VM and guest setup this expects.
"""

import argparse
import json
import os
import queue
import re
import signal
import socket
import socketserver
import subprocess
import sys
import threading
import time

# kd prints this and waits. Finding it is how a command is known to be done.
PROMPT = re.compile(rb"\r?\n?(?:\d+:\s*(?:kd|kernel)>|kd>)\s*$")

DEFAULT_TIMEOUT = 30.0
DEFAULT_PORT = 8097

KD_ROOTS = [
    r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x86",
    r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64",
    r"C:\Program Files\Windows Kits\10\Debuggers\x64",
    r"E:\DEV\WinDDK\Debuggers",
]


def find_kd():
    """Newest kd.exe on the machine, preferring the Windows 10 SDK."""
    for root in KD_ROOTS:
        path = os.path.join(root, "kd.exe")
        if os.path.exists(path):
            return path
    return ""


class Session(object):
    """The one kd process, and everything that serialises access to it."""

    def __init__(self, kd_path, pipe, resets=0, reconnect=True):
        self.kd_path = kd_path
        self.pipe = pipe
        self.proc = None
        self.out = queue.Queue()
        self.lock = threading.Lock()
        self.halted = False
        self.break_method = "none"

        prefix = "\\\\.\\pipe\\"
        self.target = "com:pipe,port=%s%s,resets=%d%s" % (
            prefix, pipe, resets, ",reconnect" if reconnect else "")

    # -- process ------------------------------------------------------

    def start(self):
        """Launch kd and swallow its banner.

        CREATE_NEW_PROCESS_GROUP is not cosmetic: it makes kd its own group
        leader, which is what allows a console control event to be
        delivered to kd alone rather than to this daemon as well.
        """
        flags = 0
        if os.name == "nt":
            flags = subprocess.CREATE_NEW_PROCESS_GROUP

        self.proc = subprocess.Popen(
            [self.kd_path, "-k", self.target],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=0, creationflags=flags)

        threading.Thread(target=self._pump, daemon=True).start()

        banner = self._drain(15.0)
        # kd prompts once it has the connection; if it did, we are halted.
        self.halted = bool(PROMPT.search(banner.encode("utf-8", "replace")))
        return banner

    def _pump(self):
        """Move kd's output into a queue until the process dies."""
        while True:
            chunk = self.proc.stdout.read(1)
            if not chunk:
                break
            self.out.put(chunk)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    # -- the wire -----------------------------------------------------

    def _drain(self, timeout):
        """Collect output until kd prompts again, or time runs out.

        RETURNS WHAT IT HAS EITHER WAY. A timeout usually means the target
        is running rather than that anything is wrong -- `g` never prompts
        until something breaks in -- so partial output is the answer.
        """
        deadline = time.time() + timeout
        buf = bytearray()
        while time.time() < deadline:
            try:
                buf += self.out.get(timeout=0.05)
            except queue.Empty:
                pass
            if PROMPT.search(bytes(buf)):
                break
        return buf.decode("utf-8", "replace")

    def _write(self, text):
        self.proc.stdin.write(text.encode("ascii", "replace"))
        self.proc.stdin.flush()

    def _flush(self):
        """Take everything kd has already said, and return it.

        A PROMPT LEFT IN THE QUEUE SHIFTS EVERY LATER RESULT BY ONE COMMAND.
        _drain stops at the first prompt it sees, so a prompt still buffered
        from the previous command ends the next drain instantly and that
        command's real output is not read until the command after it. The
        session still works, which is what makes this hard to notice: every
        answer looks plausible and belongs to the previous question.

        The backlog is RETURNED, NOT DISCARDED, because it is exactly where
        log-and-continue breakpoint output lands - those .printf lines are
        written while no command is outstanding, so throwing the backlog
        away would silently drop the trace this tool exists to collect.
        """
        buf = bytearray()
        while True:
            try:
                buf += self.out.get_nowait()
            except queue.Empty:
                break
        return buf.decode("utf-8", "replace")

    def send(self, command, timeout=DEFAULT_TIMEOUT):
        """Write one command and return everything kd says back."""
        backlog = self._flush()
        self._write(command + "\r\n")
        text = self._drain(timeout)
        self.halted = bool(PROMPT.search(text.encode("utf-8", "replace")))
        return backlog + text

    # -- halt and resume ----------------------------------------------

    def break_in(self, timeout=15.0):
        """Halt the target and get a prompt.

        A CONSOLE CONTROL EVENT IS THE ONLY THING THAT WORKS. Writing 0x03
        to kd's stdin does nothing when stdin is a pipe: Ctrl+C is a console
        event, not a byte in the input stream, so it is read as data and
        discarded. The break request must be delivered out of band with
        GenerateConsoleCtrlEvent, which is what os.kill does on Windows.
        """
        if self.halted:
            return ""

        # Anything already buffered is breakpoint output, and it would
        # otherwise end this drain before the break has even happened.
        text = self._flush()
        if os.name == "nt":
            try:
                os.kill(self.proc.pid, signal.CTRL_BREAK_EVENT)
                text = self._drain(timeout)
                if PROMPT.search(text.encode("utf-8", "replace")):
                    self.break_method = "CTRL_BREAK_EVENT"
                    self.halted = True
                    return text
            except Exception as exc:
                text += "\n[CTRL_BREAK_EVENT failed: %s]\n" % exc

        # Fallback for a kd build that only honours Ctrl+C on the stream.
        self._write("\x03")
        text += self._drain(timeout)
        if PROMPT.search(text.encode("utf-8", "replace")):
            self.break_method = "stdin 0x03"
            self.halted = True
        return text

    def resume(self):
        """Let the target run. Never leave the guest halted."""
        if not self.halted:
            return ""
        self._write("g\r\n")
        self.halted = False
        # `g` does not prompt again until something breaks in, so take
        # whatever is already buffered and return.
        return self._drain(1.0)

    # -- the operation clients actually use ---------------------------

    def batch(self, lines, timeout=DEFAULT_TIMEOUT, hold=False):
        """Break in once, run every command, resume once.

        THE FREEZE IS BOUNDED BY THIS METHOD. Breaking in per command would
        stop and start the guest N times; doing it once keeps the halt to
        the length of the batch. hold=True leaves the target halted, which
        is only correct when the caller is about to run another batch.
        """
        with self.lock:
            if not self.alive():
                return {"ok": False, "error": "kd is not running"}

            out = [self.break_in()]
            for line in lines:
                stripped = line.strip()
                if not stripped or stripped.startswith(".echo "):
                    out.append(stripped[6:] if stripped else "")
                    continue
                if stripped.lower() in ("q", "qd", "qq"):
                    out.append("[refused: %s would abandon the session]"
                               % stripped)
                    continue
                out.append(self.send(stripped, timeout))
            if not hold:
                self.resume()
            return {"ok": True, "halted": self.halted,
                    "break_method": self.break_method,
                    "text": "\n".join(out)}


class Handler(socketserver.StreamRequestHandler):
    """One JSON object per line in, one JSON object per line out."""

    def handle(self):
        session = self.server.session
        for raw in self.rfile:
            try:
                req = json.loads(raw.decode("utf-8"))
            except ValueError as exc:
                self._reply({"ok": False, "error": "bad JSON: %s" % exc})
                continue

            op = req.get("op", "batch")
            timeout = float(req.get("timeout", DEFAULT_TIMEOUT))

            if op == "status":
                self._reply({
                    "ok": True, "alive": session.alive(),
                    "halted": session.halted, "pipe": session.pipe,
                    "target": session.target, "kd": session.kd_path,
                    "break_method": session.break_method,
                    "pid": session.proc.pid if session.proc else None,
                })
            elif op == "break":
                with session.lock:
                    text = session.break_in(timeout)
                self._reply({"ok": True, "halted": session.halted,
                             "break_method": session.break_method,
                             "text": text})
            elif op == "go":
                with session.lock:
                    text = session.resume()
                self._reply({"ok": True, "halted": session.halted,
                             "text": text})
            elif op == "raw":
                with session.lock:
                    text = session.send(req.get("text", ""), timeout)
                self._reply({"ok": True, "halted": session.halted,
                             "text": text})
            elif op == "batch":
                lines = req.get("lines") or [req.get("text", "")]
                self._reply(session.batch(lines, timeout,
                                          bool(req.get("hold"))))
            else:
                self._reply({"ok": False, "error": "unknown op %r" % op})

    def _reply(self, obj):
        self.wfile.write((json.dumps(obj) + "\n").encode("utf-8"))
        self.wfile.flush()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pipe", default="adaptoid-dbg",
                    help="pipe name, without the leading path")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--kd", default="", help="full path to kd.exe")
    ap.add_argument("--resets", type=int, default=0)
    args = ap.parse_args()

    kd_path = args.kd or find_kd()
    if not kd_path:
        sys.stderr.write("No kd.exe found. Pass --kd.\n")
        return 2

    session = Session(kd_path, args.pipe, args.resets)
    print("kd     : %s" % kd_path)
    print("target : %s" % session.target)
    print("Connecting. Boot or resume the VM if this waits.")
    banner = session.start()
    print(banner)

    if not session.alive():
        sys.stderr.write("kd exited during connect.\n")
        return 1

    # Do not sit on the guest while idle.
    if session.halted:
        session.resume()
        print("[target resumed; it runs until a batch breaks in]")

    server = Server(("127.0.0.1", args.port), Handler)
    server.session = session
    print("Listening on 127.0.0.1:%d. Ctrl+C to stop." % args.port)
    print("Leave this window open for the life of the VM.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nResuming target before exit...")
        try:
            session.resume()
        except Exception:
            pass
        print("Guest left running. kd will be terminated.")
        try:
            session.proc.terminate()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
