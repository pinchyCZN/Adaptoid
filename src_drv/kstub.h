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
#define STATUS_INVALID_DEVICE_STATE     ((NTSTATUS)0xC0000184L)
#define STATUS_DEVICE_NOT_READY         ((NTSTATUS)0xC00000A3L)

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
#define IRP_MJ_SYSTEM_CONTROL           0x17
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

/*
 * Pointer-sized, so that a value smuggled through a PVOID survives the
 * round trip on both 32- and 64-bit. The DDK spells it the same way.
 */
#if defined(_WIN64)
typedef unsigned __int64 ULONG_PTR;
#else
typedef unsigned long    ULONG_PTR;
#endif

typedef ULONG KSPIN_LOCK, *PKSPIN_LOCK;
typedef ULONG KIRQL;

typedef struct _KEVENT  { LONG Signalled; } KEVENT,  *PKEVENT;
/*
 * Shaped like the DDK's, because KeSetTimer takes one BY VALUE and a
 * LONGLONG in its place compiles in the harness and then fails against the
 * real header - which is how this was found.
 */
typedef union _LARGE_INTEGER {
	struct { ULONG LowPart; LONG HighPart; } u;
	LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct _KTIMER  { ULONGLONG Due;  } KTIMER,  *PKTIMER;

struct _KDPC;
typedef void (NTAPI *PKDEFERRED_ROUTINE)(struct _KDPC *, PVOID, PVOID, PVOID);
typedef struct _KDPC {
	PKDEFERRED_ROUTINE Routine;
	PVOID              Context;
	LONG               Queued;
} KDPC, *PKDPC;

/* The original guards its control-device singleton with a fast mutex. The
 * harness is single-threaded, so this is a counter it can assert on. */
typedef struct _FAST_MUTEX { LONG Held; } FAST_MUTEX, *PFAST_MUTEX;

/*
 * A KMUTEX waits at PASSIVE_LEVEL where a FAST_MUTEX raises to APC_LEVEL.
 * See the note in wdm.c on why that matters.
 *
 * SHAPED LIKE A KEVENT ON PURPOSE. The harness waits on it through the
 * same KeWaitForSingleObject that serves events, and that stub reads the
 * first LONG as "is it signalled". An unheld mutex must therefore read as
 * signalled, or every acquisition reports that the driver would have
 * blocked forever.
 */
typedef struct _KMUTEX { LONG Signalled; } KMUTEX, *PKMUTEX;

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

	/*
	 * THE SYSTEM-TO-DEVICE POWER MAP, indexed by system power state. This
	 * is what AdaptoidDeviceStateFor reads, and the reason the driver has
	 * to fetch capabilities at all - without it there is no way to know
	 * which D-state a given sleep state should mean.
	 */
	/* PowerSystemUnspecified .. PowerSystemMaximum. The literal rather
	 * than ADAPTOID_SYSTEM_STATE_MAX because wdm.h defines that from
	 * PowerSystemMaximum, and wdm.h includes this file first. */
	ULONG  DeviceState[7];
	ULONG  DeviceWake;      /* deepest D-state the device can wake from */
	ULONG  SystemWake;
} DEVICE_CAPABILITIES, *PDEVICE_CAPABILITIES;

/* Power minor function codes. */
#define IRP_MN_WAIT_WAKE                0x00
#define IRP_MN_SET_POWER                0x02

/*
 * The two Power.Type values. An ENUM, not two #defines, because the DDK
 * spells them this way and a macro named DevicePowerState would rewrite the
 * device-extension member of the same name - which is exactly what happened
 * the first time these were written as macros.
 */
typedef enum _POWER_STATE_TYPE {
	SystemPowerState = 0,
	DevicePowerState = 1
} POWER_STATE_TYPE;

/*
 * The D and S state enumerations, and the union that carries either. Shaped
 * exactly as the DDK shapes them, because PREQUEST_POWER_COMPLETE takes a
 * POWER_STATE by value and a mismatch there is a silent ABI difference
 * between the two builds rather than a compile error in one of them.
 */
