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
   6.  The File Plan
   6.1.  State
   6.2.  Specifications For What Remains
   6.2.1.  The Post-Mortem Dump Is Still A 32-Bit Layout
   6.3.  Seams That Are Wired Only In The Harness
   7.  Installing It
   7.1.  Signing
   7.2.  Redeploying Over A Running Driver
   8.  Out Of Scope

1.  Layout

   +-----------------+---------------------------------------------------+
   | File            | Contents                                          |
   +=================+===================================================+
   | adaptoid.sln    | Both projects, four configurations                |
   | common.props    | WdkRoot, output layout, shared C settings         |
   | driver.vcxproj  | the sources below         -> wishk300.sys         |
   | harness.vcxproj | the same, plus harness.c  -> wishk300.exe         |
   | core.h core.c   | OS-free logic. No Windows types at all.           |
   | script.h .c     | The bytecode interpreter and its builtins         |
   | sched.h .c      | Script threads, the scheduler, input binding      |
   | ioctl.h .c      | The private IOCTL surface and control device      |
   | wdm.h wdm.c     | DriverEntry, AddDevice, PnP, power, URBs          |
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

   KeAcquireSpinLock, NOT KfAcquireSpinLock. The Kf forms are x86-only
   fastcall exports: on x64 the symbol does not exist and the link fails
   with an unresolved external, having compiled without complaint. The DDK's
   two-argument KeAcquireSpinLock(Lock, &OldIrql) is a macro over Kf on x86
   and a real function on x64, so it is the only spelling that works in both.
   The 2001 driver uses the Kf forms throughout, because it only ever had to
   be a 32-bit driver.

   THIS IS WHY ALL FOUR CONFIGURATIONS GET BUILT rather than just the one
   being worked on. The harness is Win32 and would never have shown it; the
   x64 driver link is what caught it.

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

6.  The File Plan

   THE SET OF SOURCE FILES IS FIXED AT SIX. Everything still to be ported
   has a named home in the table below, and nothing outside this list is to
   be created without a deliberate decision to change the plan. The reason
   for writing it down is that a port of this size drifts into a file per
   subsystem if each one is decided on its own.

   +-----------+----------+---------+---------------------------------------+
   | File      | Now      | Planned | Holds                                 |
   +===========+==========+=========+=======================================+
   | core.c    |     2088 |    2088 | decode, effects, Pak CRCs, the N64    |
   |           |          |         | transaction, HID report state; done   |
   +-----------+----------+---------+---------------------------------------+
   | script.c  |      382 |     382 | the bytecode interpreter, and only    |
   |           |          |         | that; it is finished                  |
   +-----------+----------+---------+---------------------------------------+
   | sched.c   |     1069 |    1069 | thread pool, scheduler, input         |
   |           |          |         | binding, native builtins; finished    |
   +-----------+----------+---------+---------------------------------------+
   | ioctl.c   |     1303 |    1303 | both IOCTL surfaces, the registry and |
   |           |          |         | the notify queue; finished            |
   +-----------+----------+---------+---------------------------------------+
   | wdm.c     |     1582 |   ~2100 | DriverEntry, AddDevice, PnP, power,   |
   |           |          |         | polling, URB transport, device naming |
   +-----------+----------+---------+---------------------------------------+
   | harness.c |     6964 |   ~7400 | main() and every test                 |
   +-----------+----------+---------+---------------------------------------+

   THE BUILTINS WENT INTO sched.c, NOT script.c as first planned. Eleven of
   the seventeen are thread operations - _fork, _kill, _wake, _sleep, _exit,
   _getpid - and the rest post events; all of that is scheduler state that
   script.c deliberately cannot see. Moving them kept the layering intact at
   the cost of one file being larger than estimated. The file SET is what
   this section fixes, and that has not changed.

   WHY ioctl.c IS A FILE AND NOT PART OF wdm.c. It is not a size split. It
   holds BOTH dispatchers - the per-device surface and the control device's -
   plus the device registry the second one enumerates and the user-mode
   notification queue. That is the entire contract with wishd201.exe,
   specified in one document, ../docs/ioctl-surface.txt. One surface, one
   document, one file.

   THE WHOLE SURFACE IS OS-FREE, which is not obvious from the original's
   dispatcher. Every case on both surfaces is a length check and a small
   action on device state; an IRP is decoded into a core_ioctl by the
   caller and what comes back is a status and a byte count. Only four things
   need seams: the raw vendor passthrough, the enable toggle, and the claim
   and deliver steps of
   the notification queue - the last two because whether a parked request is
   still ours is a cancellation question and therefore the OS's to answer.
   That is why the whole surface, including its four defects, is exercised
   in the harness.

   WHAT WENT TO wdm.c INSTEAD. The control DEVICE OBJECT - IoCreateDevice,
   the symbolic link, the open count, the Create/Close/Cleanup/ReadWrite
   handlers, the IRP cancel routines and the request routing in
   drv_IoctlViaHidHandle - is device-object plumbing and lives with the
   other device-object work. The registry those handles name is here; the
   objects they name are there.

