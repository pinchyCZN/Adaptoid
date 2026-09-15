Adaptoid Reverse Engineering                                 Document Index


                          DOCUMENTATION INDEX


Abstract

   This directory holds every written finding for the Adaptoid reverse
   engineering project: the USB and HID contract exposed by the driver,
   the IOCTL surface between the configurator and the driver, the on-disk
   configuration and scripting format, and the notes needed to rebuild an
   equivalent driver for 64-bit Windows.

   A finding that exists only in a chat transcript is lost. Anything
   learned about a structure, a subsystem, a file format, or an algorithm
   is written here in the same session it was found.

Table of Contents

   1.  Format
   2.  Planned Documents
   3.  Conventions

1.  Format

   Documents are plain ASCII text laid out like an old RFC. Body text is
   indented three spaces and wrapped at 80 columns; tables may run to 120
   including their indent. No markdown syntax appears in a .txt file.

   Two checkers live in tools/:

   +----------------------------------+------------------------------------+
   | Command                          | Purpose                            |
   +==================================+====================================+
   | python tools/asciify.py --check  | ASCII, width, table alignment      |
   | python tools/asciify.py --fix    | transliterate and rewrap in place  |
   | python tools/rfcfmt.py --fix P   | render a draft as RFC text         |
   +----------------------------------+------------------------------------+

   Run asciify.py --check before treating any document as finished.

2.  Planned Documents

   Written so far: hid-descriptor.txt, ioctl-surface.txt and
   script-bytecode.txt. The rest are listed so findings land in a
   predictable place rather than accumulating in one file.

   +-----------------------+---------------------------------------------+
   | File                  | Contents                                    |
   +=======================+=============================================+
   | hid-descriptor.txt    | The composite HID report descriptor: the    |
   |                       | keyboard, mouse, and gamepad collections    |
   |                       | and the report layout for each.             |
   +-----------------------+---------------------------------------------+
   | ioctl-surface.txt     | Every IOCTL code shared between             |
   |                       | wishd201.exe and wishk201.sys, with its     |
   |                       | input and output buffer layout.             |
   +-----------------------+---------------------------------------------+
   | usb-protocol.txt      | Device endpoints, the N64 serial protocol,  |
   |                       | polling cadence, and pak handling.          |
   +-----------------------+---------------------------------------------+
   | script-bytecode.txt   | DONE. The flex/bison compiler and the       |
   |                       | 32-bit opcode format it emits.              |
   +-----------------------+---------------------------------------------+
   | driver-structure.txt  | Driver object layout, dispatch table, and   |
   |                       | device extension.                           |
   +-----------------------+---------------------------------------------+
   | port-notes.txt        | What must change for 64-bit: pointer width, |
   |                       | packing, WDM to KMDF or UMDF2.              |
   +-----------------------+---------------------------------------------+

3.  Conventions

   Fact and inference are kept apart. A statement backed by an address, a
   string, an import, or a decompiled instruction is a fact, and the
   evidence is cited inline. Anything else is marked as inference.

   Addresses are written with their binary, because two binaries are open
   at once and the ranges do not overlap:

       wishk201.sys    00010000 - 0001adff   (driver)
       wishd201.exe    00400000 - 004a1ba4   (configurator)

   One fact has one home. Other documents point at that home rather than
   restating it. Documents are corrected in place, never by appending a
   note that contradicts an earlier section.
