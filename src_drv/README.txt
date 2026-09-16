Adaptoid Reverse Engineering                                 Build Note
wishk300                                                     Replacement driver


                        SOURCE TREE AND BUILD


Abstract

   src_drv holds the clean-room replacement for wishk201.sys, and a user-mode
   test harness that runs the same source as an ordinary console program.

   This note records how the tree is laid out, how to build it, and the
   settings that are not obvious. It does not describe driver behaviour;
   ../docs holds that.

   The replacement follows Architecture 1 of ../docs/replacement-architecture
   .txt: a per-device HID minidriver that owns the adapter, rather than the
   virtual-HID design. Section 8.5 of that note records why, for a single
   known adapter, owning the device is strictly better.

Table of Contents

   1.  Layout
   1.1.  Where Each Function Came From
   2.  Building
   3.  The Core Seam
   4.  Why The Driver Uses The WDK 7.1 Toolchain
   5.  Settings That Are Not Obvious
   6.  What Is Not Here Yet

1.  Layout

   +-----------------+---------------------------------------------------+
   | File            | Contents                                          |
   +=================+===================================================+
   | adaptoid.sln    | Both projects, four configurations                |
   | common.props    | WdkRoot, output layout, shared C settings         |
   | driver.vcxproj  | core.c + wdm.c            -> wishk300.sys         |
   | harness.vcxproj | core.c + wdm.c + harness.c -> wishk300.exe        |
   | core.h core.c   | OS-free logic. No Windows types at all.           |
   | wdm.h wdm.c     | DriverEntry, AddDevice, PnP, power, IOCTLs        |
   | kstub.h         | Fake kernel ABI for user-mode builds              |
   | harness.c       | main() plus the kernel stub implementations       |
   | origin.tsv      | Where each function came from in Ghidra           |
   | origin.txt      | What the rows in origin.tsv mean                  |
   | build/          | All output. Not tracked.                          |
   +-----------------+---------------------------------------------------+

   THE RULE THAT MATTERS: core.c and core.h must never include a Windows or
   DDK header, call a kernel API, or name a Windows type. Everything they
   need from outside arrives through the two seams in section 3.

   That rule is load-bearing rather than stylistic. Measured against the 2001
   driver, the script, report, effect and CRC code reaches only 18 of that
   driver's 49 kernel imports, and all 18 reduce to those two seams. Holding
   the line is what lets the interesting code be tested without a VM.

1.1.  Where Each Function Came From

   origin.tsv maps every function here to the address and name of the original
   it derives from, because the names have already diverged and no rule
   recovers that - core_pak_addr_crc5 was drv_N64PakAddrCrc5. Add a row in the
   same change that adds a function.

       python tools/originmap.py --check    report drift, exit 1 if any
       python tools/originmap.py --fix      sort and normalise

   origin.txt section 3 explains the entries that are not one to one.

2.  Building

   From a plain shell, no Visual Studio environment needed:

       msbuild src_drv\adaptoid.sln /p:Configuration=Debug /p:Platform=Win32

   Platforms are Win32 and x64; configurations are Debug and Release. All
   four combinations build clean. Output goes to

       build\<Platform>\<Configuration>\wishk300.sys
       build\<Platform>\<Configuration>\wishk300.exe

   with intermediates under build\obj\<Project>\<Platform>\<Configuration>\.

   The driver needs WDK 7.1 at the path in WdkRoot, which defaults to
   E:\DEV\WinDDK. Override it with /p:WdkRoot=... or the WDK71_ROOT
   environment variable. A wrong path fails once with a clear message rather
   than a thousand missing-header errors.

   The harness needs nothing beyond Visual Studio 2017 and the Windows SDK.
   Run it directly; it exits non-zero on failure, so it is usable from a
   script.

3.  The Core Seam

   core.c receives input and emits output through function pointers:

       typedef void (*core_report_fn)(void *ctx, u8 report_id,
                                      const u8 *data, u32 len);

       void core_init(core_state *cs, core_report_fn sink, void *sink_ctx);
       void core_on_raw_packet(core_state *cs, const u8 *raw);
       void core_tick(core_state *cs, u64 now_100ns);

   core_report_fn is the seam the 2001 driver calls drv_SubmitHidReport. In
   the driver it completes a pending HID read IRP; in the harness it prints.
   Cutting here is what removes IoAllocateIrp, IoFreeIrp, IofCallDriver and
   IofCompleteRequest from the dependency closure of the logic.

   now_100ns replaces KeQueryInterruptTime and is the single most valuable
   stub in the tree. The harness advances it by hand, which makes the script
   scheduler, the effect ring and the keep-alive deterministic and steppable.
   A VM cannot offer that.

