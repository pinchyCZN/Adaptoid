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
/* The USB layer. usbdi.h must come before usbdlib.h, and usbioctl.h brings
 * the IOCTL_INTERNAL_USB_* codes the port recovery sends. */
#include <usbdi.h>
#include <usbdlib.h>
#include <usbioctl.h>
#endif

#include "core.h"
#include "ioctl.h"

/* How many system power states DEVICE_CAPABILITIES.DeviceState maps. */
#define ADAPTOID_SYSTEM_STATE_MAX  PowerSystemMaximum

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

	/*
	 * Signalled when a transfer has finished, for the ONE caller that
	 * waits: AdaptoidQueryFirmwareInfo, which runs at PASSIVE_LEVEL
	 * during start and needs the answer before it can name the device.
	 * Nothing else on this transport blocks.
	 */
	KEVENT     Done;

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

/* "Where am I on the bus?" - see AdaptoidQueryFirmwareInfo. */
#define ADAPTOID_REQUEST_BUS_ADDRESS 0x75

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

/*
 * The pool tag, which shows as "Adp0" in a pool dump. The original uses
 * "Wish"; a replacement wants its own so the two can be told apart if they
 * are ever loaded on the same machine.
 */
/* "Adp0" as it appears in a pool dump. Written as the number rather than a
 * four-character constant, which overflows an int and warns. */
#define ADAPTOID_POOL_TAG       0x30706441UL

/*
 * How large the first configuration-descriptor fetch asks for. Generous on
 * purpose - this adapter's is a small fraction of it, so the fetch normally
 * costs one round trip rather than two. wTotalLength drives the retry when
 * it is not enough.
 */
#define ADAPTOID_CONFIG_FIRST_TRY 0x209

/*
 * What a poll completion is handed. The slot NUMBER rather than a pointer to
 * it, so the completion can reach the extension as well - and embedded in
 * the extension so submitting a read allocates nothing but the URB and IRP.
 */
typedef struct _ADAPTOID_POLL_CONTEXT {
	struct _ADAPTOID_DEVEXT *DevExt;
	ULONG                    Slot;
} ADAPTOID_POLL_CONTEXT, *PADAPTOID_POLL_CONTEXT;

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

/* ======================================================================
 * POWER
 *
 * THE DRIVER IS ITS OWN POWER POLICY OWNER. It does not simply pass power
 * IRPs down: it maps system states to device states itself, out of the
 * DEVICE_CAPABILITIES it fetched at start, and asks for the device IRP that
 * the mapping calls for.
 *
 * It also supports REMOTE WAKE, which for a 2001 game controller is unusual
 * and is easy to lose in a reimplementation - IRP_MN_WAIT_WAKE is accepted
 * whenever the device is out of D0 and the machine is no deeper than the
 * state the device can wake from.
 *
 * Two device-extension fields carry all of it, and the original's names for
 * them are misleading enough to be worth restating. DevicePowerState is the
 * CURRENT D-state; the original calls it SystemPowerState even though
 * drv_PrepareDevicePowerChange writes D1..D3 into it directly.
 * WakeIdleDeviceState is the state the device idles into, which is also the
 * deepest it can wake from; the original calls it PowerState.
 * ====================================================================== */

/* Device power states, as the Power.State parameter carries them. */
#define ADAPTOID_POWER_D0       1
#define ADAPTOID_POWER_D3       4

/* System power states. S0 is 1, matching PowerSystemWorking. */
#define ADAPTOID_POWER_S0       1

NTSTATUS AdaptoidPowerWaitWake(struct _ADAPTOID_DEVEXT *DevExt, PIRP Irp);

/*
 * The device state to move to for a given system state.
 *
 * THREE RULES, IN ORDER: the working system state always means D0; with no
 * wake armed the device drops straight to D3 whatever the capabilities say;
 * otherwise the capability table decides.
 *
 * Pure, so it is testable without a power IRP - which matters, because the
 * middle rule is the one a reimplementation gets wrong.
 */
ULONG AdaptoidDeviceStateFor(struct _ADAPTOID_DEVEXT *DevExt,
                             ULONG SystemState);

/*
 * Decide what a device power transition needs before it is passed down, and
 * do it. Returns non-zero if the transition needs a completion routine.
 *
 * Going DOWN is done here and needs no completion: polling is stopped and
 * I/O quiesced before the bus driver is allowed to remove power. Coming UP
 * is the other way round - nothing can be restarted until the bus driver has
 * actually powered the device, so that work happens in the completion, which
 * is what the non-zero return asks for.
 */
