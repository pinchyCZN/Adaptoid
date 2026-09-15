#!/usr/bin/env python3
"""
hidrd.py -- decode a USB HID report descriptor into readable item text.

Written for the Adaptoid project: wishk201.sys stores its report
descriptors as static byte arrays in .data, and this renders them as
annotated HID items plus a per-report bit layout summary.

Usage:
    python tools/hidrd.py --hex 05010902a101...
    python tools/hidrd.py --file dump.bin [--offset N] [--length N]
    python tools/hidrd.py --hex ... --split      split at top-level
                                                 application collections

Output is plain ASCII so it can be pasted straight into docs/.
"""

import argparse
import sys

MAIN, GLOBAL, LOCAL = 0, 1, 2

MAIN_TAGS = {8: "Input", 9: "Output", 10: "Collection",
             11: "Feature", 12: "End Collection"}
GLOBAL_TAGS = {0: "Usage Page", 1: "Logical Minimum", 2: "Logical Maximum",
               3: "Physical Minimum", 4: "Physical Maximum",
               5: "Unit Exponent", 6: "Unit", 7: "Report Size",
               8: "Report ID", 9: "Report Count", 10: "Push", 11: "Pop"}
LOCAL_TAGS = {0: "Usage", 1: "Usage Minimum", 2: "Usage Maximum",
              3: "Designator Index", 4: "Designator Minimum",
              5: "Designator Maximum", 7: "String Index",
              8: "String Minimum", 9: "String Maximum", 10: "Delimiter"}

PAGES = {0x01: "Generic Desktop", 0x02: "Simulation", 0x07: "Keyboard/Keypad",
         0x08: "LED", 0x09: "Button", 0x0C: "Consumer", 0x0D: "Digitizer"}

GD_USAGES = {0x01: "Pointer", 0x02: "Mouse", 0x04: "Joystick",
             0x05: "Game Pad", 0x06: "Keyboard", 0x07: "Keypad",
             0x30: "X", 0x31: "Y", 0x32: "Z", 0x33: "Rx", 0x34: "Ry",
             0x35: "Rz", 0x36: "Slider", 0x37: "Dial", 0x38: "Wheel",
             0x39: "Hat switch"}

COLLECTIONS = {0x00: "Physical", 0x01: "Application", 0x02: "Logical",
               0x03: "Report", 0x04: "Named Array"}

SIZE_MAP = {0: 0, 1: 1, 2: 2, 3: 4}


def signed(val, nbytes):
    if nbytes and val >= (1 << (nbytes * 8 - 1)):
        val -= 1 << (nbytes * 8)
    return val


def io_flags(v):
    names = []
    names.append("Constant" if v & 1 else "Data")
    names.append("Variable" if v & 2 else "Array")
    names.append("Relative" if v & 4 else "Absolute")
    if v & 8:
        names.append("Wrap")
    if v & 16:
        names.append("Non-Linear")
    if v & 32:
        names.append("No Preferred")
    if v & 64:
        names.append("Null State")
    if v & 256:
        names.append("Buffered Bytes")
    return ",".join(names)


def items(data):
    """Yield (offset, raw_bytes, itype, tag, value, nbytes)."""
    i = 0
    while i < len(data):
        b = data[i]
        if b == 0xFE:               # long item
            if i + 2 >= len(data):
                return
            dsize = data[i + 1]
            yield (i, data[i:i + 3 + dsize], None, None, None, dsize)
            i += 3 + dsize
            continue
        size = SIZE_MAP[b & 3]
        itype = (b >> 2) & 3
        tag = (b >> 4) & 0xF
        if i + 1 + size > len(data):
            return
        val = 0
        for k in range(size):
            val |= data[i + 1 + k] << (8 * k)
        yield (i, data[i:i + 1 + size], itype, tag, val, size)
        i += 1 + size


def usage_name(page, uid):
    if page == 0x01:
        return GD_USAGES.get(uid)
    if page == 0x09:
        return "Button %d" % uid if uid else "No button"
    if page == 0x07:
        return "Key 0x%02X" % uid
    if page == 0x08:
        return {1: "Num Lock", 2: "Caps Lock", 3: "Scroll Lock",
                4: "Compose", 5: "Kana"}.get(uid)
    return None


def decode(data, base=0, out=sys.stdout):
    """Render items; return a list of report summaries."""
    depth = 0
    page = None
    rsize = rcount = None
    report_id = None
    reports = {}          # (kind, id) -> total bits
    for off, raw, itype, tag, val, nb in items(data):
        if itype is None:
            print("%04X  long item (%d bytes)" % (base + off, nb), file=out)
            continue
        hexs = " ".join("%02X" % c for c in raw)
        pad = "  " * depth
        note = ""
        if itype == MAIN:
            name = MAIN_TAGS.get(tag, "Main tag %d" % tag)
            if tag == 10:
                note = "(%s)" % COLLECTIONS.get(val, "0x%02X" % val)
            elif tag in (8, 9, 11):
                note = "(%s)" % io_flags(val)
                bits = (rsize or 0) * (rcount or 0)
                kind = {8: "Input", 9: "Output", 11: "Feature"}[tag]
                reports[(kind, report_id)] = \
                    reports.get((kind, report_id), 0) + bits
            print("%04X  %-14s %s%s %s" %
                  (base + off, hexs, pad, name, note), file=out)
            if tag == 10:
                depth += 1
            elif tag == 12:
                depth = max(0, depth - 1)
            continue

        if itype == GLOBAL:
            name = GLOBAL_TAGS.get(tag, "Global tag %d" % tag)
            if tag == 0:
                page = val
                note = "(%s)" % PAGES.get(val, "0x%04X" % val)
            elif tag in (1, 2, 3, 4):
                note = "(%d)" % signed(val, nb)
            elif tag == 7:
                rsize = val
                note = "(%d bits)" % val
            elif tag == 8:
                report_id = val
                note = "(%d)" % val
            elif tag == 9:
                rcount = val
                note = "(%d)" % val
            else:
                note = "(%d)" % val
        else:
            name = LOCAL_TAGS.get(tag, "Local tag %d" % tag)
            u = usage_name(page, val)
            note = "(%s)" % (u if u else "0x%02X" % val)
        print("%04X  %-14s %s%s %s" %
              (base + off, hexs, pad, name, note), file=out)
    return reports


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hex")
    ap.add_argument("--file")
    ap.add_argument("--offset", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--length", type=lambda x: int(x, 0))
    ap.add_argument("--base", type=lambda x: int(x, 0), default=0,
                    help="address printed for the first byte")
    args = ap.parse_args()

    if args.hex:
        h = "".join(args.hex.split())
        data = bytes.fromhex(h)
    elif args.file:
        data = open(args.file, "rb").read()
    else:
        ap.error("need --hex or --file")
    data = data[args.offset:]
    if args.length:
        data = data[:args.length]

    reports = decode(data, base=args.base)
    print()
    print("Report summary")
    for (kind, rid), bits in sorted(reports.items(),
                                    key=lambda kv: (str(kv[0][1]), kv[0][0])):
        print("   %-8s Report ID %-4s %3d bits = %5.1f bytes (+1 ID)" %
              (kind, rid if rid is not None else "-", bits, bits / 8.0))


if __name__ == "__main__":
    main()