6.1.  State

   142 of the 147 functions in wishk201.sys have a counterpart here. The
   five that do not are accounted for in origin.txt section 3: a release
   with nothing to release, two string helpers the naming does not need, and
   two compiler intrinsics.

   THE COUNT IS 147, NOT 146. drv_VendorRequestCompleteKeepSlot at 00018d10
   is reached only through a stored address and never by a CALL, so nothing
   leads a disassembler to it - it sits in what looks like padding between
   two other functions. Expect a function count from Ghidra to be one short
   until that one is created by hand.

   +--------------------------+---------------------------------------+
   | Subsystem                | Where it lives, how far it is trusted |
   +==========================+=======================================+
   | packet decode, reports   | core.c, test groups                   |
   | accessory probe          | core.c, test groups                   |
   | Controller Pak CRCs      | core.c, test vectors                  |
   | effect engine and ring   | core.c, test groups                   |
   | raw N64 transaction      | core.c, test groups                   |
   | keyboard and mouse       | core.c, test groups                   |
   | HID report descriptors   | core.c, byte-compared to the original |
   | script interpreter       | script.c, test programs               |
   | scheduler and builtins   | sched.c, test groups                  |
   | private IOCTL surface    | ioctl.c, test groups                  |
   | control device surface   | ioctl.c, test groups                  |
   | SDK command block        | ioctl.c, test groups                  |
   | HID minidriver contract  | ioctl.c + wdm.c, test groups          |
   | remove lock, transport   | wdm.c, test groups                    |
   | dispatch triage and PnP  | wdm.c, test groups                    |
   | input path and queues    | wdm.c, test groups                    |
   | device naming, recovery  | wdm.c, test groups                    |
   | power                    | wdm.c, test groups                    |
   | control device object    | wdm.c, test groups                    |
   | subsystem wiring         | wdm.c + core.c, test groups           |
   | THE USB LAYER            | wdm.c, COMPILES - NOTHING MORE        |
   | 64-bit division helpers  | not ported; the toolchain supplies it |
   +--------------------------+---------------------------------------+

   TWO THINGS ARE NOT SETTLED BY ANY AMOUNT OF PORTING, and both are worth
   knowing before trusting a build.

   THE USB LAYER HAS NEVER TALKED TO A DEVICE. Descriptor fetch, select
   configuration, the two asynchronous transfer types, abort and the port
   IOCTLs are URB marshalling against a bus the harness does not have. They
   are written from ../docs/usb-transport.txt and checked only by compiling.
   Every other subsystem here was checked against hand-built vectors. Treat
   this one as the least trustworthy code in the tree.

   NOTHING HAS BEEN LOADED. The four clean builds say the code compiles and
   links as a kernel driver. They say nothing about whether it runs. See
   section 7.

   TWO TRAPS IN HOW THIS TREE IS VERIFIED, both of which have already caught
   real defects and will again:

   -  A GREEN HARNESS IS NOT A WORKING DRIVER. Every subsystem can pass its
      own tests while none of them are connected to each other - no
      AddDevice installed, no clock driving the engines, script events
      reaching nothing. Unit tests cannot see it, because each part is
      correct. The wiring test group exists for exactly this and should be
      extended whenever a new seam is added.

   -  BUILD ALL FOUR CONFIGURATIONS, not just the harness. The harness uses
      kstub.h, and a type that is wrong there compiles cleanly and then
      fails against the real DDK - or worse, differs silently. POWER_STATE
      is a union and not a ULONG; KeSetTimer takes a LARGE_INTEGER by value;
      GET_SELECT_CONFIGURATION_REQUEST_SIZE(0, 0) underflows. None of those
      is visible from Win32 Debug alone.

   -  WHERE HARNESS.C SUPPLIES A FUNCTION, THE SUITE TESTS THE STUB AND NOT
      THE DRIVER. This is the sharpest limit in the tree and it has hidden
      three separate bugs, each of which reached hardware with every group
      green:

         AdaptoidCompleteRead     the real one released RemoveLockB, the
                                  stub did not. The reference count ran to
                                  -1103 on a live device.
         the cancel-routine tests they set Cancel and called the routine
                                  but left Irp->CancelRoutine installed,
                                  which the I/O manager clears first. A
                                  routine that re-claimed the IRP therefore
                                  passed here and leaked it on hardware,
                                  wedging the calling thread and the
                                  process with it.

      The rule that follows: a stub is a claim about the real function, and
      an untrue claim is worse than no test. When a bug is found in code the
      harness replaces, FIX THE STUB IN THE SAME CHANGE and confirm the test
      goes red against the old code - otherwise the suite certifies the
      opposite of what happened.

      Prefer compiling the real function into the harness over modelling it.
      AdaptoidCompleteRead could be, given read fixtures that carry a
      UserBuffer and an OutputBufferLength; it is still a stub only because
      that work has not been done.


