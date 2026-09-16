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

/* ======================================================================
 * THE POLLING ENGINE
 *
 * The top of the input path. Everything the adapter reports arrives here and
 * leaves through core_on_raw_packet.
 *
 * A continuous double-buffered USB interrupt read: TWO IRPs in flight at all
 * times, each completion resubmitting immediately, so one read is always
 * outstanding while the other is being processed. That is what keeps latency
 * down without a timer - and two is not an arbitrary number, it is what the
 * original allocates, scans and matches against.
 *
 * FIVE BYTES is the whole controller state, and it is the raw layout
 * core_decode expects: X, Y, status, button hi, button lo.
 *
 * THE STOP MASK IS A BITMASK, not a flag. Polling runs only while it is
 * zero, so a stop for one reason cannot be undone by a start for another.
 * ====================================================================== */

#define ADAPTOID_POLL_SLOTS     2
#define ADAPTOID_POLL_BYTES     CORE_RAW_PACKET_BYTES

typedef struct _ADAPTOID_POLL_SLOT {
	/*
	 * Non-zero while a stop is cancelling this slot. It is how the stop and
	 * completion paths agree on ownership: whoever finds it CLEAR frees the
	 * IRP and URB, so they are never freed twice and never leaked.
	 */
	LONG  CancelLatch;
	PIRP  Irp;
	PVOID Urb;
	UCHAR Buffer[ADAPTOID_POLL_BYTES];
	UCHAR Active;           /* this slot should have a read outstanding */
} ADAPTOID_POLL_SLOT;

/* ======================================================================
 * THE REPORT QUEUE AND PENDING READS
 *
 * The other half of core_emit. hidclass sends down IOCTL_HID_READ_REPORT
 * and the driver answers it either immediately, from a report already
 * queued, or later, when one arrives.
 *
 * THE SAME SHAPE AS THE NOTIFICATION QUEUE in ioctl.c - two queues paired
 * one for one with a cancel-safe handshake - but NOT the same policy. When
 * that one fills it drops its OLDEST entry; this one discards the NEWEST and
 * keeps what it has. Reproduced, but worth knowing which way round it is: a
 * backed-up report queue keeps replaying stale input rather than catching up.
 * ====================================================================== */

#define ADAPTOID_REPORT_QUEUE_MAX   100

typedef struct _ADAPTOID_REPORT_NODE {
	UCHAR Length;
	UCHAR Data[CORE_REPORT_MAX_BYTES];
} ADAPTOID_REPORT_NODE;

/*
 * Claim a parked read for completion. Non-zero if it is still ours; zero
 * means cancellation got there first and the canceller owns completing it.
 * The same question the notification queue asks, and the same reason it is
 * a seam: it is a cancellation question, so the OS answers it.
 */
int  AdaptoidClaimIrp(PIRP Irp);

void AdaptoidPollComplete(struct _ADAPTOID_DEVEXT *DevExt, ULONG Slot,
                          NTSTATUS Status, ULONG Length);
void AdaptoidQueueRead(struct _ADAPTOID_DEVEXT *DevExt, PIRP Irp);
PIRP AdaptoidDequeueRead(struct _ADAPTOID_DEVEXT *DevExt);
void AdaptoidCancelPendingReads(struct _ADAPTOID_DEVEXT *DevExt);
NTSTATUS AdaptoidReadReport(struct _ADAPTOID_DEVEXT *DevExt, PIRP Irp);

