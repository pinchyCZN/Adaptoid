# Adaptoid 64-bit driver

A clean-room replacement for the 2001 Adaptoid N64-to-USB adapter driver,
targeting modern 64-bit Windows.

The original `wishk201.sys` is a 32-bit WDM driver and will not load on x64
Windows at all. This port restores the adapter on current machines while
keeping the property the hardware was bought for: **remapping happens in the
driver, not in a user-mode hook.**

The driver registers through `HidRegisterMinidriver` and reports a composite
HID descriptor - keyboard, mouse and game controller in one device - so any
part of the N64 controller can be presented to the OS as genuine key or mouse
input. Every application sees real HID input, which is why it works in games
and software that ignore user-level key injection.

The original configurator, `wishd201.exe`, runs unmodified against it. Its
`.ac` script files compile and download the same way, so existing scripts
keep working.

## Status

| | |
| --- | --- |
| Driver | `wishk300.sys`, x64, HID minidriver over USBD |
| Configurator | the original 2001 `wishd201.exe`, unmodified |
| Scripts | the original `.ac` format, compiled by the original configurator |
| Signing | test-signed only - see [Installing](#installing) |

Not WHQL-signed and not production-signed. On x64 Windows that means it loads
only with test-signing mode enabled.

## Requirements

**To build**

- Visual Studio 2017 (the v141 toolset) and the Windows 10 SDK
- WDK 7.1, for the driver only - defaults to `E:\DEV\WinDDK`, override with
  `/p:WdkRoot=...` or the `WDK71_ROOT` environment variable

The driver project deliberately uses the WDK 7.1 toolchain rather than the VS
driver project system. A wrong `WdkRoot` fails once with a clear message
rather than a thousand missing-header errors.

**To run**

- 64-bit Windows with test-signing mode on
- An Adaptoid, `USB\VID_06F7&PID_0001`

## Building

From a plain shell - no Visual Studio environment needed:

```
msbuild src_drv\adaptoid.sln /p:Configuration=Release /p:Platform=x64
```

Platforms are `Win32` and `x64`; configurations are `Debug` and `Release`.
All four combinations build clean. Output lands in
`src_drv\build\<Platform>\<Configuration>\`:

- `wishk300.sys` - the driver
- `wishk300.exe` - the test harness

The harness needs nothing beyond VS2017 and the SDK. Run it directly; it
exits non-zero on failure, so it is usable from a script. It covers the
script interpreter, scheduler, HID report builders, IOCTL surface and PnP
handling against a faked kernel.

## Signing

64-bit Windows will not load an unsigned kernel driver. Two scripts handle
it - the first is run **once**, the second after every build.

**1. Make the test certificate** (once)

```
tools\mktestcert.cmd
```

Writes two files into `src_drv\build\sign\`, which is inside the gitignored
build tree **because one of them is a private key**:

- `adaptoid-test.pfx` - the key, used by `signdriver.cmd`
- `adaptoid-test.cer` - the public half, to carry to the target machine

It uses PowerShell's `New-SelfSignedCertificate` rather than the WDK's
`makecert.exe`, which is deprecated and whose 2009 build defaults to SHA-1 -
and Windows 10 does not accept SHA-1 on a kernel-mode signature.

**2. Sign a build**

```
tools\signdriver.cmd [platform] [config]
```

This does two separate things, answering two different questions:

- the `.sys` is **embedded-signed**, so the kernel will **load** it
- `adaptoid.cat` is built and signed, so PnP will **install** the package

Signing one and not the other half-works in a confusing way: an unsigned
catalog still installs with a warning, while an unsigned `.sys` fails at load
with nothing useful in the log.

**Or do everything at once**

```
tools\package.cmd [dest]
```

Builds x64 Release, stamps `DriverVer`, signs both files and stages a
ready-to-install package. Default destination is `B:\pkg`.

## Installing

Everything below is on the **target** machine, from an **elevated** prompt.

**1. Enable test signing** (once, needs a reboot)

```
bcdedit /set testsigning on
```

**2. Trust the test certificate** (once)

```
trustcert.cmd [path-to-adaptoid-test.cer]
```

It adds the certificate to two stores, and both are needed:

- `Root` - makes the chain verifiable at all; without it the signature
  terminates in an untrusted root
- `TrustedPublisher` - makes Windows willing to load it without asking

**3. Install the driver package**

```
deploy.cmd
```

Removes any stale device node and old packages, then installs with
`pnputil /add-driver ... /install` and rescans. Plug the adapter in
afterwards.

**4. Check it took**

```
state.cmd
```

**The file on disk proves nothing.** `wishk300.sys` leaves memory only when
its last device object goes, and `wishd201.exe` holds two handles on
`\\.\Wish_NA1` for as long as it runs - so unplugging the adapter is not
enough to unload the driver. A deploy can report complete success and change
nothing at all while the kernel goes on executing the previous build.
`deploy.cmd` refuses to run while the configurator is present for this
reason; exit it from its tray icon first.

The only reliable check is the loaded image - `lm vm wishk300` under a kernel
debugger. The load address and the PDB GUID both change on every link.

## Script engine fixes

The original's script engine has three defects that **bugcheck the machine
from an ordinary text script** - no crafted bytecode, no debugger, just a
`.ac` file compiled by the shipped configurator. All three are fixed here,
and all three were confirmed against the original or against an unfixed build
of this port.

**The fault snapshot had two owners.** When a script thread faults, its
thread node and a snapshot of the globals are parked in a single post-mortem
slot for the configurator to collect. The scheduler frees whatever it
supersedes, and the drain frees whatever it took - with nothing serialising
them. Two faults close enough together, which any script faulting in more
than one thread produces, gave a use-after-free and a double free. Reproduced
seven times on the original, landing on the same two instructions every time.
Fixed by exchanging the slot under a lock, with the rule that whoever takes a
pointer out of it owns that block exclusively.

**`INT_MIN / -1` was an unguarded CPU trap.** The interpreter checked the
divisor against zero and nothing else. Signed division has a second trapping
case: `INT_MIN / -1` overflows, because the true quotient is one past the
largest representable value, and x86 raises the *same* exception it raises
for division by zero. Modulo traps identically, since one `idiv` produces
both results. Inside the scheduler DPC at `DISPATCH_LEVEL` that is a bugcheck
rather than a script error. Measured: `KMODE_EXCEPTION_NOT_HANDLED`,
exception `c0000095` `STATUS_INTEGER_OVERFLOW`, at the bare `idiv`. Fixed by
rejecting the operand pair before the divide.

**Unloading a script armed the timer instead of stopping it.** The scheduler
asks the OS layer to stop its timer by arming for a wake time of zero -
"never". Computing a due time from that instead yields zero, which
`KeSetTimer` reads as *fire immediately*. So tearing a script down re-armed
the timer on the very path that then frees what the timer's DPC runs against.
Measured: bugcheck `D1`, an **execute** fault at `DISPATCH_LEVEL` in an image
already on the unloaded list, reached through `KiProcessExpiredTimerList`.
Fixed by honouring the cancel contract, refusing to arm once teardown has
begun, and flushing queued DPCs before anything is freed.

One more, not a crash but user-visible: **`_kill` never decremented the
thread count**, so after about thirty forks `_fork` refused permanently and
any script using threads was crippled for its lifetime. Fixed.

`docs/known-defects.txt` documents twenty-four defects in total, each with
its location, trigger, and whether it is fixed here.

## Known limitations

**Adapters are named `?`.** The configurator shows a `?` beside the
controller icon instead of a port path. The driver builds that name by
walking the USB hub tree to find the device at its own bus address, and the
consumer side - the walk, its recursion bounds and its tests - is written.
What is missing is the three provider callbacks that actually issue the hub
IOCTLs; they are wired only in the test harness, so on a live device they are
NULL, the walk fails its first check, and the name falls back to `?`. It is
cosmetic: it affects the label, not remapping, and the adapter works
normally. See `src_drv/README.txt` section 6.3.

**The script crash dump reads with shifted fields.** When a script faults,
the configurator displays the parked thread node - and it parses that node at
the **32-bit** offsets the original used. This driver's node carries 8-byte
list pointers, so everything past them sits eight bytes later and every
labelled field shows its neighbour: what the configurator calls `a` is really
the thread id, and what it calls `sp` is really the program counter, while
`pid` and `ip` read as zero. The stack bytes are also the node's whole
capacity rather than its live depth, so 64-bit kernel pointers and stale
frames from previously freed threads appear in the dump.

This is not a padding problem and is deliberately not patched around. The fix
is to build an explicit 32-bit record at the drain instead of copying the
live node, which settles the field offsets and the pointer disclosure
together - a considered change to the contract rather than a repair. See
`src_drv/README.txt` section 6.2.1.

**Scripts must use CRLF line endings.** An LF-only `.ac` file hangs the
configurator unrecoverably, and because it stores the last-loaded script it
re-hangs on every launch. That is a defect in the 2001 configurator, not in
this driver, and it bites anyone generating scripts with Unix tooling. See
`docs/known-defects.txt` section 24.

## Repository layout

| Path | What it is |
| --- | --- |
| `src_drv/` | the replacement driver and its test harness |
| `docs/` | findings and specifications, RFC-style ASCII |
| `tools/` | build, signing, deployment and documentation tooling |
| `tools/test_scripts/` | `.ac` scripts exercising the engine, including ones that crash the original |
| `decomp/` | the original 2001 binaries - primary evidence, never modified |

## A note on method

This is a **clean-room** reimplementation. Findings from the original
binaries are recorded as behavioural specifications - descriptor layouts,
IOCTL codes, protocol and timing, file formats - and the replacement is
written from those. No decompiler output is copied into the source.

`src_drv/origin.tsv` maps every function in the replacement to the address
and name of the original it derives from, and a tool checks both directions
so a function cannot drift out of the map unnoticed.