typedef enum _DEVICE_POWER_STATE {
	PowerDeviceUnspecified = 0,
	PowerDeviceD0,
	PowerDeviceD1,
	PowerDeviceD2,
	PowerDeviceD3,
	PowerDeviceMaximum
} DEVICE_POWER_STATE;

typedef enum _SYSTEM_POWER_STATE {
	PowerSystemUnspecified = 0,
	PowerSystemWorking,
	PowerSystemSleeping1,
	PowerSystemSleeping2,
	PowerSystemSleeping3,
	PowerSystemHibernate,
	PowerSystemShutdown,
	PowerSystemMaximum
} SYSTEM_POWER_STATE;

typedef union _POWER_STATE {
	SYSTEM_POWER_STATE SystemState;
	DEVICE_POWER_STATE DeviceState;
} POWER_STATE, *PPOWER_STATE;

typedef struct _IO_STACK_LOCATION {
	UCHAR        MajorFunction;
	UCHAR        MinorFunction;
	PFILE_OBJECT FileObject;
	union {
		struct {
			ULONG OutputBufferLength;
			ULONG InputBufferLength;
			ULONG IoControlCode;
			/*
			 * METHOD_NEITHER's input pointer. The HID minidriver
			 * codes abuse it as a VALUE - a string index or a
			 * collection number - rather than a pointer, which is
			 * why ADAPTOID_TYPE3_ARG casts rather than reads.
			 */
			PVOID Type3InputBuffer;
		} DeviceIoControl;
		struct {
			PDEVICE_CAPABILITIES Capabilities;
		} DeviceCapabilities;
		struct {
			ULONG Length;
		} Read;
		struct {
			ULONG Length;
		} Write;
		/*
		 * Shaped like the DDK's, FIELD FOR FIELD. Type selects which
		 * arm of the State union is meaningful, and State is a union
		 * rather than a ULONG - getting that wrong compiles cleanly in
		 * the harness and then fails to build against the real DDK,
		 * which is how this was found.
		 */
		struct {
			ULONG            SystemContext;
			POWER_STATE_TYPE Type;
			POWER_STATE      State;
			ULONG            ShutdownType;
		} Power;
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
	/* METHOD_NEITHER hands the caller's output buffer over directly. */
	PVOID              UserBuffer;
	PIO_STACK_LOCATION CurrentStackLocation;
	/* The harness pre-builds both locations; the real thing walks an
	 * array and the macros below hide the difference. */
	PIO_STACK_LOCATION NextStackLocation;
	/* The DDK keeps this in Tail.Overlay; see ADAPTOID_IRP_LIST_ENTRY. */
	LIST_ENTRY         ListEntry;
	PVOID              CancelRoutine;
	BOOLEAN            Cancel;
	BOOLEAN            PendingReturned;
	KIRQL              CancelIrql;
	/*
	 * The DDK's Tail.Overlay.DriverContext - four pointers a driver may
	 * use while it owns the IRP. wdm.c parks a core_notify_waiter here so
	 * that queueing a notification cannot fail for want of memory.
	 */
	PVOID              DriverContext[4];
} IRP, *PIRP;

/*
 * The stack-location accessors, in the DDK's spelling. Real drivers use
 * these rather than touching Tail.Overlay, which is exactly why wdm.c can
 * be written once for both builds.
 */
#define IoGetCurrentIrpStackLocation(Irp)   ((Irp)->CurrentStackLocation)
#define IoGetNextIrpStackLocation(Irp)      ((Irp)->NextStackLocation)
#define IoMarkIrpPending(Irp)               ((Irp)->PendingReturned = TRUE)

void IoSetCancelRoutine(PIRP Irp, PVOID Routine);
void IoAcquireCancelSpinLock(KIRQL *Irql);
void IoReleaseCancelSpinLock(KIRQL Irql);

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
PVOID    InterlockedExchangePointer(PVOID volatile *Target,
                                    PVOID Value);

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

/* The DDK spelling. In the driver build ntddk.h supplies it. */
#define UNREFERENCED_PARAMETER(P)   ((void)(P))

void     IoCompleteRequest(PIRP Irp, CHAR PriorityBoost);

/*
 * Passing an IRP down. The harness records the call rather than making one,
 * which is what lets the PnP switch be tested without a device stack.
 */
NTSTATUS IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
void     IoCopyCurrentIrpStackLocationToNext(PIRP Irp);
void     IoSkipCurrentIrpStackLocation(PIRP Irp);

/*
 * Power. PoCallDriver and PoStartNextPowerIrp are separate calls from the
 * IRP ones on purpose: the ORDER of them is the protocol - every path must
 * call PoStartNextPowerIrp before PoCallDriver - and the harness checks that
 * by recording both.
 */
typedef void (NTAPI *PREQUEST_POWER_COMPLETE)(PDEVICE_OBJECT DeviceObject,
                                        UCHAR MinorFunction,
                                        POWER_STATE PowerState,
                                        PVOID Context, PIO_STATUS_BLOCK Io);

void     PoStartNextPowerIrp(PIRP Irp);
NTSTATUS PoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS PoRequestPowerIrp(PDEVICE_OBJECT DeviceObject, UCHAR MinorFunction,
                           POWER_STATE PowerState,
                           PREQUEST_POWER_COMPLETE Complete,
                           PVOID Context, PIRP *Irp);

/* Completion routines, and the three flags that say when to run one. */
typedef NTSTATUS (NTAPI *PIO_COMPLETION_ROUTINE)(PDEVICE_OBJECT DeviceObject,
                                           PIRP Irp, PVOID Context);

void IoSetCompletionRoutine(PIRP Irp, PIO_COMPLETION_ROUTINE Routine,
                            PVOID Context, BOOLEAN OnSuccess,
                            BOOLEAN OnError, BOOLEAN OnCancel);

/* Pool types. The driver only ever asks for non-paged. */
typedef enum _POOL_TYPE { NonPagedPool = 0, PagedPool = 1,
                          NonPagedPoolNx = 512 } POOL_TYPE;

/* Timers and DPCs, for the script scheduler. */
void    KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE Routine, PVOID Context);
void    KeInitializeTimer(PKTIMER Timer);
BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc);
BOOLEAN KeCancelTimer(PKTIMER Timer);
void    KeFlushQueuedDpcs(void);

