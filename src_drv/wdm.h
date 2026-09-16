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
/* ======================================================================
 * THE DISPATCH TRIAGE
 *
 * One driver object serves three kinds of client from a single dispatch
 * table, with no filter driver and no second driver. DriverEntry fills the
 * table, calls HidRegisterMinidriver - which OVERWRITES it with hidclass's
 * entry points - then saves those pointers and installs wrappers on top.
 *
 *   1. the private control device, recognised by a magic number at the head
 *      of its device extension
 *   2. private per-device handles, recognised by a FileObject whose
 *      FileName is four bytes and whose second WCHAR is 'q' - an open of the
 *      ordinary HID interface path with a two-character suffix, claimed here
 *      before hidclass can see it
 *   3. everything else, chained to the saved hidclass handler untouched
 *
 * That is the whole trick behind the private configuration API, and it is
 * why hidclass.sys still owns the HID device in every respect Windows cares
 * about. See ../docs/ioctl-surface.txt section 1.
 * ====================================================================== */

/* The three dwords at the head of the control device's extension. Any value
 * would do; these are the original's. */
#define ADAPTOID_CDO_MAGIC0     0x3E178AA3u
#define ADAPTOID_CDO_MAGIC1     0x7625F013u
#define ADAPTOID_CDO_MAGIC2     0xED739374u

/* The suffix character that claims an open for the private channel. */
#define ADAPTOID_PRIVATE_CHAR   ((WCHAR)'q')

/*
 * The hidclass entry points HidRegisterMinidriver installed, saved so the
 * wrappers can chain to them. A null one means hidclass did not claim that
 * major function, and the wrapper answers STATUS_NOT_SUPPORTED.
 */
typedef struct _ADAPTOID_SAVED_DISPATCH {
	PDRIVER_DISPATCH Create;
	PDRIVER_DISPATCH Cleanup;
	PDRIVER_DISPATCH Close;
	PDRIVER_DISPATCH Read;
	PDRIVER_DISPATCH Write;
	PDRIVER_DISPATCH DeviceControl;
	PDRIVER_DISPATCH Pnp;
	PDRIVER_DISPATCH Power;
} ADAPTOID_SAVED_DISPATCH;

extern ADAPTOID_SAVED_DISPATCH AdaptoidSavedDispatch;

/* Which of the three a request belongs to. Exposed because the decision is
 * the interesting part and the harness checks it directly. */
#define ADAPTOID_ROUTE_HIDCLASS 0
#define ADAPTOID_ROUTE_CONTROL  1
#define ADAPTOID_ROUTE_PRIVATE  2

int AdaptoidRouteOf(PDEVICE_OBJECT DeviceObject, PIRP Irp);

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
	ULONG           Removing;
	ULONG           RemovePending;
	ULONG           StopPending;
	/* The one veto this driver casts on QUERY_STOP. */
	ULONG           StopVeto;
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
NTSTATUS NTAPI AdaptoidPnpTriage(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI AdaptoidPowerTriage(PDEVICE_OBJECT DeviceObject, PIRP Irp);
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

/* Why polling is stopped. The reasons are a bitmask so that a stop for one
 * reason cannot restart while another still holds it. */
#define ADAPTOID_STOP_REASON_PNP    4
#define ADAPTOID_STOP_REASON_REMOVE 8

NTSTATUS AdaptoidStartDevice(struct _ADAPTOID_DEVEXT *DevExt);
NTSTATUS NTAPI AdaptoidPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* Reach the minidriver extension from the device object. hidclass owns the
 * first level; ours hangs off it. See ../docs/driver-structures.txt 1. */
struct _ADAPTOID_DEVEXT *AdaptoidDevExtOf(PDEVICE_OBJECT DeviceObject);

/*
 * Stage-three stubs. Declared now because the dispatcher above names them,
 * and naming them is what fixes the shape of the next stage.
 */
NTSTATUS AdaptoidFetchDeviceDescriptor(struct _ADAPTOID_DEVEXT *DevExt);
NTSTATUS AdaptoidSelectConfiguration(struct _ADAPTOID_DEVEXT *DevExt);
void     AdaptoidSetDeviceName(struct _ADAPTOID_DEVEXT *DevExt);
void     AdaptoidPollStart(struct _ADAPTOID_DEVEXT *DevExt, ULONG Reason);
void     AdaptoidPollStop(struct _ADAPTOID_DEVEXT *DevExt, ULONG Reason);
void     AdaptoidQuiesceIo(struct _ADAPTOID_DEVEXT *DevExt);
void     AdaptoidUnconfigureDevice(struct _ADAPTOID_DEVEXT *DevExt);
void     AdaptoidAbortPipes(struct _ADAPTOID_DEVEXT *DevExt);
void     AdaptoidFreeDeviceResources(struct _ADAPTOID_DEVEXT *DevExt);
void     AdaptoidEnableInterface(struct _ADAPTOID_DEVEXT *DevExt);
void     AdaptoidRegistryRemove(struct _ADAPTOID_DEVEXT *DevExt);
void     AdaptoidSetCompletionRoutine(PIRP Irp, PVOID Event);
void     AdaptoidStartNextPowerIrp(PIRP Irp);

/* The control device and private channel handlers, stage three. */
NTSTATUS NTAPI AdaptoidControlCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI AdaptoidControlCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI AdaptoidControlClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI AdaptoidControlIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI AdaptoidControlReadWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI AdaptoidChannelCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI AdaptoidChannelClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS NTAPI AdaptoidChannelIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp);

#endif /* ADAPTOID_WDM_H */
