/*
 * wdm.h - the OS-facing layer of wishk300.
 *
 * This is the half of the driver that is ABOUT Windows: entry points, IRPs,
 * PnP, power, URBs and the private IOCTL surface. It is the only place that
 * may name a kernel type.
 *
 * It compiles twice. In the driver build it sees the real DDK headers. In the
 * harness build it sees kstub.h instead, and harness.c supplies the bodies of
 * the kernel routines it calls.
 */
#ifndef ADAPTOID_WDM_H
#define ADAPTOID_WDM_H

#ifdef ADAPTOID_USERMODE
#include "kstub.h"
#else
#include <ntddk.h>
#include <hidport.h>
#endif

#include "core.h"

/*
 * The per-device extension.
 *
 * hidclass allocates this for us, sized from DeviceExtensionSize in the
 * registration block, and hands it back as MiniDeviceExtension - see
 * ../docs/driver-structures.txt section 1 for the two-level hop.
 *
 * SKELETON: the 2001 driver's equivalent is 0x1800 bytes across 124 fields.
 * Only the members the skeleton needs are present. Note that none of the
 * original's offsets carry over to 64-bit; the portable content is field order
 * and meaning, which is why this is a struct and not an offset table.
 */
/* ======================================================================
 * THE REMOVE LOCK
 *
 * drv_LockInitialize / Acquire / Release / ReleaseAndWait (000158e0 ..
 * 000159a0) are a hand-rolled IoRemoveLock: a count that starts at one, an
 * event signalled when it reaches zero, and a Removed flag that makes every
 * further acquire fail. The 2001 driver carries its own rather than calling
 * the kernel's, so the replacement carries one too - and it is small enough
 * and pure enough to test, which the kernel's would not be.
 *
 * Two per device, as the original has: A guards the device object's
 * lifetime and B guards vendor transfers in flight.
 * ====================================================================== */

typedef struct _ADAPTOID_REMOVE_LOCK {
	LONG   IoCount;
	LONG   Removed;
	KEVENT RemoveEvent;
} ADAPTOID_REMOVE_LOCK, *PADAPTOID_REMOVE_LOCK;

void     AdaptoidLockInit(PADAPTOID_REMOVE_LOCK Lock);
NTSTATUS AdaptoidLockAcquire(PADAPTOID_REMOVE_LOCK Lock);
void     AdaptoidLockRelease(PADAPTOID_REMOVE_LOCK Lock);
void     AdaptoidLockReleaseAndWait(PADAPTOID_REMOVE_LOCK Lock);

/* ======================================================================
 * THE VENDOR TRANSPORT
 *
 * Exactly ONE vendor control transfer is in flight per device, and every
 * vendor command in the driver serialises through it. That is the slot the
 * core's core_vendor_claim_fn asks about and core_vendor_fn writes into;
 * this is the other end of both.
 *
 *     0  free
 *     1  claimed - a caller has it but has not submitted yet
 *     3  a URB is in flight
 *
 * A claim that cannot be granted is not an error: the work is remembered as
 * deferred and picked up when the slot frees. That is why core.c has
 * claim_effect_tick and its siblings.
 * ====================================================================== */

#define ADAPTOID_SLOT_FREE      0
#define ADAPTOID_SLOT_CLAIMED   1
#define ADAPTOID_SLOT_IN_FLIGHT 3

/*
 * A completion callback. Returning non-zero means "I have taken the slot
 * again and submitted more work"; zero means "I am finished with it". The
 * completion path walks the deferred queue until something takes it or the
 * queue empties.
 */
typedef int (*ADAPTOID_VENDOR_CALLBACK)(struct _ADAPTOID_DEVEXT *DevExt);

typedef struct _ADAPTOID_VENDOR_SLOT {
	KSPIN_LOCK Lock;
	LONG       State;
	PIRP       PendingIrp;      /* completed with the transfer's result */
	PIRP       UrbIrp;
	PVOID      Urb;
	ADAPTOID_VENDOR_CALLBACK Callback;

	/* The last transfer's outcome, which the pending IRP inherits. */
	NTSTATUS   LastStatus;
	ULONG      LastInformation;
	ULONGLONG  SubmitTime;
} ADAPTOID_VENDOR_SLOT, *PADAPTOID_VENDOR_SLOT;