6.2.  Specifications For What Remains

   +-------------------------+-------------------------------------------+
   | Piece                   | Specification                             |
   +=========================+===========================================+
   | PnP, power, URB plumbing| ../docs/driver-lifecycle.txt              |
   | Private IOCTL surface   | ../docs/ioctl-surface.txt                 |
   | SDK command block       | ../docs/command-block.txt                 |
   | USB transport           | ../docs/usb-transport.txt                 |
   +-------------------------+-------------------------------------------+

   Ported: the raw packet decode, the joystick report, the accessory probe,
   the Controller Pak CRCs, the effect engine and its ring, the raw N64
   transaction, the keyboard and mouse report state machines, the eighteen
   private IOCTL functions, the eleven control-device functions with their
   registry and notification queue, the SDK command-block channel, the
   whole of PnP and power, the USB layer, and the entire script engine -
   interpreter, scheduler, input binding and builtin library, specified in
   ../docs/script-bytecode.txt sections 5, 6 and 9.

6.2.1.  The Post-Mortem Dump Is Still A 32-Bit Layout

   KNOWN AND NOT FIXED. The fault dump that fn 0x83d returns is the thread
   node copied out verbatim, and the client parses it at the offsets the
   ORIGINAL's node had. That node was 32-bit: two 4-byte list pointers,
   then thread_id at +0x08 and pc at +0x0C. This one carries 8-byte
   pointers, so everything past them sits eight bytes later and every
   labelled field in the client is reading its neighbour:

   +--------------+--------+----------------------------------------+
   | Client shows | Reads  | Actually gets                          |
   +==============+========+========================================+
   | pid          | +0x08  | the low half of blink                  |
   | ip           | +0x0C  | the high half of blink                 |
   | a            | +0x10  | thread_id                              |
   | sp           | +0x14  | pc - the real faulting instruction     |
   +--------------+--------+----------------------------------------+

   Measured: a divide fault reported pid 0, ip 0, a 22, sp 806, against a
   script of 1340 opcodes with thread ids running to 21. "a" and "sp" are
   the thread id and the program counter; pid and ip are the zeroed list
   links.

   IT ALSO PUTS KERNEL POINTERS IN A USER BUFFER. flink, blink and the
   stack pointer are all copied, and their high halves are visible in the
   client's own display - every dump shows the ffff8a08 prefix of this
   machine's kernel addresses.

   The fix is not to pad the struct. It is to BUILD AN EXPLICIT 32-BIT
   RECORD at the drain rather than copying the live node, which settles
   the field offsets and the pointer disclosure together, because such a
   record would hold no pointers at all. That changes the shape
   ioc_script_fault writes, so it is a deliberate change to the contract
   rather than a repair, and it has not been made.

   IT ALSO CARRIES STALE STACK FROM OTHER THREADS. The dump is the node's
   whole capacity, stack_size words, not the live saved_depth, and nodes
   are recycled through the free list. Observed: a fault raised by a
   handler that never recursed returned a dump containing forty frames of
   a DIFFERENT handler's recursion - return address 0x146 with a counter
   walking 0x27 down to 0 - left in the recycled node below the live
   depth. Harmless within one script, since core_sched_unload drains the
   free pool, but it is script data the faulting thread never wrote. The
   explicit record above would end this too, by copying saved_depth words
   rather than the capacity.