/* The control device object. */
void     ExInitializeFastMutex(PFAST_MUTEX Mutex);
void     ExAcquireFastMutex(PFAST_MUTEX Mutex);
void     ExReleaseFastMutex(PFAST_MUTEX Mutex);
void     KeInitializeMutex(PKMUTEX Mutex, ULONG Level);
LONG     KeReleaseMutex(PKMUTEX Mutex, BOOLEAN Wait);

#define FILE_DEVICE_UNKNOWN 0x00000022
#define DO_BUFFERED_IO      0x00000004
#define DO_POWER_PAGABLE    0x00002000
#define DO_DEVICE_INITIALIZING 0x00000080

void     RtlInitUnicodeString(PUNICODE_STRING Target, PCWSTR Source);
void     RtlZeroMemory(PVOID Destination, ULONG_PTR Length);
void     RtlCopyMemory(PVOID Destination, const void *Source,
                       ULONG_PTR Length);
NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG ExtensionSize,
                        PUNICODE_STRING Name, ULONG DeviceType,
                        ULONG Characteristics, BOOLEAN Exclusive,
                        PDEVICE_OBJECT *DeviceObject);
void     IoDeleteDevice(PDEVICE_OBJECT DeviceObject);
NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING Link, PUNICODE_STRING Target);
NTSTATUS IoDeleteSymbolicLink(PUNICODE_STRING Link);

#endif /* ADAPTOID_KSTUB_H */