/* ======================================================================
 * DEVICE NAMING
 *
 * The display string IOCTL function 0x835 returns, and the key the
 * driver-wide device list is sorted by. It is a USB TOPOLOGY PATH: a
 * controller letter followed by one digit per hub tier.
 *
 *     A21     controller A, port 2 of the root hub, port 1 of the hub there
 *
 * HOW THE DRIVER FINDS ITSELF is the interesting part. It cannot ask the bus
 * driver "which port am I on"; instead it asks the ADAPTER, with vendor
 * request 0x75, for its own USB bus address, and then walks every host
 * controller and every hub looking for the port whose DeviceAddress matches.
 * The device tells you who it is and you go and find it.
 *
 * THE FIELD THAT HOLDS THAT ADDRESS WAS CALLED FirmwareRevision. It is not
 * one: drv_QueryFirmwareInfo stores the first byte of vendor request 0x75,
 * and drv_FindDeviceOnHub compares it against DeviceAddress at offset 25 of
 * the packed 0x23-byte USB_NODE_CONNECTION_INFORMATION. The firmware
 * revision the configurator displays is a different thing entirely -
 * bcdDevice, read out of the device descriptor by IOCTL 0x836 selector 1.
 * ====================================================================== */

#define ADAPTOID_VENDOR_ID      0x06F7u
#define ADAPTOID_PRODUCT_ID     0x0001u

/* Host controllers are searched as \\DosDevices\\HCD0 .. HCD5, and the
 * controller's letter is 'A' plus its index. */
#define ADAPTOID_MAX_CONTROLLERS 6

/* One port of one hub, as the topology seam reports it. */
typedef struct _ADAPTOID_PORT_INFO {
	int    Connected;
	int    IsHub;
	USHORT VendorId;
	USHORT ProductId;
	USHORT DeviceAddress;
	/* Valid only when IsHub; the name to recurse into. */
	const WCHAR *ChildHubName;
} ADAPTOID_PORT_INFO;

/*
 * THE TOPOLOGY SEAM. Walking hubs is four USB IOCTLs and a pile of
 * marshalling; deciding what the walk MEANS is a dozen lines of recursion.
 * Splitting them is what lets the naming be tested against a made-up
 * topology instead of a real one.
 *
 * PortCount is the number of ports on that hub; ports are numbered from 1.
 */
typedef NTSTATUS (*ADAPTOID_HUB_PORTS_FN)(void *ctx, const WCHAR *HubName,
                                          ULONG *PortCount);
typedef NTSTATUS (*ADAPTOID_PORT_INFO_FN)(void *ctx, const WCHAR *HubName,
                                          ULONG Port,
                                          ADAPTOID_PORT_INFO *Info);
/* The root hub of controller Index, or failure if there is none. */
typedef NTSTATUS (*ADAPTOID_ROOT_HUB_FN)(void *ctx, ULONG Index,
                                         const WCHAR **HubName);

typedef struct _ADAPTOID_TOPOLOGY {
	ADAPTOID_ROOT_HUB_FN  RootHub;
	ADAPTOID_HUB_PORTS_FN HubPorts;
	ADAPTOID_PORT_INFO_FN PortInfo;
	void                 *Context;
} ADAPTOID_TOPOLOGY;

/*
 * Build the location name for the device at UsbAddress into Name.
 *
 * NameBytes BOUNDS THE WRITE, which the original does not do anywhere on
 * this path: it builds the name with pool allocations and then copies it
 * into a ten-byte field with a plain strcpy. See known-defects.txt section 6.
 *
 * Returns non-zero if the device was found and named.
 */
int AdaptoidBuildLocationName(const ADAPTOID_TOPOLOGY *Topo,
                              USHORT UsbAddress, char *Name,
                              ULONG NameBytes);

/* ======================================================================
 * USB PORT RECOVERY
 *
 * What happens when a read fails. A ladder, not a loop: try to recover the
 * port up to three times, and if that does not work, cycle it and let the
 * device re-enumerate.
 * ====================================================================== */

/* Port status bits, as the bus driver reports them. */
#define ADAPTOID_PORT_ENABLED   0x1u
#define ADAPTOID_PORT_CONNECTED 0x2u

/*
 * The sentinel drv_RecoverPort returns when it has ALREADY cycled the port
 * itself. It is not a normal error: the retry loop tests for it specifically
 * and stops, because retrying would cycle the port again and again.
 *
 * An earlier note recorded this value as unexplained. It is simply
 * drv_RecoverPort saying "I have already given up on your behalf".
 */