4.  Why The Driver Uses The WDK 7.1 Toolchain

   No WDK is installed on this machine. Windows Kits 10 is present but is
   SDK-only: it has um and ucrt headers, no km headers, no kernel libraries,
   and no WindowsDriver targets. The Visual Studio Driver project type is
   therefore unavailable.

   WDK 7.1.0 at E:\DEV\WinDDK does ship a complete kernel toolchain,
   including its own compiler and linker:

       bin\x86\x86\cl.exe       15.00.30729.207   (x86)
       bin\x86\amd64\cl.exe     15.00.30729.207   (x64)
       matching link.exe        9.00.30729.207

   That is the Visual Studio 2008 SP1 toolchain those headers were written
   and tested against. So rather than pairing 2009 headers with the 2018
   compiler - which fails on at least one count, since cl 19 emits calls to
   __report_rangecheckfailure and that symbol does not exist in the 2009
   kernel libraries - driver.vcxproj keeps the v141 project system for build
   plumbing and redirects the CL and Link tasks:

       CLToolPath / CLToolExe / LinkToolPath / LinkToolExe

   One solution, one build command, a matched toolchain, and no installs.
   IncludePath and LibraryPath are overridden outright rather than inherited,
   because the DDK ships its own SAL 1 sal.h and it must be the one that
   wins.

   Compiler and linker flags follow the DDK build system rather than being
   invented here. E:\DEV\WinDDK\bin\makefile.new, i386mk.inc and amd64mk.inc
   are the reference.

   If the modern WDK is ever installed, WdkRoot and the two path properties
   are the only things that change.

5.  Settings That Are Not Obvious

   Each of these was found the hard way and is commented at its site.

   /Z7 RATHER THAN /Zi. mspdbsrv.exe is absent from the DDK bin tree, so the
   PDB server cannot be spawned and /Zi fails. /Z7 keeps debug information in
   the object file, and is the DDK default anyway.

   UseDebugLibraries IS FALSE IN EVERY DRIVER CONFIGURATION, Debug included.
   Four v141 defaults key off that one property: /MDd, /RTC1, /Od and
   /DEBUG:FASTLINK. link 9.00 rejects /DEBUG:FASTLINK outright, and the /RTC
   switches emit _RTC_InitBase and friends, which the 2009 libraries do not
   provide. Debug-ness is set explicitly instead.

   GenerateDebugInformation IS true, NOT DebugFull. link 9.00 ignores
   /DEBUG:FULL with LNK4224, which would silently produce no PDB at all.

   THE ENTRY POINT IS GsDriverEntry, NOT DriverEntry. /GS makes
   BufferOverflowK.lib::GsDriverEntry the real entry; it initialises the
   stack cookie and then calls DriverEntry. x86 decorates the stdcall name as
   GsDriverEntry@8, x64 does not.

   /Gz ON x86. The DDK declares its exports with no explicit calling
   convention and hidclass.lib exports _HidRegisterMinidriver@4, so stdcall
   must be the compiler default. STD_CALL must be defined to match, and
   neither may be set without the other.

   NonCoreWin IS true. Without it Microsoft.Cpp.CoreWin.props prepends
   kernel32.lib, user32.lib and nine others to the link line.

   NO /DYNAMICBASE, IN EITHER DIRECTION. link 9.00 rejects the switch
   alongside /DRIVER; driver ASLR arrived with the Win8 WDK. The property is
   left empty so nothing is emitted.

   INCREMENTAL LINKING IS OFF FOR BOTH PROJECTS. They share an output
   directory and both target the base name wishk300, so each would want the
   same wishk300.ilk. The linker PDB is named wishk300.sys.pdb and
   wishk300.exe.pdb for the same reason. Intermediates are per-project
   because two projects sharing an IntDir is an MSB8027 error, and because
   both compile core.c with different toolchains and different defines.

   LNK4078 IS SUPPRESSED, and only that one. link adds the WRITE attribute to
   any section named in /SECTION, so the image INIT section differs from the
   one the compiler emitted and the linker reports the mismatch. Verified by
   dumping both. The resulting section is correct - discardable code - and
   4078 is in the DDK own suppression list.

6.  What Is Not Here Yet

   This is a skeleton. Every function has the signature it will be called
   with and a body that is a stub; the shape is the deliverable, not the
   behaviour. Still to come, each with its specification already written:

   +-------------------------+-------------------------------------------+
   | Piece                   | Specification                             |
   +=========================+===========================================+
   | Report descriptor       | ../docs/hid-descriptor.txt section 4      |
   | Raw packet decode       | ../docs/usb-transport.txt                 |
   | Script interpreter      | ../docs/script-bytecode.txt sections 6, 7 |
   | Effect engine           | ../docs/driver-structures.txt section 2.6 |
   | PnP, power, URB plumbing| ../docs/driver-lifecycle.txt              |
   | Private IOCTL surface   | ../docs/ioctl-surface.txt                 |
   | Controller Pak CRCs     | ../docs/usb-transport.txt                 |
   +-------------------------+-------------------------------------------+

   Loading the driver is out of scope here. It is unsigned, and x64 Windows
   will not load an unsigned driver without test-signing mode; see
   ../docs/replacement-architecture.txt section 8.4.

   Note also that none of the 2001 offsets carry over to 64-bit. The portable
   content is field order and meaning, which is why the extension is written
   as a struct rather than an offset table; ../docs/driver-structures.txt
   section 8 lists what changes.
