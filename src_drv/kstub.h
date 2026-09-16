/*
 * kstub.h - a fake kernel ABI, just large enough to compile wdm.c in user mode.
 *
 * Included by wdm.h when ADAPTOID_USERMODE is defined. The implementations
 * live in harness.c. This is NOT an emulation of Windows: it is the smallest
 * set of types and entry points that lets the OS-facing layer compile and be
 * driven from a console program.
 *
 * Scope rule: declare what a caller actually needs, nothing more. The 2001
 * driver imports 49 kernel functions, but the code worth testing reaches only
 * 18 of them. Add to this file when a caller appears, not in anticipation.
 *
 * Deliberately does NOT include <windows.h>. Nothing in the harness may, or
 * these definitions collide with the real ones.
 */
#ifndef ADAPTOID_KSTUB_H
#define ADAPTOID_KSTUB_H

/* ---- scalar types ------------------------------------------------- */

typedef long                NTSTATUS;
typedef unsigned long       ULONG;
typedef long                LONG;
typedef unsigned short      USHORT;
typedef unsigned char       UCHAR;
typedef unsigned char       BOOLEAN;
typedef void               *PVOID;
typedef unsigned short      WCHAR;
typedef WCHAR              *PWSTR;
typedef const WCHAR        *PCWSTR;
typedef char                CHAR;

#if defined(_MSC_VER)
typedef unsigned __int64    ULONGLONG;
typedef signed   __int64    LONGLONG;
#else
typedef unsigned long long  ULONGLONG;
typedef signed   long long  LONGLONG;
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#define IN
#define OUT
#define OPTIONAL

/*
 * The kernel calls DriverEntry and every dispatch routine as __stdcall. On x86
 * that is not cosmetic - a cdecl callee would leave the caller's arguments on
 * the kernel stack. Keep the convention identical in both builds so the
 * harness exercises the same signatures the kernel will use.
 */
#ifndef NTAPI
#define NTAPI __stdcall
#endif

/* ---- status codes ------------------------------------------------- */

#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000L)
#define STATUS_PENDING                  ((NTSTATUS)0x00000103L)
#define STATUS_DEVICE_BUSY              ((NTSTATUS)0x80000011L)
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001L)
#define STATUS_NOT_IMPLEMENTED          ((NTSTATUS)0xC0000002L)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000DL)
#define STATUS_NO_SUCH_DEVICE           ((NTSTATUS)0xC000000EL)
#define STATUS_INVALID_DEVICE_REQUEST   ((NTSTATUS)0xC0000010L)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023L)
#define STATUS_INSUFFICIENT_RESOURCES   ((NTSTATUS)0xC000009AL)
#define STATUS_NOT_SUPPORTED            ((NTSTATUS)0xC00000BBL)
#define STATUS_DELETE_PENDING           ((NTSTATUS)0xC0000056L)
#define STATUS_CANCELLED                ((NTSTATUS)0xC0000120L)

#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)

/* ---- IRP major function codes ------------------------------------- */

#define IRP_MJ_CREATE                   0x00
#define IRP_MJ_CLOSE                    0x02
#define IRP_MJ_READ                     0x03
#define IRP_MJ_WRITE                    0x04
#define IRP_MJ_DEVICE_CONTROL           0x0e
#define IRP_MJ_INTERNAL_DEVICE_CONTROL  0x0f
#define IRP_MJ_CLEANUP                  0x12
#define IRP_MJ_POWER                    0x16
#define IRP_MJ_PNP                      0x1b
#define IRP_MJ_MAXIMUM_FUNCTION         0x1b

/* ---- structures --------------------------------------------------- */

typedef struct _LIST_ENTRY {
	struct _LIST_ENTRY *Flink;
	struct _LIST_ENTRY *Blink;
} LIST_ENTRY, *PLIST_ENTRY;