6.3.  Seams That Are Wired Only In The Harness

   THESE COMPILE, PASS THEIR TESTS, AND DO NOTHING ON HARDWARE. Each is a
   consumer written against a seam whose real provider was never supplied,
   so harness.c installs a double, every group passes, and the driver
   silently has no such feature. They are listed together because they
   share that shape and because a green suite is what hides them.

   +--------------------+--------------------------------------------------+
   | Seam               | State                                            |
   +====================+==================================================+
   | Topology providers | ADAPTOID_TOPOLOGY.RootHub, HubPorts and PortInfo |
   |                    | are set only at harness.c:7631. Measured NULL on |
   |                    | a live device, so AdaptoidBuildLocationName      |
   |                    | fails its first check and every adapter is named |
   |                    | "?". See below.                                  |
   | Script stick owner | core_state.script_owns_stick is READ at          |
   |                    | core.c:426 and written only by the harness. The  |
   |                    | _stick builtin stores into stick_x and stick_y,  |
   |                    | but with the flag clear the next packet decode   |
   |                    | overwrites them, so the builtin has no lasting   |
   |                    | effect.                                          |
   +--------------------+--------------------------------------------------+

   THE DEVICE NAME IS USER VISIBLE AND COSTS MORE THAN IT LOOKS. fn 0x835
   returns that name, and the configurator finds adapters TWICE - once
   through the driver interface and once by walking the USB bus itself -
   then merges the two on the device path and the port path TOGETHER
   (../docs/configurator-architecture.txt section 5). Its own walk yields a
   real path such as A2 while the driver answers "?", so the match fails
   and one physical adapter appears as two entries: the working one, and a
   second the UI marks with a red X as "present but not claimed".

   Implementing the providers means, from wdm.c and at PASSIVE_LEVEL,
   opening \??\HCD0 through HCD9 and issuing the same hub IOCTLs the
   configurator uses - GET_ROOT_HUB_NAME, GET_NODE_INFORMATION,
   GET_NODE_CONNECTION_INFORMATION and GET_NODE_CONNECTION_NAME - each of
   which is a two-call query-size-then-fetch against a variable-length
   reply. The consumer, AdaptoidBuildLocationName and FindOnHub, is already
   written, already bounds its recursion and already has tests; only the
   three providers and a synchronous-IOCTL helper are missing.

   WEIGH IT AGAINST IoGetDeviceProperty FIRST. A minidriver reaching
   sideways to open host controller objects is unusual - user mode is the
   normal place for that walk, which is where the configurator does it -
   and DevicePropertyLocationInformation gives a port path with none of
   that machinery. It yields a DIFFERENT string, though, and the merge
   above needs the configurator's own format, so the cheaper route may not
   actually buy the fix.

7.  Installing It

   adaptoid.inf claims USB\VID_06F7&PID_0001 as HIDClass with wishk300.sys
   as the minidriver service. THE CLASS IS THE POINT: hidclass.sys has to own
   the device object for the composite descriptor to become a keyboard, a
   mouse and a game controller, so installing it as a raw USB device loses
   the property the driver exists for.

   IT DIVERGES FROM THE ORIGINAL'S INF DELIBERATELY. wishna1.inf declares a
   CUSTOM setup class, GCAClass, and creates it from [ClassInstall32] - and
   that is precisely what stops it installing on Windows 7, because staging
   a package does not register a class and a device cannot bind to an INF
   whose class does not exist. Using the standard HIDClass removes the
   failure mode. The original's INF is specified in
   ../docs/configurator-architecture.txt sections 7.3 to 7.5.

   It also carries the DirectInput registration the original's Joy.AddReg
   writes - axis and button names - minus the force-feedback CLSID, which
   names a COM object only Wish shipped.

   It also writes the one setting the driver reads. Note where:

       HKLM\Software\Wish Technologies\Adaptoid    VirtualDevices

   which is a FIXED ABSOLUTE PATH, not the service key - drv_RegQueryDword
   hardcodes it and takes only a value name, so the replacement does too.
   The value is read ONCE PER DEVICE ARRIVAL, so changing it needs a replug
   rather than a service restart.