#define ADAPTOID_STATUS_GAVE_UP ((NTSTATUS)0xC0012345L)

/* How many times the worker tries before cycling the port. */
#define ADAPTOID_RECOVER_TRIES  3

NTSTATUS AdaptoidRecoverPort(struct _ADAPTOID_DEVEXT *DevExt);
void     AdaptoidPollRestartWorker(struct _ADAPTOID_DEVEXT *DevExt);

/* The OS edge of recovery. */
NTSTATUS AdaptoidUsbGetPortStatus(struct _ADAPTOID_DEVEXT *DevExt,
                                  ULONG *Status);
NTSTATUS AdaptoidUsbResetPort(struct _ADAPTOID_DEVEXT *DevExt);
void     AdaptoidUsbCyclePort(struct _ADAPTOID_DEVEXT *DevExt);

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

	/* The polling engine. */
	KSPIN_LOCK          PollLock;
	ADAPTOID_POLL_SLOT  PollSlot[ADAPTOID_POLL_SLOTS];
	ULONG               PollStopMask;
	ULONG               PollRestartPending;

	/*
	 * The report queue, and the reads waiting on it. A fixed ring rather
	 * than the original's pool allocations: the cap is the same 100, and
	 * pre-allocating removes a failure path that had nothing useful to do
	 * with it anyway - the original silently drops the report if the
	 * allocation fails, which is what a full queue does too.
	 */
	KSPIN_LOCK           ReportLock;
	ADAPTOID_REPORT_NODE ReportQueue[ADAPTOID_REPORT_QUEUE_MAX];
	LONG                 ReportHead;
	LONG                 ReportCount;
	ULONG                ReportsDropped;

	LIST_ENTRY           PendingReads;
	LONG                 PendingReadCount;

	/*
	 * The device's own USB bus address, from vendor request 0x75. Named
	 * FirmwareRevision in the original, which it is not - see the note
	 * above DEVICE NAMING. It is what locates this adapter in the hub
	 * topology.
	 */
	USHORT               UsbAddress;

	/* How to walk the hubs. Filled at AddDevice; a seam so that naming
	 * can be tested against a topology that does not exist. */
	ADAPTOID_TOPOLOGY    Topology;
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
#define ADAPTOID_STOP_REASON_ERROR  1

/*
 * Where an IRP's queue link lives. The DDK puts it in Tail.Overlay; the
 * harness has a plain member. One accessor so wdm.c never names either.
 */
#ifdef ADAPTOID_USERMODE
#define ADAPTOID_IRP_LIST_ENTRY(Irp)  (&(Irp)->ListEntry)
#define ADAPTOID_IRP_FROM_ENTRY(e)    	((PIRP)((char *)(e) - (char *)&(((PIRP)0)->ListEntry)))
#else
#define ADAPTOID_IRP_LIST_ENTRY(Irp)  (&(Irp)->Tail.Overlay.ListEntry)
#define ADAPTOID_IRP_FROM_ENTRY(e)    	CONTAINING_RECORD((e), IRP, Tail.Overlay.ListEntry)
#endif

void     AdaptoidDevExtInit(struct _ADAPTOID_DEVEXT *DevExt);
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
int      AdaptoidPollStop(struct _ADAPTOID_DEVEXT *DevExt, ULONG Reason,
                          ULONG Slot);
/* The OS edge of the poll loop: build and submit one interrupt read. */
NTSTATUS AdaptoidPollSubmit(struct _ADAPTOID_DEVEXT *DevExt, ULONG Slot);
void     AdaptoidFreePollIrp(PIRP Irp, PVOID Urb);
void     AdaptoidCancelIrp(PIRP Irp);
void     AdaptoidQueuePollRestart(struct _ADAPTOID_DEVEXT *DevExt);
NTSTATUS AdaptoidCompleteRead(struct _ADAPTOID_DEVEXT *DevExt, PIRP Irp,
                              const UCHAR *Data, UCHAR Length);
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
