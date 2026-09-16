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

   Note that replacement-architecture.txt is a DESIGN note rather than a
   reverse engineering note: it records decisions and reasoning for the
   port, not findings about the binaries.

   1.  Format
   2.  Planned Documents
   3.  Conventions

1.  Format

   Documents are plain ASCII text laid out like an old RFC. Body text is
   indented three spaces and wrapped at 80 columns; tables may run to 120
   including their indent. No markdown syntax appears in a .txt file.

   tools/ holds the checkers these documents are held to, the ones the
   replacement source is held to, and the driver-signing pair:

   +----------------------------------+------------------------------------+
   | Command                          | Purpose                            |
   +==================================+====================================+
   | python tools/asciify.py --check  | ASCII, width, table alignment      |
   | python tools/asciify.py --fix    | transliterate and rewrap in place  |
   | python tools/rfcfmt.py --fix P   | render a draft as RFC text         |
   | python tools/retab.py --check    | tabs for indent, spaces to align   |
   | python tools/originmap.py --check| every function maps to an original |
   | tools/mktestcert.cmd             | make the test certificate, ONCE    |
   | tools/signdriver.cmd             | sign a build for a test machine    |
   | python tools/kdmcp.py --pipe N   | MCP server driving a live kd       |
   +----------------------------------+------------------------------------+

   Run asciify.py --check before treating any document as finished. The two
   .cmd files are documented in src_drv/README.txt section 7.1; note that
   mktestcert.cmd writes a PRIVATE KEY, into the gitignored build tree.

2.  Planned Documents

   Every document below marked DONE is written. The one marked PLANNED is
   listed so findings land in a predictable place rather than accumulating in
   one file. The replacement source tree lives in ../src_drv and carries its
   own build note, src_drv/README.txt.

   +-----------------------+---------------------------------------------+
   | File                  | Contents                                    |
   +=======================+=============================================+
   | hid-descriptor.txt    | DONE. The composite HID report descriptor:  |
   |                       | the keyboard, mouse and gamepad             |
   |                       | collections, and the report layout for      |
   |                       | each.                                       |
   +-----------------------+---------------------------------------------+
   | ioctl-surface.txt     | DONE. Every IOCTL code shared between       |
   |                       | wishd201.exe and wishk201.sys, with its     |
   |                       | input and output buffer layout.             |
   +-----------------------+---------------------------------------------+
   | kernel-debugging.txt  | DONE. Breaking into wishk201.sys live in a  |
   |                       | Win7 VM: the VirtualBox serial pipe, the    |
   |                       | guest's boot flag, the host debugger, and   |
   |                       | driving the session from an agent.          |
   +-----------------------+---------------------------------------------+
   | command-block.txt     | DONE. The control device's OTHER surface:   |
   |                       | the 64-byte joybus command block the vendor |
   |                       | SDK's clients read and write, its two-pass  |
   |                       | execution, and the Rumble Pak the driver    |
   |                       | emulates rather than putting on the wire.   |
   +-----------------------+---------------------------------------------+
   | script-bytecode.txt   | DONE. The flex/bison compiler and the       |
   |                       | 32-bit opcode format it emits.              |
   +-----------------------+---------------------------------------------+
   | script-language.txt   | DONE. The scripting language itself: the    |
   |                       | token set, the grammar, and the native      |
   |                       | builtin library.                            |
   +-----------------------+---------------------------------------------+
   | known-defects.txt     | DONE. Defects found in the originals, what  |
   |                       | triggers each, and whether a user would     |
   |                       | notice.                                     |
   +-----------------------+---------------------------------------------+
   | usb-transport.txt     | DONE. Device endpoints, the N64 serial      |
   |                       | protocol, polling cadence, and pak          |
   |                       | handling.                                   |
   +-----------------------+---------------------------------------------+
   | driver-lifecycle.txt  | DONE. DriverEntry, AddDevice, PnP and       |
   |                       | power, including the teardown ordering.     |
   +-----------------------+---------------------------------------------+
   | driver-structures.txt | DONE. The 0x1800-byte device extension, the |
   |                       | structures embedded in it, the Windows and  |
   |                       | USB layouts the driver indexes, and what    |
   |                       | changes on 64-bit.                          |
   +-----------------------+---------------------------------------------+
   | function-map.txt      | DONE. Every named function across the three |
   |                       | binaries, and how coverage was established. |
   +-----------------------+---------------------------------------------+
   | config-format.txt     | DONE. The on-disk configuration and profile |
   |                       | format.                                     |
   +-----------------------+---------------------------------------------+
   | configurator-         | DONE. How wishd201.exe is built: the edit-  |
   | architecture.txt      | as-script-text model, device discovery,     |
   |                       | driver installation, and the user-mode half |
   |                       | of the event queue.                         |
   +-----------------------+---------------------------------------------+
   | replacement-          | DONE. Design note: the candidate            |
   | architecture.txt      | architectures, which one was chosen, and    |
   |                       | what that costs.                            |
   +-----------------------+---------------------------------------------+
   | port-notes.txt        | PLANNED. What must change for 64-bit beyond |
   |                       | the structure layouts: calling convention,  |
   |                       | and WDM to KMDF or UMDF2.                   |
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
   restating it.

   THESE DOCUMENTS STATE WHAT IS TRUE NOW. They are not a development log.
   A finding that turns out to be wrong is DELETED and replaced by the right
   one - not annotated, not retracted in place, not given a paragraph
   recording that someone once believed it. There is no "an earlier revision
   said", and no before-and-after.

   THE ONE EXCEPTION IS A TRAP: code that reads as a defect and is not, an
   approach that looks correct and fails for a reason invisible from the
   result, a tool whose obvious use breaks. Those are recorded as standing
   cautions in the present tense, because a reader without them repeats the
   mistake. known-defects.txt sections 3 and 7 are the pattern: stated as
   "this is deliberate, here is why it looks wrong", never as "this was
   once reported as a bug".