int AdaptoidPrepareDevicePower(struct _ADAPTOID_DEVEXT *DevExt, ULONG State);

/*
 * Move the device between its idle and working states, if it may.
 *
 * GoIdle non-zero asks to drop to WakeIdleDeviceState, zero asks to return
 * to D1. Returns the status of the request, or success having done nothing.
 *
 * THE AbortedPipeCount GATE IS DEAD IN THE ORIGINAL. Idling is refused while
 * it is non-zero and waking is refused while it is zero, and NOTHING IN THE
 * DRIVER EVER INCREMENTS IT - so the wake half can never run and the idle
 * half always passes. Reproduced, because a replacement that "fixes" it
 * silently changes when the device powers down.
 */
NTSTATUS AdaptoidUpdateIdlePower(struct _ADAPTOID_DEVEXT *DevExt,
                                 int GoIdle);

/* Is the device in a state where a power request makes sense? */
int AdaptoidIsDeviceReady(struct _ADAPTOID_DEVEXT *DevExt);

/*
 * THE OS EDGE OF POWER: ask the bus for a device power IRP, naming the
 * completion that finishes whatever this request was for. Two callers with
 * two different completions, which is why the routine is a parameter and not
 * a fixed choice inside.
 */
NTSTATUS AdaptoidRequestPowerIrp(struct _ADAPTOID_DEVEXT *DevExt, ULONG State,
                                 PREQUEST_POWER_COMPLETE Complete);

/* Ask for an idle transition on this driver's own behalf. */
NTSTATUS AdaptoidRequestDevicePower(struct _ADAPTOID_DEVEXT *DevExt,
                                    ULONG State);

void NTAPI AdaptoidSystemPowerComplete(PDEVICE_OBJECT DeviceObject,
                                       UCHAR MinorFunction,
                                       POWER_STATE PowerState,
                                       PVOID Context,
                                       PIO_STATUS_BLOCK IoStatus);
void NTAPI AdaptoidIdlePowerComplete(PDEVICE_OBJECT DeviceObject,
                                     UCHAR MinorFunction,
                                     POWER_STATE PowerState,
                                     PVOID Context,
                                     PIO_STATUS_BLOCK IoStatus);

/* The DPC that drives the script scheduler. */
void NTAPI AdaptoidScriptDpc(PKDPC Dpc, PVOID Context, PVOID Arg1,
                             PVOID Arg2);

/* ======================================================================
 * THE DEVICE ENABLE, AND THE KEEP-ALIVE WINDOW
 *
 * Turning the adapter on is not one vendor request but two, chosen by HOW
 * RECENTLY the last one went out:
 *
 *   inside three seconds of the last poke   a short kick, bRequest 0x32
 *   otherwise                               the full start, bRequest 0x72,
 *                                           whose completion then sends the
 *                                           same 0x32 kick
 *
 * so a device that is already running is not re-initialised. Turning it off
 * is always the single idle command.
 *
 * A BUSY VENDOR SLOT IS HANDLED ASYMMETRICALLY. Switching off defers - the
 * claim flag is set and the deferred-work drain will send it - while
 * switching on is simply dropped and STATUS_DEVICE_BUSY returned. That is
 * the safe way round, and deliberate: a missed "off" leaves a motor running.
 * ====================================================================== */

/* Three seconds, in 100ns units. */
#define ADAPTOID_KEEPALIVE_100NS  30000000ULL

/*
 * The two enable sequences, as they go on the wire. The kick and the idle
 * command share bRequest 0x32 and are told apart by wValue and wIndex, which
 * is worth naming rather than leaving as four bare numbers.
 */
#define ADAPTOID_ENABLE_KICK_REQUEST   0x32
#define ADAPTOID_ENABLE_KICK_VALUE     0x0002
#define ADAPTOID_ENABLE_KICK_INDEX     0xFE80
#define ADAPTOID_ENABLE_START_REQUEST  0x72
#define ADAPTOID_ENABLE_START_VALUE    0x0022
#define ADAPTOID_ENABLE_START_INDEX    0x0094

NTSTATUS AdaptoidSetDeviceEnable(struct _ADAPTOID_DEVEXT *DevExt, int On);
void     AdaptoidQueryFirmwareInfo(struct _ADAPTOID_DEVEXT *DevExt);
void     AdaptoidCancelVendorRequest(struct _ADAPTOID_DEVEXT *DevExt);
void     AdaptoidNotifyInterfaceChange(struct _ADAPTOID_DEVEXT *DevExt,
                                       int Live);