typedef struct _UNICODE_STRING {
	USHORT  Length;
	USHORT  MaximumLength;
	PWSTR   Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef ULONG KSPIN_LOCK, *PKSPIN_LOCK;
typedef ULONG KIRQL;

typedef struct _KEVENT  { LONG Signalled; } KEVENT,  *PKEVENT;
typedef struct _KTIMER  { ULONGLONG Due;  } KTIMER,  *PKTIMER;

/*
 * DEVICE_OBJECT and IRP are opaque to the skeleton. Only the members the
 * driver layer actually touches are declared; the rest is deliberately absent
 * so that reaching for an undeclared field is a compile error rather than a
 * silent difference from the real kernel.
 */
typedef struct _DEVICE_OBJECT {
	PVOID                   DeviceExtension;
	struct _DEVICE_OBJECT  *NextDevice;
	ULONG                   Flags;
} DEVICE_OBJECT, *PDEVICE_OBJECT;

/*
 * The IRP stack, enough of it for the dispatch triage and the PnP switch.
 * Field names and the shape of the Parameters union match the DDK so the
 * same source compiles both ways; what is missing is everything nothing
 * reaches yet.
 */
typedef struct _FILE_OBJECT {
	UNICODE_STRING FileName;
} FILE_OBJECT, *PFILE_OBJECT;

/*
 * The bitfields are declared in the DDK's order because their POSITIONS are
 * the contract: the 2001 driver ORs 0x210 into the dword at offset 4, and
 * 0x210 is bit 4 plus bit 9 - Removable and SurpriseRemovalOK. Setting them
 * by name says the same thing and survives a recompile.
 */
typedef struct _DEVICE_CAPABILITIES {
	USHORT Size;
	USHORT Version;
	ULONG  DeviceD1:1;
	ULONG  DeviceD2:1;
	ULONG  LockSupported:1;
	ULONG  EjectSupported:1;
	ULONG  Removable:1;             /* 0x010 */
	ULONG  DockDevice:1;
	ULONG  UniqueID:1;
	ULONG  SilentInstall:1;
	ULONG  RawDeviceOK:1;
	ULONG  SurpriseRemovalOK:1;     /* 0x200 */
	ULONG  WakeFromD0:1;
} DEVICE_CAPABILITIES, *PDEVICE_CAPABILITIES;

typedef struct _IO_STACK_LOCATION {
	UCHAR        MajorFunction;
	UCHAR        MinorFunction;
	PFILE_OBJECT FileObject;
	union {
		struct {
			ULONG OutputBufferLength;
			ULONG InputBufferLength;
			ULONG IoControlCode;
		} DeviceIoControl;
		struct {
			PDEVICE_CAPABILITIES Capabilities;
		} DeviceCapabilities;
	} Parameters;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;

/* PnP minor function codes, the ones the dispatcher names. */
#define IRP_MN_START_DEVICE             0x00
#define IRP_MN_QUERY_REMOVE_DEVICE      0x01
#define IRP_MN_REMOVE_DEVICE            0x02
#define IRP_MN_CANCEL_REMOVE_DEVICE     0x03
#define IRP_MN_STOP_DEVICE              0x04
#define IRP_MN_QUERY_STOP_DEVICE        0x05
#define IRP_MN_CANCEL_STOP_DEVICE       0x06
#define IRP_MN_QUERY_CAPABILITIES       0x09
#define IRP_MN_SURPRISE_REMOVAL         0x17

/* Shaped like the DDK's, so wdm.c reaches IoStatus.Status in both builds. */
typedef struct _IO_STATUS_BLOCK {
	NTSTATUS    Status;
	ULONG       Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _IRP {
	IO_STATUS_BLOCK    IoStatus;
	PVOID              SystemBuffer;
	PIO_STACK_LOCATION CurrentStackLocation;
	/* The harness pre-builds both locations; the real thing walks an
	 * array and the macros below hide the difference. */
	PIO_STACK_LOCATION NextStackLocation;
	/* The DDK keeps this in Tail.Overlay; see ADAPTOID_IRP_LIST_ENTRY. */
	LIST_ENTRY         ListEntry;
	PVOID              CancelRoutine;
	BOOLEAN            Cancel;
} IRP, *PIRP;

/*
 * The stack-location accessors, in the DDK's spelling. Real drivers use
 * these rather than touching Tail.Overlay, which is exactly why wdm.c can
 * be written once for both builds.
 */
#define IoGetCurrentIrpStackLocation(Irp)   ((Irp)->CurrentStackLocation)
#define IoGetNextIrpStackLocation(Irp)      ((Irp)->NextStackLocation)

struct _DRIVER_OBJECT;

/* Typed exactly as the DDK types them, so the dispatch table needs no casts
 * and a wrong handler signature is a compile error in both builds. */
typedef NTSTATUS (NTAPI *PDRIVER_DISPATCH)(PDEVICE_OBJECT, PIRP);
typedef void     (NTAPI *PDRIVER_UNLOAD)(struct _DRIVER_OBJECT *);

typedef struct _DRIVER_OBJECT {
	PDEVICE_OBJECT      DeviceObject;
	PVOID               DriverExtension;
	PDRIVER_DISPATCH    MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
	PDRIVER_UNLOAD      DriverUnload;
} DRIVER_OBJECT, *PDRIVER_OBJECT;

/* ---- HID minidriver registration ---------------------------------- */

#define HID_REVISION 0x00000001

typedef struct _HID_MINIDRIVER_REGISTRATION {
	ULONG           Revision;
	PDRIVER_OBJECT  DriverObject;
	PUNICODE_STRING RegistryPath;
	ULONG           DeviceExtensionSize;
	BOOLEAN         DevicesArePolled;
	UCHAR           Reserved[3];
} HID_MINIDRIVER_REGISTRATION, *PHID_MINIDRIVER_REGISTRATION;

/*
 * The public half of the extension hidclass creates. Reaching the driver's own
 * state is a two-step hop through MiniDeviceExtension; see
 * ../docs/driver-structures.txt section 1.
 */
typedef struct _HID_DEVICE_EXTENSION {
	PDEVICE_OBJECT  PhysicalDeviceObject;
	PDEVICE_OBJECT  NextDeviceObject;
	PVOID           MiniDeviceExtension;
} HID_DEVICE_EXTENSION, *PHID_DEVICE_EXTENSION;

NTSTATUS HidRegisterMinidriver(PHID_MINIDRIVER_REGISTRATION Registration);

/* ---- the kernel subset the core closure reaches -------------------- */

PVOID    ExAllocatePoolWithTag(ULONG PoolType, ULONG NumberOfBytes, ULONG Tag);
void     ExFreePool(PVOID P);

LONG     InterlockedIncrement(LONG volatile *Addend);
LONG     InterlockedDecrement(LONG volatile *Addend);
LONG     InterlockedExchange(LONG volatile *Target, LONG Value);

void     KeInitializeSpinLock(PKSPIN_LOCK SpinLock);
KIRQL    KfAcquireSpinLock(PKSPIN_LOCK SpinLock);
void     KfReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL NewIrql);

/*
 * THE PORTABLE SPELLING, and the only one new code should use.
 *
 * KfAcquireSpinLock is an x86-only fastcall export; on x64 the symbol does
 * not exist at all and the link fails. The DDK's two-argument
 * KeAcquireSpinLock is a macro over Kf on x86 and a real function on x64,
 * so it is what compiles on both. These definitions match the x86 DDK ones
 * exactly; the real headers supply their own.
 */
typedef KIRQL *PKIRQL;
#define KeAcquireSpinLock(l, p)     (*(p) = KfAcquireSpinLock(l))
#define KeReleaseSpinLock(l, i)     KfReleaseSpinLock((l), (i))

/* The two enumerations the event and wait calls take. Values match the DDK
 * so the same source compiles against the real headers. */
#define NotificationEvent       0
#define SynchronizationEvent    1
#define Executive               0
#define KernelMode              0

void     KeInitializeEvent(PKEVENT Event, ULONG Type, BOOLEAN State);
LONG     KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait);

/*
 * Waiting is where a user-mode harness and a kernel part company: there is no
 * other thread to signal the event. The stub asserts the event is ALREADY
 * signalled and returns, which turns "this code would have blocked forever"
 * into a test failure instead of a hang.
 */
NTSTATUS KeWaitForSingleObject(PVOID Object, ULONG WaitReason,
                               ULONG WaitMode, BOOLEAN Alertable,
                               PVOID Timeout);


/*
 * The single most valuable stub. Backed by a variable the harness advances by
 * hand, which makes the script scheduler, the effect ring and the keep-alive
 * deterministic and steppable - something no VM can offer.
 */
ULONGLONG KeQueryInterruptTime(void);

/* Completing a request is the one kernel call the transport cannot avoid. */
#define IO_NO_INCREMENT 0

void     IoCompleteRequest(PIRP Irp, CHAR PriorityBoost);

/*
 * Passing an IRP down. The harness records the call rather than making one,
 * which is what lets the PnP switch be tested without a device stack.
 */
NTSTATUS IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
void     IoCopyCurrentIrpStackLocationToNext(PIRP Irp);
void     IoSkipCurrentIrpStackLocation(PIRP Irp);

#endif /* ADAPTOID_KSTUB_H */
