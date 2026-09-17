"""kdc.py -- one-shot client for the kd daemon in tools/kdd.py.

    python tools/kdc.py --status
    python tools/kdc.py "lm m wish*" "dd wishk201+0x9d94 L1"
    python tools/kdc.py --file B:/kd-globals.kd
    python tools/kdc.py --go

Connects, sends one request, prints the debugger output, disconnects. The
daemon holds the only kd session, so running this a hundred times still
opens the VM's pipe exactly once.

COMMANDS ARE SENT AS ONE BATCH. The daemon breaks in once, runs all of
them, and resumes, so the guest freezes for the length of the batch rather
than once per command. Pass --hold only when another batch follows
immediately; a held target is a stopped VM.

`q`, `qd` and `qq` are refused by the daemon rather than passed through.
Quitting while the target is halted leaves the guest hung with nothing to
release it -- see the header of kdd.py.
"""

import argparse
import json
import socket
import sys

DEFAULT_PORT = 8097


def request(obj, port=DEFAULT_PORT, host="127.0.0.1", timeout=120.0):
    """Send one JSON request and return the decoded reply."""
    sock = socket.create_connection((host, port), timeout=10.0)
    sock.settimeout(timeout)
    try:
        sock.sendall((json.dumps(obj) + "\n").encode("utf-8"))
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = sock.recv(65536)
            if not chunk:
                break
            buf += chunk
    finally:
        sock.close()
    if not buf.strip():
        return {"ok": False, "error": "no reply from daemon"}
    return json.loads(buf.decode("utf-8"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("commands", nargs="*", help="debugger commands")
    ap.add_argument("--file", default="",
                    help="read commands from a file, one per line")
    ap.add_argument("--out", default="", help="write output here too")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--hold", action="store_true",
                    help="leave the target halted (another batch follows)")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--go", action="store_true", help="resume the target")
    ap.add_argument("--break", dest="brk", action="store_true")
    args = ap.parse_args()

    if args.status:
        req = {"op": "status"}
    elif args.go:
        req = {"op": "go"}
    elif args.brk:
        req = {"op": "break", "timeout": args.timeout}
    else:
        lines = list(args.commands)
        if args.file:
            with open(args.file, "r") as handle:
                lines += [ln.rstrip("\r\n") for ln in handle]
        if not lines:
            ap.error("nothing to send; give commands, --file, or --status")
        req = {"op": "batch", "lines": lines, "timeout": args.timeout,
               "hold": args.hold}

    try:
        reply = request(req, args.port)
    except socket.error as exc:
        sys.stderr.write(
            "Cannot reach the kd daemon on port %d: %s\n"
            "Start it with:  python tools/kdd.py --pipe adaptoid-dbg\n"
            % (args.port, exc))
        return 2

    if not reply.get("ok"):
        sys.stderr.write("error: %s\n" % reply.get("error", "unknown"))
        return 1

    if req["op"] == "status":
        for key in ("alive", "halted", "pid", "pipe", "kd", "target",
                    "break_method"):
            if key in reply:
                print("%-13s %s" % (key, reply[key]))
        return 0

    text = reply.get("text", "")
    print(text)
    if reply.get("halted"):
        sys.stderr.write("\n[target is HALTED - the VM is frozen. "
                         "Release it with: python tools/kdc.py --go]\n")
    if args.out:
        with open(args.out, "w") as handle:
            handle.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