NTSTATUS NTAPI AdaptoidPassThroughDeviceControl(PDEVICE_OBJECT DeviceObject,
                                                PIRP Irp);
/* Publish this adapter's device interface; AddDevice's last step. */
NTSTATUS AdaptoidRegisterDeviceInterface(struct _ADAPTOID_DEVEXT *DevExt);
/* One DWORD out of the driver's service key, or Default if it is absent. */
ULONG    AdaptoidRegQueryDword(PUNICODE_STRING RegistryPath,
                               PCWSTR Name, ULONG Default);
void     AdaptoidSendIdleCommand(struct _ADAPTOID_DEVEXT *DevExt);

/* ======================================================================
 * THE CONTROL DEVICE OBJECT
 *
 * One singleton for the whole driver, created on the first adapter's arrival
 * and deleted with the last. It carries the registry every adapter registers
 * into, the driver-wide notification queue, and the SDK command-block
 * channel - all three of which are ioctl.c's, so what is here is the object,
 * the symbolic link and the reference count.
 *
 * THE REFERENCE COUNT IS THE SUBTLE PART, and the original gets it wrong:
 * drv_CreateControlDevice increments it even when IoCreateDevice FAILED, so
 * a failed creation still counts as a user. See known-defects.txt.
 * ====================================================================== */

/*
 * The names user mode reaches it by. \.\Wish_NA1 in the original, kept
 * because the SDK's own clients open it by that name and a replacement that
 * renames it is not a replacement.
 *
 * The device type 0xB98C is the same value the private IOCTL codes encode,
 * which is a useful consistency check when decoding them.
 */
#define ADAPTOID_CDO_NAME        L"\\Device\\Wish_NA1"
#define ADAPTOID_CDO_LINK        L"\\DosDevices\\Wish_NA1"
#define ADAPTOID_CDO_DEVICE_TYPE 0xB98C

typedef struct _ADAPTOID_CDO_EXT {
	/* What every dispatch wrapper tests to recognise this device. It has
	 * to be first, because that test runs on an extension whose type is
	 * not yet known. */
	ULONG            Magic[3];
	PDEVICE_OBJECT   Self;

	/* All three of the control device's surfaces live in ioctl.c. */
	core_registry    Registry;
	core_cmd_channel Channel;

	KSPIN_LOCK       Lock;
	LONG             OpenCount;
} ADAPTOID_CDO_EXT, *PADAPTOID_CDO_EXT;

/* 0x78 bytes in the original; ours is larger because the queue and the
 * registry are inside it rather than in pool. */
NTSTATUS AdaptoidCreateControlDevice(PDRIVER_OBJECT DriverObject);
void     AdaptoidReleaseControlDevice(void);

/* Delete the singleton when neither an adapter nor a handle needs it. */
void     AdaptoidControlMaybeDelete(void);

/* The singleton, or NULL. Does NOT take a reference - unlike the original's
 * drv_AcquireControlDeviceExt, which takes one and then leaks it on one of
 * its two failure paths. See known-defects.txt section 16. */
PADAPTOID_CDO_EXT AdaptoidControlDeviceExt(void);

/* ======================================================================
 * THE NOTIFICATION WAITER
 *
 * core_notify owns the queue; what is here is the IRP. A waiter is embedded
 * in the IRP's driver context so that no allocation can fail on this path,
 * and the cancel routine claims it with the same interlocked exchange the
 * report queue uses.
 * ====================================================================== */

/* Park this IRP on the queue. Returns STATUS_PENDING if it parked. */
/*
 * WHERE THE WAITER LIVES. In the IRP's own driver context, which the DDK
 * gives a driver four pointers of while it owns the request - a
 * core_notify_waiter is three. Parking it there rather than allocating means
 * queueing a notification has no failure path at all.
 */
#define ADAPTOID_IRP_WAITER(Irp) \
    ((core_notify_waiter *)ADAPTOID_IRP_CONTEXT(Irp))

void     AdaptoidNotifyInit(PADAPTOID_CDO_EXT CdoExt);
NTSTATUS AdaptoidWaitNotification(PADAPTOID_CDO_EXT CdoExt, PIRP Irp);

/* Cancel every parked waiter, or only those belonging to one file object.
 * Irp names the handle; NULL means all of them. */