/* Six bytes, exactly as they go on the wire. */
typedef struct _ADAPTOID_SETUP {
	UCHAR  bmRequestType;
	UCHAR  bRequest;
	USHORT wValue;
	USHORT wIndex;
} ADAPTOID_SETUP, *PADAPTOID_SETUP;

#define ADAPTOID_VENDOR_OUT     0x40u
#define ADAPTOID_VENDOR_IN      0xC0u

int      AdaptoidVendorTryClaim(struct _ADAPTOID_DEVEXT *DevExt);
int      AdaptoidVendorClaimForIrp(struct _ADAPTOID_DEVEXT *DevExt, PIRP Irp);
NTSTATUS AdaptoidVendorSend(struct _ADAPTOID_DEVEXT *DevExt,
                            const ADAPTOID_SETUP *Setup,
                            ULONG TransferLength, PVOID TransferBuffer,
                            ADAPTOID_VENDOR_CALLBACK Callback);
void     AdaptoidVendorComplete(struct _ADAPTOID_DEVEXT *DevExt,
                                NTSTATUS Status, ULONG Information);

struct _ADAPTOID_DEVEXT;

typedef struct _ADAPTOID_DEVEXT {
	PDEVICE_OBJECT  Self;
	PDEVICE_OBJECT  NextDeviceObject;
	PDEVICE_OBJECT  PhysicalDeviceObject;

	/* All the OS-free state lives here. */
	core_state      Core;

	/* Two remove locks, as the original has: A for the device object's
	 * lifetime, B for vendor transfers in flight. */
	ADAPTOID_REMOVE_LOCK RemoveLockA;
	ADAPTOID_REMOVE_LOCK RemoveLockB;

	ADAPTOID_VENDOR_SLOT Vendor;

	ULONG           Started;
} ADAPTOID_DEVEXT, *PADAPTOID_DEVEXT;

/*
 * Entry points. These twelve are the driver's entire OS-facing surface; the
 * set was read out of the Ghidra database for wishk201.sys rather than
 * assumed. Everything else the kernel invokes is an asynchronous callback
 * reached through a stored pointer, which the skeleton does not yet register.
 */
NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject,
                           PUNICODE_STRING RegistryPath);

NTSTATUS NTAPI AdaptoidAddDevice(PDRIVER_OBJECT DriverObject,
                                 PDEVICE_OBJECT FunctionalDeviceObject);

void     NTAPI AdaptoidUnload(PDRIVER_OBJECT DriverObject);

NTSTATUS NTAPI AdaptoidCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI AdaptoidClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI AdaptoidCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI AdaptoidRead(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI AdaptoidWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI AdaptoidDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI AdaptoidIntDeviceControl(PDEVICE_OBJECT DeviceObject,
                                             PIRP Irp);
NTSTATUS NTAPI AdaptoidPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI AdaptoidPower(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/*
 * The driver's report sink: the other end of core_report_fn. In the driver it
 * completes a pending HID read IRP; in the harness, harness.c supplies its own
 * sink instead and this one is never installed.
 */
void AdaptoidReportSink(void *ctx, u8 report_id, const u8 *data, u32 len);

/*
 * Complete one IRP. Broken out because the failure paths below all need it
 * and the harness needs somewhere to observe it.
 */
void AdaptoidCompleteIrp(PIRP Irp, NTSTATUS Status, ULONG Information);

/*
 * Put a prepared transfer on the wire. Separate from AdaptoidVendorSend so
 * that everything above it - validation, the slot, the remove lock - is
 * testable without a USB stack underneath.
 */
NTSTATUS AdaptoidVendorSubmitUrb(struct _ADAPTOID_DEVEXT *DevExt,
                                 const ADAPTOID_SETUP *Setup,
                                 ULONG TransferLength, PVOID TransferBuffer);

#endif /* ADAPTOID_WDM_H */