7.1.  Signing

   64-bit Windows will not load an unsigned kernel driver. Two scripts take
   care of it; the first is run ONCE and the second after every build.

       tools\mktestcert.cmd                  make the certificate
       tools\signdriver.cmd [plat] [config]  sign a build

   mktestcert.cmd writes two files into src_drv\build\sign\, which is
   inside the gitignored build tree BECAUSE ONE OF THEM IS A PRIVATE KEY:

       adaptoid-test.pfx   the key, used by signdriver.cmd
       adaptoid-test.cer   the public half, to carry to the test machine

   It uses PowerShell's New-SelfSignedCertificate rather than the WDK's
   makecert.exe, which is deprecated and whose 2009 build defaults to SHA-1
   - and Windows 10 does not accept SHA-1 on a kernel-mode signature.

   signdriver.cmd does two separate things, and they answer two different
   questions:

       the .sys is EMBEDDED-SIGNED   so the kernel will LOAD it
       adaptoid.cat is built+signed  so PnP will INSTALL the package

   Signing one and not the other half-works in a way that is confusing: an
   unsigned catalog still installs, with a warning, and an unsigned .sys
   fails at load with nothing useful in the log.

   It picks the Windows 10 SDK's signtool deliberately - the WDK 7.1 one
   cannot produce a SHA-256 signature. Both files come out sha256RSA; that
   has been run and checked.

   ON THE TEST MACHINE, all three steps, and the second is the one people
   miss:

       certutil -addstore -f Root             adaptoid-test.cer
       certutil -addstore -f TrustedPublisher adaptoid-test.cer
       bcdedit /set testsigning on            (then REBOOT)

   Root alone gets the driver loading; PnP checks TrustedPublisher, so
   without it the INF install still prompts.

   THE CATALOG'S OS ATTRIBUTE IS THE ONE UNCERTAINTY. The only Inf2Cat on
   this machine is WDK 7.1's, whose newest /os value is 7_X64 - it predates
   Windows 8 and cannot write a Windows 10 attribute. The catalog it
   produces is correctly signed but claims the wrong platform. Whether a
   given Windows 10 build accepts that has NOT been tested here. If the
   install refuses the package, the options in order of preference are: get
   a modern Inf2Cat from a current WDK; install with
   "pnputil /add-driver adaptoid.inf /install" and accept the warning; or
   fall back to loading the .sys as a service by hand, which skips PnP
   entirely and therefore skips the catalog.

   THE PACKAGE INSTALLS AND THE DRIVER RUNS. It is deployed and loaded on
   64-bit Windows 10 with test signing on, hidclass accepts the composite
   descriptor, and the original 2001 configurator - a 32-bit binary - drives
   it: it opens the control device, enumerates the adapter, downloads
   compiled scripts, and keyboard and mouse remapping work. So the catalog
   attribute above is not a blocker in practice on that build.

7.2.  Redeploying Over A Running Driver

   THE CONFIGURATOR PINS THE OLD DRIVER IN MEMORY, and this is the one way
   a deploy reports complete success and changes nothing at all.

   wishk300.sys leaves memory only when its last device object goes.
   AdaptoidControlMaybeDelete deletes the control device only when the
   adapter count and the open handle count are BOTH zero, and wishd201.exe
   holds two handles on \\.\Wish_NA1 for as long as it runs. Unplugging the
   adapter is therefore not enough: the control device survives, the driver
   object survives, and Windows will not map a second copy of an image that
   is still resident.

   Everything downstream then succeeds. The file stages, pnputil installs
   the package, the rescan binds the device - and the kernel goes on
   executing the previous build.

   THE FILE ON DISK PROVES NOTHING. The only reliable check is the loaded
   image:

       lm vm wishk300

   The load address and the PDB GUID in the symbol path both change on
   every link. If neither moved across a deploy, the old image is still
   running no matter what the install reported.

   deploy.cmd refuses to run while wishd201.exe is present for this reason.
   Exit it from its tray icon first, or reboot after deploying.

8.  Out Of Scope

   The driver is test-signed rather than WHQL-signed, so x64 Windows loads
   it only with test-signing mode on; see
   ../docs/replacement-architecture.txt section 8.4. Production signing is
   out of scope here.

   Note also that none of the 2001 offsets carry over to 64-bit. The portable
   content is field order and meaning, which is why the extension is written
   as a struct rather than an offset table; ../docs/driver-structures.txt
   section 8 lists what changes.