void AdaptoidCancelNotifications(PADAPTOID_CDO_EXT CdoExt, PIRP Irp);

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
	ADAPTOID_POLL_CONTEXT PollContext[ADAPTOID_POLL_SLOTS];
	PVOID               PollWorkItem;   /* PIO_WORKITEM */
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

	/*
	 * The script scheduler, and the timer and DPC that drive it. The
	 * scheduler itself is OS-free; what is here is only what wakes it.
	 */
	core_sched           Sched;
	KTIMER               ScriptTimer;
	KDPC                 ScriptDpc;
	KSPIN_LOCK           ScriptLock;
	/*
	 * NOT A BOOLEAN, AND IT STARTS AT -1. The scheduler DPC increments it
	 * only while it is non-negative and decrements only while it is
	 * positive, so it stays latched at -1 until something else lifts it,
	 * and the report builder tests it for >= 0 to decide whether a script
	 * owns the stick. See drv_ScriptSchedulerDpc (00017910).
	 */
	LONG                 ScriptDepth;

	/* This adapter's row in the driver-wide registry. */
	core_device_entry    Registration;

	/* ---- power ---- */
	DEVICE_CAPABILITIES  Capabilities;
	ULONG                DevicePowerState;     /* D0 = 1 .. D3 = 4      */
	ULONG                WakeIdleDeviceState;  /* and the deepest wake  */
	ULONG                WaitWakePending;
	ULONG                PowerRequestInProgress;
	PIRP                 PendingSystemPowerIrp;
	/*
	 * NOTHING EVER INCREMENTS THIS, in the original or here. It gates the
	 * idle transition in AdaptoidUpdateIdlePower, and it is kept so that
	 * the gate behaves as the original's does.
	 */
	LONG                 AbortedPipeCount;

	/*
	 * ---- USB configuration ----
	 *
	 * Held as PVOID rather than as their USB types so that kstub.h does
	 * not have to reproduce the USB headers for a harness that never
	 * touches any of them.
	 */
	PVOID                ConfigDescriptor;    /* PUSB_CONFIGURATION_... */
	PVOID                ConfigurationHandle; /* USBD_CONFIGURATION_HANDLE */
	PVOID                InterfaceInfo;       /* PUSBD_INTERFACE_INFO... */
	PVOID                InterruptPipe;       /* USBD_PIPE_HANDLE        */

	/* The device interface this adapter is published under. */
	UNICODE_STRING       InterfaceName;
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
#define ADAPTOID_STOP_REASON_POWER  0x20

/*
 * Where an IRP's queue link lives. The DDK puts it in Tail.Overlay; the
 * harness has a plain member. One accessor so wdm.c never names either.
 */
#ifdef ADAPTOID_USERMODE
#define ADAPTOID_IRP_LIST_ENTRY(Irp)  (&(Irp)->ListEntry)
#define ADAPTOID_IRP_FROM_ENTRY(e)    \
    ((PIRP)((char *)(e) - (char *)&(((PIRP)0)->ListEntry)))
#define ADAPTOID_IRP_CONTEXT(Irp)     ((PVOID)(Irp)->DriverContext)
/* METHOD_BUFFERED gives one buffer for both directions; the DDK keeps it
 * inside a union the harness has no reason to reproduce. */
#define ADAPTOID_IRP_BUFFER(Irp)      ((Irp)->SystemBuffer)
#else
#define ADAPTOID_IRP_LIST_ENTRY(Irp)  (&(Irp)->Tail.Overlay.ListEntry)
#define ADAPTOID_IRP_FROM_ENTRY(e)    \
    CONTAINING_RECORD((e), IRP, Tail.Overlay.ListEntry)
/* Four pointers a driver may use while it owns the IRP. */
#define ADAPTOID_IRP_CONTEXT(Irp)     ((PVOID)(Irp)->Tail.Overlay.DriverContext)
#define ADAPTOID_IRP_BUFFER(Irp)      ((Irp)->AssociatedIrp.SystemBuffer)
#endif

/*
 * The Type3InputBuffer VALUE. The HID minidriver codes pass a small integer
 * where the DDK declares a pointer, so it is cast rather than dereferenced -
 * and the cast goes through a pointer-sized integer so that it is the same
 * on 32- and 64-bit.
 */
#define ADAPTOID_TYPE3_ARG(sl) \
    ((u32)(ULONG_PTR)(sl)->Parameters.DeviceIoControl.Type3InputBuffer)

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
