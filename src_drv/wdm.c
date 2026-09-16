/*
 * wdm.c - the OS-facing layer of wishk300.
 *
 * COMPLETE except for one thing, said plainly: THE USB LAYER HAS NEVER
 * TALKED TO A DEVICE. Descriptor fetch, select configuration, the two
 * asynchronous transfer types, abort and the port IOCTLs are written from a
 * specification and checked only by compiling in all four configurations.
 * Everything else here has a test group behind it.
 *
 * The references each part was written from:
 *   ../docs/driver-lifecycle.txt   DriverEntry, AddDevice, PnP, power
 *   ../docs/ioctl-surface.txt      the private IOCTL surface
 *   ../docs/usb-transport.txt      the vendor protocol and the poll engine
 *   ../docs/hid-descriptor.txt     the minidriver contract and descriptors
 *   ../docs/command-block.txt      the SDK's read/write channel
 */

#include "wdm.h"

/*
 * DriverEntry runs once and is never needed again, so it belongs in the
 * discardable INIT section - the kernel reclaims that memory after load. The
 * linker is told to make INIT discardable via /SECTION:INIT,d. Meaningless in
 * the harness build, where there are no sections to discard.
 */
#ifndef ADAPTOID_USERMODE
#pragma alloc_text(INIT, DriverEntry)
#endif

/* ------------------------------------------------------------------ */
/* Report sink - the driver side of the core seam                      */
/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/* Load and unload                                                     */
/* ------------------------------------------------------------------ */

/*
 * THE CONTROL DEVICE SINGLETON. Declared here rather than beside its own
 * code because DriverEntry has to initialise the mutex before anything can
 * use it - see the comment there.
 */
static PDEVICE_OBJECT g_ControlDevice;
static LONG           g_ControlRefCount;
static FAST_MUTEX     g_ControlMutex;

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject,
                           PUNICODE_STRING RegistryPath)
{
	HID_MINIDRIVER_REGISTRATION reg;
	ULONG                       i;
	PDRIVER_DISPATCH           *mj;
	NTSTATUS                    status;

	/*
	 * Register as a HID minidriver so hidclass stacks on top of us. This is
	 * the whole reason the original can present a composite keyboard, mouse
	 * and joystick device; see ../docs/replacement-architecture.txt section 2.
	 */
	mj = DriverObject->MajorFunction;

	for (i = 0; i < IRP_MJ_MAXIMUM_FUNCTION + 1; i++) {
		mj[i] = 0;
	}

	/*
	 * STEP 1: the MINIDRIVER's own handlers. HidRegisterMinidriver captures
	 * these and calls them from inside hidclass, so they are the entry
	 * points for traffic hidclass has already triaged - and the private
	 * channel reuses the create and close ones rather than duplicating
	 * them.
	 */
	mj[IRP_MJ_CREATE]                  = AdaptoidChannelCreate;
	mj[IRP_MJ_CLOSE]                   = AdaptoidChannelClose;
	mj[IRP_MJ_INTERNAL_DEVICE_CONTROL] = AdaptoidIntDeviceControl;
	mj[IRP_MJ_DEVICE_CONTROL]          = AdaptoidPassThroughDeviceControl;
	/*
	 * SYSTEM_CONTROL GETS THE SAME PASS-THROUGH. The original installs
	 * drv_DispatchDeviceControl at both 0x0E and 0x17; WMI requests have
	 * to reach the bus driver or the stack answers them itself and
	 * confuses whoever asked.
	 */
	mj[IRP_MJ_SYSTEM_CONTROL]          = AdaptoidPassThroughDeviceControl;

	/*
	 * THE CONTROL DEVICE'S MUTEX MUST BE INITIALISED HERE. A zeroed
	 * FAST_MUTEX is not an unheld one - its count reads as already taken
	 * and its event is unsignalled - so the first adapter to arrive would
	 * block in AdaptoidCreateControlDevice and never come back. The
	 * original does the same thing inline at 000115fd.
	 *
	 * The harness cannot see this: its fast mutex is a counter.
	 */
	ExInitializeFastMutex(&g_ControlMutex);
	mj[IRP_MJ_PNP]                     = AdaptoidPnp;
	mj[IRP_MJ_POWER]                   = AdaptoidPower;
	DriverObject->DriverUnload         = AdaptoidUnload;
	/*
	 * WITHOUT THIS NOTHING EVER ENUMERATES. hidclass calls AddDevice
	 * through the driver extension, and a null there means the driver
	 * loads and never sees a device.
	 */
	AdaptoidSetAddDevice(DriverObject, AdaptoidAddDevice);


	reg.Revision            = HID_REVISION;
	reg.DriverObject        = DriverObject;
	reg.RegistryPath        = RegistryPath;
	reg.DeviceExtensionSize = (ULONG)sizeof(ADAPTOID_DEVEXT);
	reg.DevicesArePolled    = 0;
	reg.Reserved[0]         = 0;
	reg.Reserved[1]         = 0;
	reg.Reserved[2]         = 0;

	status = HidRegisterMinidriver(&reg);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	/*
	 * STEP 2: hidclass has just OVERWRITTEN the dispatch table with its own
	 * entry points. Save them.
	 */
	AdaptoidSavedDispatch.Create        = mj[IRP_MJ_CREATE];
	AdaptoidSavedDispatch.Cleanup       = mj[IRP_MJ_CLEANUP];
	AdaptoidSavedDispatch.Close         = mj[IRP_MJ_CLOSE];
	AdaptoidSavedDispatch.Read          = mj[IRP_MJ_READ];
	AdaptoidSavedDispatch.Write         = mj[IRP_MJ_WRITE];
	AdaptoidSavedDispatch.DeviceControl = mj[IRP_MJ_DEVICE_CONTROL];
	AdaptoidSavedDispatch.Pnp           = mj[IRP_MJ_PNP];
	AdaptoidSavedDispatch.Power         = mj[IRP_MJ_POWER];

	/*
	 * STEP 3: install the triage wrappers ON TOP. Every request now lands
	 * here first, and anything that is not ours is handed straight to the
	 * pointer saved above. That is the whole trick: one driver object, three
	 * kinds of client, and hidclass still owns the HID device.
	 */
	mj[IRP_MJ_CREATE]         = AdaptoidCreate;
	mj[IRP_MJ_CLEANUP]        = AdaptoidCleanup;
	mj[IRP_MJ_CLOSE]          = AdaptoidClose;
	mj[IRP_MJ_READ]           = AdaptoidRead;
	mj[IRP_MJ_WRITE]          = AdaptoidWrite;
	mj[IRP_MJ_DEVICE_CONTROL] = AdaptoidDeviceControl;
	mj[IRP_MJ_PNP]            = AdaptoidPnpTriage;
	mj[IRP_MJ_POWER]          = AdaptoidPowerTriage;

	return STATUS_SUCCESS;
}

/*
 * EMPTY, AND CORRECTLY SO - drv_Unload at 000117e0 is a single RET.
 *
 * DriverEntry allocates nothing. It fills a dispatch table, registers with
 * hidclass, and initialises statics; there is no pool, no device object and
 * no work item to give back. The control device is created by the first
 * AddDevice and deleted by the last removal, and the I/O manager will not
 * unload a driver that still owns a device object, so by the time this runs
 * there is provably nothing left.
 *
 * Kept rather than left NULL because DriverUnload being non-NULL is what
 * tells the system the driver MAY be unloaded at all.
 */
void NTAPI AdaptoidUnload(PDRIVER_OBJECT DriverObject)
{
	UNREFERENCED_PARAMETER(DriverObject);
}

/*
 * The per-adapter instance number, and the stick position virtual mode
 * reports. drv_AddDevice assigns it as (Interlocked++ % 1100) + 50, which
 * keeps it inside the +/-1200 stick range on purpose - see core.h.
 */
static LONG g_InstanceCounter;

#define ADAPTOID_INSTANCE_SPAN  1100
#define ADAPTOID_INSTANCE_BASE  50

/*
 * One adapter has arrived.
 *
 * hidclass has already created the device object and allocated our
 * extension; what is left is to make that extension usable, join the
 * subsystems to each other, and publish the device.
 *
 * POLLING STARTS STOPPED. PollStopMask is seeded with the PnP reason, so
 * nothing is submitted until IRP_MN_START_DEVICE clears it - a read against
 * a device the bus has not started yet is a bugcheck waiting to happen.
 */
NTSTATUS NTAPI AdaptoidAddDevice(PDRIVER_OBJECT DriverObject,
                                 PDEVICE_OBJECT FunctionalDeviceObject)
{
	PHID_DEVICE_EXTENSION hidext;
	PADAPTOID_DEVEXT      devext;
	PADAPTOID_CDO_EXT     cdo;
	NTSTATUS              status;
	ULONG                 i;

	if (FunctionalDeviceObject == NULL) {
		return STATUS_INVALID_PARAMETER;
	}

	/* The two-level hop: hidclass owns DeviceExtension, we own
	 * MiniDeviceExtension. Confusing the two makes every offset wrong. */
	hidext = (PHID_DEVICE_EXTENSION)FunctionalDeviceObject->DeviceExtension;
	if (hidext == NULL) {
		return STATUS_UNSUCCESSFUL;
	}
	devext = (PADAPTOID_DEVEXT)hidext->MiniDeviceExtension;
	if (devext == NULL) {
		return STATUS_UNSUCCESSFUL;
	}

	RtlZeroMemory(devext, sizeof(*devext));
	devext->Self                 = FunctionalDeviceObject;
	devext->NextDeviceObject     = hidext->NextDeviceObject;
	devext->PhysicalDeviceObject = hidext->PhysicalDeviceObject;
	devext->Started              = 0;
	devext->DevicePowerState     = ADAPTOID_POWER_D0;

	/* Every lock, list head and remove lock. */
	AdaptoidDevExtInit(devext);

	/* Nothing may poll until PnP says start. */
	devext->PollStopMask = ADAPTOID_STOP_REASON_PNP;

	KeInitializeDpc(&devext->ScriptDpc, AdaptoidScriptDpc, devext);
	KeInitializeTimer(&devext->ScriptTimer);
	/* Latched negative: a script cannot own the stick until something
	 * lifts it. See AdaptoidScriptDpc. */
	devext->ScriptDepth = -1;

	devext->PollWorkItem = AdaptoidAllocateWorkItem(FunctionalDeviceObject);
	if (devext->PollWorkItem == NULL) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	/* THE WIRING, and the only reason any of the rest does anything. */
	AdaptoidWireDevice(devext);

	/*
	 * The HID personality is a single registry DWORD, read once per
	 * arrival. Changing it needs a replug; it is not renegotiated.
	 */
	devext->Core.devices_mask =
	        AdaptoidRegQueryDword(L"VirtualDevices", CORE_DEVICE_DEFAULT);

	AdaptoidRegisterDeviceInterface(devext);

	/*
	 * The capabilities, and the D-state to idle into. The original walks
	 * DeviceState[2..4] and keeps the LAST entry below D4, which is the
	 * deepest state the device can still be resumed from.
	 */
	devext->Capabilities.SurpriseRemovalOK = 1;
	devext->WakeIdleDeviceState = 0;
	for (i = 2; i <= 4 && i < ADAPTOID_SYSTEM_STATE_MAX; i++) {
		if (devext->Capabilities.DeviceState[i] < ADAPTOID_POWER_D3) {
			devext->WakeIdleDeviceState =
			        devext->Capabilities.DeviceState[i];
		}
	}

	status = AdaptoidCreateControlDevice(DriverObject);
	if (!NT_SUCCESS(status)) {
		AdaptoidFreeWorkItem(devext->PollWorkItem);
		devext->PollWorkItem = NULL;
		return status;
	}

	/*
	 * THE INSTANCE NUMBER IS ALSO A STICK POSITION. Virtual mode reports
	 * it as the Y axis, and control-device function 0x822 resolves a
	 * handle from the same value, so each adapter parks its stick at its
	 * own identity.
	 */
	devext->Core.instance_id =
	        (InterlockedIncrement(&g_InstanceCounter) %
	         ADAPTOID_INSTANCE_SPAN) + ADAPTOID_INSTANCE_BASE;

	cdo = AdaptoidControlDeviceExt();
	if (cdo != NULL) {
		core_registry_add(&cdo->Registry, &devext->Registration);
	}

	/*
	 * DO_POWER_PAGABLE, and DO_DEVICE_INITIALIZING cleared. hidclass
	 * created the object, so clearing the flag is still ours to do.
	 */
	FunctionalDeviceObject->Flags |= DO_POWER_PAGABLE;
	FunctionalDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
	return STATUS_SUCCESS;
}

/*
 * One adapter is leaving, for good.
 *
 * THE ORDER IS THE WHOLE OF IT. Take the device off the registry first so
 * nothing new can find it; stop the clock; cancel what is outstanding;
 * release what was allocated; and only then let the control device go,
 * because the notification posted by the un-registration has to have
 * somewhere to land.
 */
void AdaptoidUnwireDevice(PADAPTOID_DEVEXT DevExt)
{
	PADAPTOID_CDO_EXT cdo = AdaptoidControlDeviceExt();

	if (cdo != NULL) {
		core_registry_set_live(&cdo->Registry, &DevExt->Registration, 0);
		core_registry_remove(&cdo->Registry, &DevExt->Registration);
	}

	KeCancelTimer(&DevExt->ScriptTimer);
	core_sched_unload(&DevExt->Sched);

	AdaptoidCancelVendorRequest(DevExt);
	AdaptoidCancelPendingReads(DevExt);

	if (DevExt->PollWorkItem != NULL) {
		AdaptoidFreeWorkItem(DevExt->PollWorkItem);
		DevExt->PollWorkItem = NULL;
	}
	AdaptoidRegistryRemove(DevExt);
	AdaptoidReleaseControlDevice();
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */







/*
 * IRP_MJ_INTERNAL_DEVICE_CONTROL - THE HID MINIDRIVER CONTRACT.
 *
 * Installed before HidRegisterMinidriver, so hidclass.sys is the only
 * caller. Eight codes, specified in ../docs/hid-descriptor.txt section 7
 * and answered by core_hid_ioctl, except for the one that cannot be.
 *
 * THESE USE METHOD_NEITHER, which is why the buffer is Irp->UserBuffer and
 * not AssociatedIrp.SystemBuffer, and why the argument arrives as a value
 * stuffed into Type3InputBuffer rather than in an input buffer. hidclass is
 * a kernel caller, so the pointer needs no probing - but that is true only
 * because nothing else can reach this entry point.
 *
 * THE ENTRY GUARD refuses everything while the device is being torn down.
 * Note the status: STATUS_DELETE_PENDING, which is what 0xC0000056 is - an
 * earlier note here named it STATUS_DEVICE_NOT_CONNECTED, which is
 * 0xC000009D and a different thing.
 */
NTSTATUS NTAPI AdaptoidIntDeviceControl(PDEVICE_OBJECT DeviceObject,
                                        PIRP Irp)
{
	PADAPTOID_DEVEXT   dx = AdaptoidDevExtOf(DeviceObject);
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
	u32                info = 0;
	u32                st;
	ULONG              code;

	if (!AdaptoidIsDeviceReady(dx)) {
		AdaptoidCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
		return STATUS_DELETE_PENDING;
	}
	code = sl->Parameters.DeviceIoControl.IoControlCode;

	/*
	 * READ_REPORT IS THE ONE THAT KEEPS THE IRP. It parks on the report
	 * queue and is completed when a packet arrives, so it is handled here
	 * rather than in core_hid_ioctl - and it is bracketed by RemoveLockA,
	 * because the device must not finish removing while a read is parked.
	 */
	if (code == CORE_HID_IOC_READ_REPORT) {
		NTSTATUS status;

		if (dx->Started == 0) {
			AdaptoidCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
			return STATUS_DEVICE_NOT_READY;
		}
		status = AdaptoidLockAcquire(&dx->RemoveLockA);
		if (!NT_SUCCESS(status)) {
			AdaptoidCompleteIrp(Irp, status, 0);
			return status;
		}
		status = AdaptoidReadReport(dx, Irp);
		AdaptoidLockRelease(&dx->RemoveLockA);
		return status;
	}

	st = core_hid_ioctl(&dx->Core, code,
	                    (u8 *)Irp->UserBuffer,
	                    sl->Parameters.DeviceIoControl.OutputBufferLength,
	                    sl->Parameters.DeviceIoControl.InputBufferLength,
	                    ADAPTOID_TYPE3_ARG(sl),
	                    &info);
	AdaptoidCompleteIrp(Irp, (NTSTATUS)st, info);
	return (NTSTATUS)st;
}



/* ------------------------------------------------------------------ */
/* the remove lock                                                     */
/* ------------------------------------------------------------------ */

void AdaptoidLockInit(PADAPTOID_REMOVE_LOCK Lock)
{
	/* Starts at ONE, not zero. That initial reference is what
	 * AdaptoidLockReleaseAndWait drops, and it is why a device with no
	 * outstanding work does not signal its event the moment it is
	 * created. */
	Lock->IoCount = 1;
	Lock->Removed = 0;
	KeInitializeEvent(&Lock->RemoveEvent, NotificationEvent, FALSE);
}

NTSTATUS AdaptoidLockAcquire(PADAPTOID_REMOVE_LOCK Lock)
{
	InterlockedIncrement(&Lock->IoCount);
	if (Lock->Removed) {
		/* Put it straight back, and signal if that was the last one -
		 * the waiter may already be blocked. */
		if (InterlockedDecrement(&Lock->IoCount) == 0) {
			KeSetEvent(&Lock->RemoveEvent, 0, FALSE);
		}
		return STATUS_DELETE_PENDING;
	}
	return STATUS_SUCCESS;
}

void AdaptoidLockRelease(PADAPTOID_REMOVE_LOCK Lock)
{
	if (InterlockedDecrement(&Lock->IoCount) == 0) {
		KeSetEvent(&Lock->RemoveEvent, 0, FALSE);
	}
}

void AdaptoidLockReleaseAndWait(PADAPTOID_REMOVE_LOCK Lock)
{
	Lock->Removed = 1;
	AdaptoidLockRelease(Lock);      /* the caller's own reference */
	AdaptoidLockRelease(Lock);      /* the initial one from Init  */
	KeWaitForSingleObject(&Lock->RemoveEvent, Executive, KernelMode,
	                      FALSE, NULL);
}

/* ------------------------------------------------------------------ */
/* the vendor transport                                                */
/* ------------------------------------------------------------------ */

/*
 * Take the slot if it is free. Returns non-zero on success. A refusal is
 * ordinary: the caller records the work as deferred and the completion path
 * picks it up.
 */
int AdaptoidVendorTryClaim(PADAPTOID_DEVEXT DevExt)
{
	PADAPTOID_VENDOR_SLOT slot = &DevExt->Vendor;
	KIRQL irql;
	int got;

	KeAcquireSpinLock(&slot->Lock, &irql);
	got = (slot->State == ADAPTOID_SLOT_FREE);
	if (got) {
		slot->State      = ADAPTOID_SLOT_CLAIMED;
		slot->PendingIrp = NULL;
	}
	KeReleaseSpinLock(&slot->Lock, irql);
	return got;
}

/* The same, but the transfer's result is owed to an IRP. */
/*
 * NOT ON THE REPLACEMENT'S PATH, and kept deliberately.
 *
 * It claims the vendor slot ON BEHALF OF AN IRP, so the transfer can
 * complete asynchronously and the IRP be finished from the completion.
 * The original's raw vendor IOCTL does that and can therefore return
 * STATUS_PENDING.
 *
 * This driver answers the same IOCTL SYNCHRONOUSLY instead - see
 * AdaptoidIoctlVendor - because core_ioctl_vendor_fn is handed a buffer
 * and not an IRP, and device-control requests arrive at PASSIVE_LEVEL
 * where blocking is legal. A caller sees the same bytes; only the status
 * differs, never STATUS_PENDING.
 *
 * The function stays because it is a faithful port of a real original and
 * because the asynchronous shape is what a future caller would need.
 */
int AdaptoidVendorClaimForIrp(PADAPTOID_DEVEXT DevExt, PIRP Irp)
{
	PADAPTOID_VENDOR_SLOT slot = &DevExt->Vendor;
	KIRQL irql;
	int got;

	KeAcquireSpinLock(&slot->Lock, &irql);
	got = (slot->State == ADAPTOID_SLOT_FREE);
	if (got) {
		slot->State      = ADAPTOID_SLOT_CLAIMED;
		slot->PendingIrp = Irp;
	}
	KeReleaseSpinLock(&slot->Lock, irql);
	return got;
}

/* Give the slot back and complete whatever IRP was waiting on it. */
static void VendorFail(PADAPTOID_DEVEXT DevExt, NTSTATUS Status)
{
	PADAPTOID_VENDOR_SLOT slot = &DevExt->Vendor;
	KIRQL irql;
	PIRP  pending = NULL;

	KeAcquireSpinLock(&slot->Lock, &irql);
	if (slot->State == ADAPTOID_SLOT_CLAIMED) {
		pending          = slot->PendingIrp;
		slot->State      = ADAPTOID_SLOT_FREE;
		slot->PendingIrp = NULL;
	}
	KeReleaseSpinLock(&slot->Lock, irql);

	if (pending != NULL) {
		AdaptoidCompleteIrp(pending, Status, 0);
	}
}

/*
 * Build and submit a vendor control transfer.
 *
 * bmRequestType must be exactly 0x40 or 0xC0 - vendor OUT or vendor IN.
 * Nothing else is a request this device understands, and the byte is
 * CALLER-SUPPLIED on the raw passthrough IOCTL, so this is a validation of
 * untrusted input rather than an assertion about our own code.
 *
 * On success the remove lock stays held; AdaptoidVendorComplete drops it.
 *
 * DIVERGENCE, and it fixes a defect. drv_SendVendorRequest's failure path
 * calls drv_LockAcquire where it means drv_LockRelease - verified at
 * 00019509, which targets 00015930 and not 00015970. Every rejected request
 * therefore leaks TWO references: the one taken on entry and the one that
 * bogus call adds. See ../docs/known-defects.txt section 17.
 */
NTSTATUS AdaptoidVendorSend(PADAPTOID_DEVEXT DevExt,
                            const ADAPTOID_SETUP *Setup,
                            ULONG TransferLength, PVOID TransferBuffer,
                            ADAPTOID_VENDOR_CALLBACK Callback)
{
	PADAPTOID_VENDOR_SLOT slot = &DevExt->Vendor;
	NTSTATUS status;
	KIRQL irql;
	int submitted;

	status = AdaptoidLockAcquire(&DevExt->RemoveLockB);
	if (!NT_SUCCESS(status)) {
		/* Nothing was acquired, so nothing is released. The slot still
		 * has to be given back. */
		VendorFail(DevExt, status);
		return status;
	}

	if (Setup->bmRequestType != ADAPTOID_VENDOR_OUT &&
	    Setup->bmRequestType != ADAPTOID_VENDOR_IN) {
		VendorFail(DevExt, STATUS_INVALID_PARAMETER);
		AdaptoidLockRelease(&DevExt->RemoveLockB);
		return STATUS_INVALID_PARAMETER;
	}

	/*
	 * Only a caller that already claimed the slot may submit. The original
	 * checks this inside its submit helper and answers STATUS_DEVICE_BUSY;
	 * the check is here because there is nothing to allocate first.
	 */
	KeAcquireSpinLock(&slot->Lock, &irql);
	submitted = (slot->State == ADAPTOID_SLOT_CLAIMED);
	if (submitted) {
		slot->State    = ADAPTOID_SLOT_IN_FLIGHT;
		slot->Callback = Callback;
	}
	KeReleaseSpinLock(&slot->Lock, irql);

	if (!submitted) {
		AdaptoidLockRelease(&DevExt->RemoveLockB);
		return STATUS_DEVICE_BUSY;
	}

	slot->SubmitTime = KeQueryInterruptTime();
	return AdaptoidVendorSubmitUrb(DevExt, Setup, TransferLength,
	                               TransferBuffer);
}

/*
 * A transfer finished. Run the callback, then drain deferred work, and hand
 * the slot on to whichever of them takes it.
 *
 * A callback returning non-zero has re-used the slot and submitted again, so
 * the chain stops there. Zero means it is done and the next deferred item
 * gets a turn. If nothing takes the slot it goes back to free and any IRP
 * waiting on the result is completed with it.
 *
 * A FAILED TRANSFER SUPPRESSES THE CALLBACK but still drains deferred work.
 * That is deliberate in the original and worth keeping: the callback exists
 * to act on data that did not arrive, while the deferred queue is work that
 * never depended on this transfer.
 */
void AdaptoidVendorComplete(PADAPTOID_DEVEXT DevExt, NTSTATUS Status,
                            ULONG Information)
{
	PADAPTOID_VENDOR_SLOT slot = &DevExt->Vendor;
	ADAPTOID_VENDOR_CALLBACK cb;
	KIRQL irql;
	PIRP  pending = NULL;
	int   taken   = 0;
	int   held;

	held = NT_SUCCESS(AdaptoidLockAcquire(&DevExt->RemoveLockA));

	KeAcquireSpinLock(&slot->Lock, &irql);
	if (slot->State != ADAPTOID_SLOT_IN_FLIGHT) {
		/* A completion for a transfer we do not think we made. Drop it
		 * rather than corrupting the slot. */
		KeReleaseSpinLock(&slot->Lock, irql);
		if (held) {
			AdaptoidLockRelease(&DevExt->RemoveLockA);
		}
		AdaptoidLockRelease(&DevExt->RemoveLockB);
		return;
	}
	cb = slot->Callback;
	slot->Callback  = NULL;
	slot->LastStatus      = Status;
	slot->LastInformation = NT_SUCCESS(Status) ? Information : 0;

	/* Keep it claimed while the chain below decides. */
	slot->State = ADAPTOID_SLOT_CLAIMED;
	KeReleaseSpinLock(&slot->Lock, irql);

	if (!held) {
		cb = NULL;              /* the device is going away */
	}
	if (!NT_SUCCESS(Status)) {
		cb = NULL;
	}
	if (cb != NULL) {
		taken = cb(DevExt);
	}
	if (!taken) {
		/* Deferred work, in the core's own priority order. */
		taken = core_effect_run_deferred(&DevExt->Core, slot->SubmitTime);
	}

	if (!taken) {
		KeAcquireSpinLock(&slot->Lock, &irql);
		if (slot->State == ADAPTOID_SLOT_CLAIMED) {
			pending          = slot->PendingIrp;
			slot->State      = ADAPTOID_SLOT_FREE;
			slot->PendingIrp = NULL;
		}
		KeReleaseSpinLock(&slot->Lock, irql);

		if (pending != NULL) {
			AdaptoidCompleteIrp(pending, slot->LastStatus,
			                    slot->LastInformation);
		}
	}

	/* Signalled unconditionally: the one waiter is only ever waiting for
	 * "this transfer is over", not for a particular outcome. */
	KeSetEvent(&slot->Done, IO_NO_INCREMENT, FALSE);

	if (held) {
		AdaptoidLockRelease(&DevExt->RemoveLockA);
	}
	AdaptoidLockRelease(&DevExt->RemoveLockB);
}

/* ------------------------------------------------------------------ */
/* the OS edge                                                         */
/* ------------------------------------------------------------------ */

void AdaptoidCompleteIrp(PIRP Irp, NTSTATUS Status, ULONG Information)
{
	Irp->IoStatus.Status      = Status;
	Irp->IoStatus.Information = Information;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

#ifndef ADAPTOID_USERMODE
/*
 * Put one vendor control transfer on the wire.
 *
 * Builds a URB_FUNCTION_VENDOR_DEVICE request, attaches it to a fresh IRP as
 * IRP_MJ_INTERNAL_DEVICE_CONTROL with IOCTL_INTERNAL_USB_SUBMIT_URB, and
 * sends it down. TransferFlags carries USBD_TRANSFER_DIRECTION_IN plus
 * USBD_SHORT_TRANSFER_OK - the 3 the original writes - for an IN transfer,
 * and zero for an OUT.
 *
 * ONLY THIS FUNCTION IS BUILD-SPECIFIC. Everything above it - validating the
 * setup packet, arbitrating the slot, the remove lock, the callback and
 * deferred-work chain - is the same code in both builds, which is what lets
 * the harness exercise the transport without a USB stack.
 *
 * STILL A STUB: the URB layout needs usbdi.h and there is no device to send
 * it to yet.
 */
void AdaptoidFreePollIrp(PIRP Irp, PVOID Urb)
{
	if (Urb != NULL) {
		ExFreePool(Urb);
	}
	if (Irp != NULL) {
		IoFreeIrp(Irp);
	}
}

void AdaptoidCancelIrp(PIRP Irp)
{
	IoCancelIrp(Irp);
}

/*
 * Whether a parked request is still ours. The InterlockedExchange is the
 * whole of it: if the cancel routine was still set we won the race, and if
 * it was already null the canceller owns completing the request.
 */
int AdaptoidClaimIrp(PIRP Irp)
{
	return InterlockedExchange((LONG volatile *)&Irp->CancelRoutine, 0) != 0;
}

NTSTATUS AdaptoidCompleteRead(PADAPTOID_DEVEXT DevExt, PIRP Irp,
                              const UCHAR *Data, UCHAR Length)
{
	UCHAR *out = (UCHAR *)Irp->AssociatedIrp.SystemBuffer;
	ULONG  i;

	UNREFERENCED_PARAMETER(DevExt);
	for (i = 0; i < Length; i++) {
		out[i] = Data[i];
	}
	AdaptoidCompleteIrp(Irp, STATUS_SUCCESS, Length);
	AdaptoidLockRelease(&DevExt->RemoveLockB);
	return STATUS_SUCCESS;
}

#endif /* !ADAPTOID_USERMODE */

/* ------------------------------------------------------------------ */
/* the dispatch triage                                                 */
/* ------------------------------------------------------------------ */

ADAPTOID_SAVED_DISPATCH AdaptoidSavedDispatch;

/*
 * Which of the three clients this request belongs to.
 *
 * Both tests are on things the caller cannot forge from user mode: the
 * extension belongs to a device object this driver created, and the
 * FileName is what the I/O manager parsed out of the open path.
 *
 * NOTE ONLY Buffer[1] IS TESTED, not Buffer[0]. A four-byte FileName is two
 * WCHARs, and the first is almost certainly a backslash - a relative open of
 * "\q" - but the original never looks, so neither does this. Treating the
 * first character as significant would reject opens the original accepts.
 */
int AdaptoidRouteOf(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	const ULONG *ext = (const ULONG *)DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION sp;
	PFILE_OBJECT file;

	if (ext != NULL &&
	    ext[0] == ADAPTOID_CDO_MAGIC0 &&
	    ext[1] == ADAPTOID_CDO_MAGIC1 &&
	    ext[2] == ADAPTOID_CDO_MAGIC2) {
		return ADAPTOID_ROUTE_CONTROL;
	}

	sp   = IoGetCurrentIrpStackLocation(Irp);
	file = sp->FileObject;
	if (file != NULL && file->FileName.Length == 4 &&
	    file->FileName.Buffer != NULL &&
	    file->FileName.Buffer[1] == ADAPTOID_PRIVATE_CHAR) {
		return ADAPTOID_ROUTE_PRIVATE;
	}
	return ADAPTOID_ROUTE_HIDCLASS;
}

/* Chain to hidclass, or refuse if it did not claim this major function. */
static NTSTATUS ToHidclass(PDRIVER_DISPATCH Saved, PDEVICE_OBJECT DeviceObject,
                           PIRP Irp)
{
	if (Saved != NULL) {
		return Saved(DeviceObject, Irp);
	}
	return AdaptoidCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0),
	       STATUS_NOT_SUPPORTED;
}

/*
 * THE FOUR-WAY SHAPE: control device, private channel, or hidclass.
 * CREATE, CLEANUP, CLOSE and DEVICE_CONTROL all take it.
 */
NTSTATUS NTAPI AdaptoidCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	switch (AdaptoidRouteOf(DeviceObject, Irp)) {
	case ADAPTOID_ROUTE_CONTROL:
		return AdaptoidControlCreate(DeviceObject, Irp);
	case ADAPTOID_ROUTE_PRIVATE:
		return AdaptoidChannelCreate(DeviceObject, Irp);
	default:
		return ToHidclass(AdaptoidSavedDispatch.Create, DeviceObject, Irp);
	}
}

NTSTATUS NTAPI AdaptoidCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	switch (AdaptoidRouteOf(DeviceObject, Irp)) {
	case ADAPTOID_ROUTE_CONTROL:
		return AdaptoidControlCleanup(DeviceObject, Irp);
	case ADAPTOID_ROUTE_PRIVATE:
		return AdaptoidCompleteIrp(Irp, STATUS_SUCCESS, 0), STATUS_SUCCESS;
	default:
		return ToHidclass(AdaptoidSavedDispatch.Cleanup, DeviceObject, Irp);
	}
}

NTSTATUS NTAPI AdaptoidClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	switch (AdaptoidRouteOf(DeviceObject, Irp)) {
	case ADAPTOID_ROUTE_CONTROL:
		return AdaptoidControlClose(DeviceObject, Irp);
	case ADAPTOID_ROUTE_PRIVATE:
		return AdaptoidChannelClose(DeviceObject, Irp);
	default:
		return ToHidclass(AdaptoidSavedDispatch.Close, DeviceObject, Irp);
	}
}

NTSTATUS NTAPI AdaptoidDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	switch (AdaptoidRouteOf(DeviceObject, Irp)) {
	case ADAPTOID_ROUTE_CONTROL:
		return AdaptoidControlIoctl(DeviceObject, Irp);
	case ADAPTOID_ROUTE_PRIVATE:
		return AdaptoidChannelIoctl(DeviceObject, Irp);
	default:
		return ToHidclass(AdaptoidSavedDispatch.DeviceControl, DeviceObject,
		                  Irp);
	}
}

/*
 * THE THREE-WAY SHAPE: READ and WRITE. There is no private-channel path -
 * that channel is IOCTL-only.
 */
NTSTATUS NTAPI AdaptoidRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	if (AdaptoidRouteOf(DeviceObject, Irp) == ADAPTOID_ROUTE_CONTROL) {
		return AdaptoidControlReadWrite(DeviceObject, Irp);
	}
	return ToHidclass(AdaptoidSavedDispatch.Read, DeviceObject, Irp);
}

NTSTATUS NTAPI AdaptoidWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	if (AdaptoidRouteOf(DeviceObject, Irp) == ADAPTOID_ROUTE_CONTROL) {
		return AdaptoidControlReadWrite(DeviceObject, Irp);
	}
	return ToHidclass(AdaptoidSavedDispatch.Write, DeviceObject, Irp);
}

/*
 * THE TWO-WAY SHAPE: PNP and POWER. The control device is a plain
 * IoCreateDevice object with no PnP or power stack beneath it, so refusing
 * is correct rather than lazy.
 *
 * DIVERGENCE on the power path: the original completes a power IRP aimed at
 * the control device WITHOUT calling PoStartNextPowerIrp, which is a rule
 * violation. It cannot happen in practice - nothing sends power IRPs to a
 * bare control device - but the rule does not have an exception for that,
 * so the call is made here.
 */
NTSTATUS NTAPI AdaptoidPnpTriage(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	if (AdaptoidRouteOf(DeviceObject, Irp) == ADAPTOID_ROUTE_CONTROL) {
		return AdaptoidCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0),
		       STATUS_NOT_SUPPORTED;
	}
	return ToHidclass(AdaptoidSavedDispatch.Pnp, DeviceObject, Irp);
}

NTSTATUS NTAPI AdaptoidPowerTriage(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	if (AdaptoidRouteOf(DeviceObject, Irp) == ADAPTOID_ROUTE_CONTROL) {
		/*
		 * THE ORIGINAL DOES NOT DO THIS. drv_PowerTriage completes a
		 * control-device power IRP without calling PoStartNextPowerIrp
		 * first, which stalls the power queue for that device object -
		 * the one path in the driver that breaks a rule its own
		 * adapter dispatcher keeps everywhere. See known-defects.txt.
		 */
		PoStartNextPowerIrp(Irp);
		return AdaptoidCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0),
		       STATUS_NOT_SUPPORTED;
	}
	return ToHidclass(AdaptoidSavedDispatch.Power, DeviceObject, Irp);
}

/* ------------------------------------------------------------------ */
/* PnP                                                                 */
/* ------------------------------------------------------------------ */

/*
 * Bring the device up. Called from the PnP dispatcher on START_DEVICE after
 * the IRP has been passed down and completed successfully, so the bus driver
 * has already done its half.
 *
 *   1. fetch the device descriptor and cache it - IOCTL fn 0x836 selector 1
 *      reads bcdDevice out of that cache
 *   2. fetch the configuration descriptor, growing the buffer and retrying
 *      if wTotalLength says it is bigger than asked for
 *   3. select the configuration, and keep the FIRST pipe of the interface -
 *      the poll loop reads it without inspecting its type or endpoint
 *      address, which is a real assumption about this one device
 *   4. mark started; the query and cancel paths test that flag
 *   5. build the display name and re-sort the driver-wide list by it
 *   6. start polling, clearing the stop reason STOP_DEVICE sets
 *   7. publish the device interface, which posts the interface-changed event
 *
 * ORDER MATTERS: polling starts BEFORE the interface is published, so a
 * listener that reacts to the event finds a device already producing reports.
 */
NTSTATUS AdaptoidStartDevice(PADAPTOID_DEVEXT DevExt)
{
	NTSTATUS status;

	status = AdaptoidFetchDeviceDescriptor(DevExt);
	if (!NT_SUCCESS(status)) {
		return status;
	}
	status = AdaptoidSelectConfiguration(DevExt);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	DevExt->Started = 1;
	/* The name is a hub path found by matching this address, so the
	 * address has to be known first. */
	AdaptoidQueryFirmwareInfo(DevExt);
	AdaptoidSetDeviceName(DevExt);
	AdaptoidPollStart(DevExt, ADAPTOID_STOP_REASON_PNP);
	AdaptoidEnableInterface(DevExt);
	/* And tell any listening client that an adapter has arrived. */
	AdaptoidNotifyInterfaceChange(DevExt, 1);
	return STATUS_SUCCESS;
}

/* Pass the IRP down without touching it. */
static NTSTATUS PassDown(PADAPTOID_DEVEXT DevExt, PIRP Irp)
{
	IoSkipCurrentIrpStackLocation(Irp);
	return IofCallDriver(DevExt->NextDeviceObject, Irp);
}

/*
 * Pass the IRP down and WAIT for it, so the caller can act on the result.
 * START_DEVICE and QUERY_CAPABILITIES both need this; everything else the
 * dispatcher handles acts before passing down, or does not pass down at all.
 */
static NTSTATUS PassDownAndWait(PADAPTOID_DEVEXT DevExt, PIRP Irp)
{
	KEVENT done;
	NTSTATUS status;

	KeInitializeEvent(&done, NotificationEvent, FALSE);
	IoCopyCurrentIrpStackLocationToNext(Irp);
	AdaptoidSetCompletionRoutine(Irp, &done);
	status = IofCallDriver(DevExt->NextDeviceObject, Irp);
	if (status == STATUS_PENDING) {
		KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, NULL);
		status = Irp->IoStatus.Status;
	}
	return status;
}

/*
 * IRP_MJ_PNP. Installed BEFORE HidRegisterMinidriver, so hidclass owns this
 * entry and calls it; it is not the wrapper above.
 *
 * THE ASYMMETRY IS THE POINT and is worth preserving: START and
 * QUERY_CAPABILITIES are handled BOTTOM-UP, passing down first and acting on
 * the way back, while STOP and REMOVE are handled TOP-DOWN, tearing down
 * before passing down. That ordering is what keeps the poll loop from
 * touching USB resources the bus driver has already reclaimed.
 */
NTSTATUS NTAPI AdaptoidPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PADAPTOID_DEVEXT DevExt = AdaptoidDevExtOf(DeviceObject);
	PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
	NTSTATUS status;

	status = AdaptoidLockAcquire(&DevExt->RemoveLockB);
	if (!NT_SUCCESS(status)) {
		return AdaptoidCompleteIrp(Irp, status, 0), status;
	}

	switch (sp->MinorFunction) {

	case IRP_MN_START_DEVICE:
		status = AdaptoidLockAcquire(&DevExt->RemoveLockA);
		if (!NT_SUCCESS(status)) {
			AdaptoidLockRelease(&DevExt->RemoveLockB);
			return AdaptoidCompleteIrp(Irp, status, 0), status;
		}
		status = PassDownAndWait(DevExt, Irp);
		if (NT_SUCCESS(status)) {
			status = AdaptoidStartDevice(DevExt);
		}
		AdaptoidLockRelease(&DevExt->RemoveLockA);
		Irp->IoStatus.Information = 0;
		AdaptoidCompleteIrp(Irp, status, 0);
		AdaptoidLockRelease(&DevExt->RemoveLockB);
		return status;

	case IRP_MN_QUERY_REMOVE_DEVICE:
		/* Only meaningful once started; an unstarted device leaves the
		 * status alone and lets the bus driver answer. */
		if (DevExt->Started) {
			DevExt->RemovePending = 1;
			Irp->IoStatus.Status = STATUS_SUCCESS;
		}
		break;

	case IRP_MN_CANCEL_REMOVE_DEVICE:
		if (DevExt->Started) {
			DevExt->RemovePending = 0;
			Irp->IoStatus.Status = STATUS_SUCCESS;
		}
		break;

	case IRP_MN_QUERY_STOP_DEVICE:
		if (DevExt->Started) {
			/* The one veto this driver casts. */
			if (DevExt->StopVeto) {
				AdaptoidCompleteIrp(Irp, STATUS_UNSUCCESSFUL, 0);
				AdaptoidLockRelease(&DevExt->RemoveLockB);
				return STATUS_UNSUCCESSFUL;
			}
			DevExt->StopPending = 1;
			Irp->IoStatus.Status = STATUS_SUCCESS;
		}
		break;

	case IRP_MN_CANCEL_STOP_DEVICE:
		if (DevExt->Started) {
			DevExt->StopPending = 0;
			Irp->IoStatus.Status = STATUS_SUCCESS;
		}
		break;

	case IRP_MN_STOP_DEVICE:
		/* TOP-DOWN: stop polling and release USB resources BEFORE the bus
		 * driver reclaims them. */
		AdaptoidPollStop(DevExt, ADAPTOID_STOP_REASON_PNP,
		                 ADAPTOID_POLL_SLOTS);
		AdaptoidQuiesceIo(DevExt);
		AdaptoidUnconfigureDevice(DevExt);
		DevExt->Started = 0;
		/*
		 * A STOP IS NOT A REMOVE: the device may be started again, so
		 * the engine state is put back to defaults rather than torn
		 * down. Leaving a half-finished effect chain across a stop
		 * would resume it against a device that had been reconfigured.
		 */
		AdaptoidNotifyInterfaceChange(DevExt, 0);
		core_reset(&DevExt->Core);
		Irp->IoStatus.Status = STATUS_SUCCESS;
		break;

	case IRP_MN_REMOVE_DEVICE:
		DevExt->Removing = 1;
		/* Drain A first: no new work may start while we tear down. */
		AdaptoidLockAcquire(&DevExt->RemoveLockA);
		AdaptoidLockReleaseAndWait(&DevExt->RemoveLockA);

		AdaptoidRegistryRemove(DevExt);
		AdaptoidPollStop(DevExt, ADAPTOID_STOP_REASON_REMOVE,
		                 ADAPTOID_POLL_SLOTS);
		AdaptoidQuiesceIo(DevExt);
		AdaptoidAbortPipes(DevExt);

		IoCopyCurrentIrpStackLocationToNext(Irp);
		IofCallDriver(DevExt->NextDeviceObject, Irp);

		/* Now wait for B, the one this dispatcher itself holds. */
		AdaptoidLockReleaseAndWait(&DevExt->RemoveLockB);

		/* Unpick the wiring before the memory under it goes. */
		AdaptoidUnwireDevice(DevExt);
		AdaptoidFreeDeviceResources(DevExt);
		return STATUS_SUCCESS;

	case IRP_MN_QUERY_CAPABILITIES: {
		PDEVICE_CAPABILITIES caps = sp->Parameters.DeviceCapabilities.
			                        Capabilities;

		/* BOTTOM-UP: let the bus driver fill it in, then add ours. */
		status = PassDownAndWait(DevExt, Irp);
		if (NT_SUCCESS(status) && caps != NULL) {
			/*
			 * Removable and SurpriseRemovalOK. The correct declaration
			 * for something that can be unplugged mid-transfer, and
			 * without it Windows warns the user to stop the device
			 * first.
			 */
			caps->Removable         = 1;
			caps->SurpriseRemovalOK = 1;
		}
		Irp->IoStatus.Information = 0;
		AdaptoidCompleteIrp(Irp, status, 0);
		AdaptoidLockRelease(&DevExt->RemoveLockB);
		return status;
	}

	case IRP_MN_SURPRISE_REMOVAL:
		Irp->IoStatus.Status = STATUS_SUCCESS;
		break;

	default:
		break;
	}

	status = PassDown(DevExt, Irp);
	AdaptoidLockRelease(&DevExt->RemoveLockB);
	return status;
}

/* ------------------------------------------------------------------ */
/* stage three                                                         */
/* ------------------------------------------------------------------ */

/*
 * Everything below is named and shaped but not yet written. They are here
 * rather than absent because the dispatcher above calls them, and having
 * them compile fixes the interface the next stage has to satisfy.
 *
 * In the harness build these are supplied by harness.c instead, which is how
 * the PnP dispatcher's ORDERING - what it calls, and in which order, for each
 * minor function - is checked without a device stack underneath.
 */
#ifndef ADAPTOID_USERMODE

PADAPTOID_DEVEXT AdaptoidDevExtOf(PDEVICE_OBJECT DeviceObject)
{
	/* hidclass owns the first level; ours hangs off it. */
	return (PADAPTOID_DEVEXT)
	       (((PHID_DEVICE_EXTENSION)DeviceObject->DeviceExtension)
	        ->MiniDeviceExtension);
}

/* ------------------------------------------------------------------ */
/* the USB layer                                                       */
/*                                                                     */
/* Driver build only. Everything above this line decides WHAT to send; */
/* this decides how to put it on the wire, and it is the one part of   */
/* the driver the harness cannot exercise - there is no bus to talk to.*/
/* ------------------------------------------------------------------ */

/*
 * Send one IRP down and wait for it. The bus driver's port requests and the
 * descriptor fetch are all synchronous, and all three want the same eleven
 * lines, so they share them.
 */
static NTSTATUS NTAPI SyncComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                                   PVOID Context)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	UNREFERENCED_PARAMETER(Irp);
	KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS SendInternalIoctlSync(PADAPTOID_DEVEXT DevExt, ULONG Code,
                                      PVOID Argument1, PVOID Argument2)
{
	KEVENT             done;
	IO_STATUS_BLOCK    io;
	PIRP               irp;
	PIO_STACK_LOCATION sl;
	NTSTATUS           st;

	KeInitializeEvent(&done, NotificationEvent, FALSE);
	irp = IoBuildDeviceIoControlRequest(Code, DevExt->NextDeviceObject,
	                                    NULL, 0, NULL, 0, TRUE, &done,
	                                    &io);
	if (irp == NULL) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	sl = IoGetNextIrpStackLocation(irp);
	sl->Parameters.Others.Argument1 = Argument1;
	sl->Parameters.Others.Argument2 = Argument2;
	/*
	 * IoBuildDeviceIoControlRequest already set the event; the completion
	 * routine below would double-signal it, so this one is left to the
	 * I/O manager and only the wait is ours.
	 */
	st = IoCallDriver(DevExt->NextDeviceObject, irp);
	if (st == STATUS_PENDING) {
		KeWaitForSingleObject(&done, Executive, KernelMode, FALSE,
		                      NULL);
		st = io.Status;
	}
	return st;
}

/* Put one URB on the bus and wait for it. */
static NTSTATUS SubmitUrbSync(PADAPTOID_DEVEXT DevExt, PURB Urb)
{
	return SendInternalIoctlSync(DevExt, IOCTL_INTERNAL_USB_SUBMIT_URB,
	                             Urb, NULL);
}

/*
 * Fetch the configuration descriptor, growing the buffer until the whole of
 * it fits.
 *
 * THE FIRST ASK IS DELIBERATELY GENEROUS - one page and change - because a
 * descriptor that fits first time saves a round trip, and this adapter's is
 * far smaller than that. wTotalLength in the reply says how much there
 * really is; anything larger than what was asked for means the reply was
 * truncated and the fetch is repeated at the size it named.
 */
/* The URB type's own name runs past eighty columns everywhere it is used. */
typedef struct _URB_CONTROL_DESCRIPTOR_REQUEST DESC_REQUEST;

NTSTATUS AdaptoidFetchDeviceDescriptor(PADAPTOID_DEVEXT DevExt)
{
	PURB     urb;
	ULONG    size = ADAPTOID_CONFIG_FIRST_TRY;
	NTSTATUS st;

	urb = (PURB)ExAllocatePoolWithTag(NonPagedPool, sizeof(DESC_REQUEST),
	                                  ADAPTOID_POOL_TAG);
	if (urb == NULL) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	for (;;) {
		PUSB_CONFIGURATION_DESCRIPTOR cd;

		DevExt->ConfigDescriptor =
		        ExAllocatePoolWithTag(NonPagedPool, size,
		                              ADAPTOID_POOL_TAG);
		if (DevExt->ConfigDescriptor == NULL) {
			ExFreePool(urb);
			return STATUS_INSUFFICIENT_RESOURCES;
		}

		UsbBuildGetDescriptorRequest(urb, (USHORT)sizeof(DESC_REQUEST),
		        USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0,
		        DevExt->ConfigDescriptor, NULL, size, NULL);
		st = SubmitUrbSync(DevExt, urb);

		cd = (PUSB_CONFIGURATION_DESCRIPTOR)DevExt->ConfigDescriptor;
		if (!NT_SUCCESS(st) ||
		    urb->UrbControlDescriptorRequest.TransferBufferLength == 0) {
			break;
		}
		if (cd->wTotalLength <= size) {
			break;              /* the whole of it arrived */
		}
		/* Truncated. Try again at the size the device named. */
		size = cd->wTotalLength;
		ExFreePool(DevExt->ConfigDescriptor);
		DevExt->ConfigDescriptor = NULL;
	}

	ExFreePool(urb);
	if (DevExt->ConfigDescriptor == NULL) {
		return STATUS_DEVICE_DATA_ERROR;
	}
	/*
	 * The original tail-calls drv_SelectConfiguration from here.
	 * AdaptoidStartDevice makes that call instead, so the start sequence
	 * reads in one place rather than half of it hiding inside the fetch.
	 */
	return STATUS_SUCCESS;
}

/*
 * Select the one configuration and its one interface, and keep the pipe the
 * controller state arrives on.
 *
 * THE INTERRUPT IN PIPE IS FOUND BY TYPE AND DIRECTION, not by index. The
 * adapter has exactly one, but reading it out of the descriptor rather than
 * assuming pipe 0 is what makes this survive a firmware revision that adds
 * another endpoint.
 */
NTSTATUS AdaptoidSelectConfiguration(PADAPTOID_DEVEXT DevExt)
{
	PUSB_CONFIGURATION_DESCRIPTOR cd =
	        (PUSB_CONFIGURATION_DESCRIPTOR)DevExt->ConfigDescriptor;
	USBD_INTERFACE_LIST_ENTRY     list[2];
	PUSBD_INTERFACE_INFORMATION   info;
	PURB                          urb;
	NTSTATUS                      st;
	ULONG                         i;

	if (cd == NULL) {
		return STATUS_DEVICE_DATA_ERROR;
	}
	list[0].InterfaceDescriptor =
	        USBD_ParseConfigurationDescriptorEx(cd, cd, -1, -1, -1, -1, -1);
	list[0].Interface = NULL;
	list[1].InterfaceDescriptor = NULL;
	list[1].Interface = NULL;
	if (list[0].InterfaceDescriptor == NULL) {
		return STATUS_DEVICE_DATA_ERROR;
	}

	urb = USBD_CreateConfigurationRequestEx(cd, list);
	if (urb == NULL) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	st = SubmitUrbSync(DevExt, urb);
	if (!NT_SUCCESS(st)) {
		ExFreePool(urb);
		return st;
	}

	DevExt->ConfigurationHandle =
	        urb->UrbSelectConfiguration.ConfigurationHandle;

	info = list[0].Interface;
	DevExt->InterfaceInfo =
	        ExAllocatePoolWithTag(NonPagedPool, info->Length,
	                              ADAPTOID_POOL_TAG);
	if (DevExt->InterfaceInfo == NULL) {
		ExFreePool(urb);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	RtlCopyMemory(DevExt->InterfaceInfo, info, info->Length);
	info = (PUSBD_INTERFACE_INFORMATION)DevExt->InterfaceInfo;

	DevExt->InterruptPipe = NULL;
	for (i = 0; i < info->NumberOfPipes; i++) {
		if (info->Pipes[i].PipeType == UsbdPipeTypeInterrupt &&
		    USB_ENDPOINT_DIRECTION_IN(
		            info->Pipes[i].EndpointAddress)) {
			DevExt->InterruptPipe = info->Pipes[i].PipeHandle;
			break;
		}
	}
	ExFreePool(urb);
	if (DevExt->InterruptPipe == NULL) {
		return STATUS_DEVICE_DATA_ERROR;
	}
	return STATUS_SUCCESS;
}

/*
 * Drop the configuration. A SELECT_CONFIGURATION with a null descriptor is
 * the USB way of saying "unconfigured", and it is what invalidates every
 * pipe handle - so nothing may be in flight when it goes out.
 */
void AdaptoidUnconfigureDevice(PADAPTOID_DEVEXT DevExt)
{
	PURB  urb;
	/*
	 * NOT GET_SELECT_CONFIGURATION_REQUEST_SIZE(0, 0). That macro
	 * subtracts one from the interface count before multiplying, so at
	 * zero interfaces it underflows - the compiler says so. An
	 * unconfigure request carries no interface array, so the bare
	 * structure is exactly the right size.
	 */
	ULONG size = sizeof(struct _URB_SELECT_CONFIGURATION);

	if (DevExt->ConfigurationHandle == NULL) {
		return;
	}
	urb = (PURB)ExAllocatePoolWithTag(NonPagedPool, size,
	                                  ADAPTOID_POOL_TAG);
	if (urb == NULL) {
		return;
	}
	UsbBuildSelectConfigurationRequest(urb, (USHORT)size, NULL);
	SubmitUrbSync(DevExt, urb);
	ExFreePool(urb);

	DevExt->ConfigurationHandle = NULL;
	DevExt->InterruptPipe       = NULL;
}

/*
 * Abort whatever is outstanding on the interrupt pipe.
 *
 * NOT THE SAME AS CANCELLING THE IRPS. Cancelling asks the bus driver to
 * give an IRP back; aborting tells it to discard everything queued on the
 * pipe and put it back in a known state. Removal does both, in that order.
 */
void AdaptoidAbortPipes(PADAPTOID_DEVEXT DevExt)
{
	struct _URB_PIPE_REQUEST urb;

	if (DevExt->InterruptPipe == NULL) {
		return;
	}
	RtlZeroMemory(&urb, sizeof(urb));
	urb.Hdr.Length     = (USHORT)sizeof(urb);
	urb.Hdr.Function   = URB_FUNCTION_ABORT_PIPE;
	urb.PipeHandle     = DevExt->InterruptPipe;
	SubmitUrbSync(DevExt, (PURB)&urb);
}

/*
 * Stop everything and wait for it to have stopped.
 *
 * THE WAIT IS THE POINT. AdaptoidPollStop only asks; the reads come back
 * through their completion routine some time later, and unconfiguring the
 * device before then would invalidate pipe handles that are still in use.
 */
void AdaptoidQuiesceIo(PADAPTOID_DEVEXT DevExt)
{
	AdaptoidPollStop(DevExt, ADAPTOID_STOP_REASON_PNP,
	                 ADAPTOID_POLL_SLOTS);
	AdaptoidAbortPipes(DevExt);
}

void AdaptoidFreeDeviceResources(PADAPTOID_DEVEXT DevExt)
{
	if (DevExt->ConfigDescriptor != NULL) {
		ExFreePool(DevExt->ConfigDescriptor);
		DevExt->ConfigDescriptor = NULL;
	}
	if (DevExt->InterfaceInfo != NULL) {
		ExFreePool(DevExt->InterfaceInfo);
		DevExt->InterfaceInfo = NULL;
	}
	if (DevExt->PollWorkItem != NULL) {
		IoFreeWorkItem((PIO_WORKITEM)DevExt->PollWorkItem);
		DevExt->PollWorkItem = NULL;
	}
}

/* ---- the two asynchronous transfers -------------------------------- */

/*
 * A vendor control transfer. The URB and the IRP are freed by the
 * completion, not here, because this returns as soon as the bus driver has
 * taken the request.
 */
static NTSTATUS NTAPI VendorUrbComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                                        PVOID Context)
{
	PADAPTOID_DEVEXT dx = (PADAPTOID_DEVEXT)Context;
	PURB             urb = (PURB)dx->Vendor.Urb;
	ULONG            length = 0;

	UNREFERENCED_PARAMETER(DeviceObject);

	if (urb != NULL) {
		length = urb->UrbControlVendorClassRequest.TransferBufferLength;
		ExFreePool(urb);
		dx->Vendor.Urb = NULL;
	}
	dx->Vendor.UrbIrp = NULL;
	IoFreeIrp(Irp);

	AdaptoidVendorComplete(dx, Irp->IoStatus.Status, length);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS AdaptoidVendorSubmitUrb(PADAPTOID_DEVEXT DevExt,
                                 const ADAPTOID_SETUP *Setup,
                                 ULONG TransferLength, PVOID TransferBuffer)
{
	struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *urb;
	PIRP               irp;
	PIO_STACK_LOCATION sl;
	ULONG              flags;

	urb = (struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *)
	        ExAllocatePoolWithTag(NonPagedPool, sizeof(*urb),
	                              ADAPTOID_POOL_TAG);
	if (urb == NULL) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	irp = IoAllocateIrp(DevExt->NextDeviceObject->StackSize, FALSE);
	if (irp == NULL) {
		ExFreePool(urb);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	/*
	 * SHORT TRANSFERS ARE NOT ERRORS on this device: a reply shorter than
	 * the buffer is how it says "that is all there was", and the status
	 * byte in the reply carries the real outcome.
	 */
	flags = USBD_SHORT_TRANSFER_OK;
	if (Setup->bmRequestType == ADAPTOID_VENDOR_IN) {
		flags |= USBD_TRANSFER_DIRECTION_IN;
	}
	UsbBuildVendorRequest((PURB)urb,
	                      URB_FUNCTION_VENDOR_DEVICE,
	                      (USHORT)sizeof(*urb), flags, 0,
	                      Setup->bRequest, Setup->wValue, Setup->wIndex,
	                      TransferBuffer, NULL, TransferLength, NULL);

	DevExt->Vendor.Urb    = urb;
	DevExt->Vendor.UrbIrp = irp;

	sl = IoGetNextIrpStackLocation(irp);
	sl->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
	sl->Parameters.DeviceIoControl.IoControlCode =
	        IOCTL_INTERNAL_USB_SUBMIT_URB;
	sl->Parameters.Others.Argument1 = urb;
	IoSetCompletionRoutine(irp, VendorUrbComplete, DevExt, TRUE, TRUE,
	                       TRUE);
	return IoCallDriver(DevExt->NextDeviceObject, irp);
}

/*
 * One interrupt read into one of the two slots.
 *
 * WHICH SLOT IS CARRIED IN THE CONTEXT, as a small integer rather than a
 * pointer, so the completion can find its slot without a search and without
 * a second allocation to hold the pairing.
 */
static NTSTATUS NTAPI PollUrbComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                                      PVOID Context)
{
	PADAPTOID_POLL_CONTEXT ctx = (PADAPTOID_POLL_CONTEXT)Context;
	PADAPTOID_DEVEXT       dx  = ctx->DevExt;
	ULONG                  slot = ctx->Slot;
	PURB                   urb = (PURB)dx->PollSlot[slot].Urb;
	ULONG                  length = 0;

	UNREFERENCED_PARAMETER(DeviceObject);

	if (urb != NULL) {
		length = urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
	}
	AdaptoidPollComplete(dx, slot, Irp->IoStatus.Status, length);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS AdaptoidPollSubmit(PADAPTOID_DEVEXT DevExt, ULONG Slot)
{
	struct _URB_BULK_OR_INTERRUPT_TRANSFER *urb;
	PIRP                irp;
	PIO_STACK_LOCATION  sl;
	PADAPTOID_POLL_CONTEXT ctx;

	if (Slot >= ADAPTOID_POLL_SLOTS || DevExt->InterruptPipe == NULL) {
		return STATUS_INVALID_PARAMETER;
	}
	urb = (struct _URB_BULK_OR_INTERRUPT_TRANSFER *)
	        ExAllocatePoolWithTag(NonPagedPool, sizeof(*urb),
	                              ADAPTOID_POOL_TAG);
	if (urb == NULL) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	irp = IoAllocateIrp(DevExt->NextDeviceObject->StackSize, FALSE);
	if (irp == NULL) {
		ExFreePool(urb);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	UsbBuildInterruptOrBulkTransferRequest((PURB)urb, (USHORT)sizeof(*urb),
	        DevExt->InterruptPipe, DevExt->PollSlot[Slot].Buffer, NULL,
	        ADAPTOID_POLL_BYTES,
	        USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK, NULL);

	DevExt->PollSlot[Slot].Urb = urb;
	DevExt->PollSlot[Slot].Irp = irp;

	ctx = &DevExt->PollContext[Slot];
	ctx->DevExt = DevExt;
	ctx->Slot   = Slot;

	sl = IoGetNextIrpStackLocation(irp);
	sl->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
	sl->Parameters.DeviceIoControl.IoControlCode =
	        IOCTL_INTERNAL_USB_SUBMIT_URB;
	sl->Parameters.Others.Argument1 = urb;
	IoSetCompletionRoutine(irp, PollUrbComplete, ctx, TRUE, TRUE, TRUE);
	return IoCallDriver(DevExt->NextDeviceObject, irp);
}

/* ---- port recovery ------------------------------------------------- */

NTSTATUS AdaptoidUsbGetPortStatus(PADAPTOID_DEVEXT DevExt, ULONG *Status)
{
	*Status = 0;
	return SendInternalIoctlSync(DevExt,
	                             IOCTL_INTERNAL_USB_GET_PORT_STATUS,
	                             Status, NULL);
}

NTSTATUS AdaptoidUsbResetPort(PADAPTOID_DEVEXT DevExt)
{
	return SendInternalIoctlSync(DevExt, IOCTL_INTERNAL_USB_RESET_PORT,
	                             NULL, NULL);
}

void AdaptoidUsbCyclePort(PADAPTOID_DEVEXT DevExt)
{
	SendInternalIoctlSync(DevExt, IOCTL_INTERNAL_USB_CYCLE_PORT, NULL,
	                      NULL);
}

/* ---- the rest of the kernel edge ----------------------------------- */

static void NTAPI PollRestartWorker(PDEVICE_OBJECT DeviceObject,
                                    PVOID Context)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	AdaptoidPollRestartWorker((PADAPTOID_DEVEXT)Context);
}

/*
 * THE RESTART RUNS ON A WORK ITEM BECAUSE IT BLOCKS. A read completes at
 * DISPATCH_LEVEL and the recovery ladder sends synchronous port requests, so
 * it cannot possibly run where it was decided.
 */
void AdaptoidQueuePollRestart(PADAPTOID_DEVEXT DevExt)
{
	if (DevExt->PollWorkItem == NULL) {
		return;
	}
	IoQueueWorkItem((PIO_WORKITEM)DevExt->PollWorkItem, PollRestartWorker,
	                DelayedWorkQueue, DevExt);
}

void AdaptoidEnableInterface(PADAPTOID_DEVEXT DevExt)
{
	if (DevExt->InterfaceName.Buffer != NULL) {
		IoSetDeviceInterfaceState(&DevExt->InterfaceName, TRUE);
	}
}

void AdaptoidRegistryRemove(PADAPTOID_DEVEXT DevExt)
{
	if (DevExt->InterfaceName.Buffer != NULL) {
		IoSetDeviceInterfaceState(&DevExt->InterfaceName, FALSE);
		RtlFreeUnicodeString(&DevExt->InterfaceName);
		DevExt->InterfaceName.Buffer = NULL;
	}
}

void AdaptoidSetCompletionRoutine(PIRP Irp, PVOID Event)
{
	IoSetCompletionRoutine(Irp, SyncComplete, Event, TRUE, TRUE, TRUE);
}

/*
 * THE DEVICE INTERFACE CLASS, read out of the original at 00010358:
 *
 *     a1 71 7e 82 af 2c d3 11 85 27 00 a0 c9 9b 19 df
 *
 * REUSED DELIBERATELY. The configurator opens adapters by enumerating this
 * class, so a replacement that invents its own GUID is invisible to every
 * existing client - the same reason the control device keeps the name
 * Wish_NA1.
 */
static const GUID GUID_DEVINTERFACE_ADAPTOID = {
	0x827E71A1, 0x2CAF, 0x11D3,
	{ 0x85, 0x27, 0x00, 0xA0, 0xC9, 0x9B, 0x19, 0xDF }
};

/*
 * Publish the device interface user mode finds this adapter by, and record
 * the name so it can be enabled at start and torn down at removal.
 *
 * NO COPY, which is the difference that matters. The original copies the
 * returned name into a fixed 0x200-byte field with a loop driven only by the
 * source length - see known-defects.txt section 6. Here the UNICODE_STRING
 * the I/O manager allocated is kept as it stands and freed at removal, so
 * there is no destination to overrun.
 */
NTSTATUS AdaptoidRegisterDeviceInterface(PADAPTOID_DEVEXT DevExt)
{
	return IoRegisterDeviceInterface(DevExt->PhysicalDeviceObject,
	                                 &GUID_DEVINTERFACE_ADAPTOID, NULL,
	                                 &DevExt->InterfaceName);
}

/*
 * Read one DWORD out of the driver's service key.
 *
 * ABSENT IS NOT AN ERROR - it means "use the default", which is how the
 * virtual-device mask is made configurable without needing an INF to write
 * it. Anything that goes wrong gives the default too, because a driver that
 * refuses to start over a missing optional registry value is worse than one
 * that uses its built-in answer.
 */
ULONG AdaptoidRegQueryDword(PCWSTR Name, ULONG Default)
{
	RTL_QUERY_REGISTRY_TABLE table[2];
	ULONG                    value = Default;

	RtlZeroMemory(table, sizeof(table));
	table[0].Flags         = RTL_QUERY_REGISTRY_DIRECT |
	                         RTL_QUERY_REGISTRY_REQUIRED;
	table[0].Name          = (PWSTR)Name;
	table[0].EntryContext  = &value;
	table[0].DefaultType   = REG_DWORD;
	table[0].DefaultData   = &Default;
	table[0].DefaultLength = sizeof(Default);

	if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
	                                       ADAPTOID_SETTINGS_KEY, table,
	                                       NULL, NULL))) {
		return Default;
	}
	return value;
}

/*
 * Installing AddDevice. A one-line write in the DDK, behind a seam because
 * the harness has no driver extension to write into and wants to record
 * that the call happened - which is the only way to catch it being missed.
 */
void AdaptoidSetAddDevice(PDRIVER_OBJECT DriverObject,
                          ADAPTOID_ADD_DEVICE AddDevice)
{
	DriverObject->DriverExtension->AddDevice =
	        (PDRIVER_ADD_DEVICE)AddDevice;
}

PVOID AdaptoidAllocateWorkItem(PDEVICE_OBJECT DeviceObject)
{
	return IoAllocateWorkItem(DeviceObject);
}

void AdaptoidFreeWorkItem(PVOID WorkItem)
{
	if (WorkItem != NULL) {
		IoFreeWorkItem((PIO_WORKITEM)WorkItem);
	}
}

NTSTATUS AdaptoidRequestPowerIrp(PADAPTOID_DEVEXT DevExt, ULONG State,
                                 PREQUEST_POWER_COMPLETE Complete)
{
	POWER_STATE ps;

	ps.DeviceState = (DEVICE_POWER_STATE)State;
	return PoRequestPowerIrp(DevExt->PhysicalDeviceObject,
	                         IRP_MN_SET_POWER, ps, Complete, DevExt, NULL);
}

NTSTATUS NTAPI AdaptoidChannelCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{ UNREFERENCED_PARAMETER(DeviceObject);
  return AdaptoidCompleteIrp(Irp, STATUS_SUCCESS, 0), STATUS_SUCCESS; }

NTSTATUS NTAPI AdaptoidChannelClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{ UNREFERENCED_PARAMETER(DeviceObject);
  return AdaptoidCompleteIrp(Irp, STATUS_SUCCESS, 0), STATUS_SUCCESS; }

NTSTATUS NTAPI AdaptoidChannelIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{ UNREFERENCED_PARAMETER(DeviceObject);
  return AdaptoidCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0),
	     STATUS_NOT_SUPPORTED; }

#endif /* !ADAPTOID_USERMODE */

/*
 * Bring a device extension to a usable state. Every lock initialised, every
 * list head pointing at itself.
 *
 * A LIST HEAD OF ZEROES IS NOT AN EMPTY LIST, it is a null pointer waiting
 * to be walked - so this is not optional, and it is one call rather than
 * scattered initialisation so that adding a list cannot forget it.
 */
void AdaptoidDevExtInit(PADAPTOID_DEVEXT DevExt)
{
	ULONG i;

	AdaptoidLockInit(&DevExt->RemoveLockA);
	AdaptoidLockInit(&DevExt->RemoveLockB);

	KeInitializeSpinLock(&DevExt->Vendor.Lock);
	DevExt->Vendor.State = ADAPTOID_SLOT_FREE;

	KeInitializeSpinLock(&DevExt->PollLock);
	for (i = 0; i < ADAPTOID_POLL_SLOTS; i++) {
		DevExt->PollSlot[i].CancelLatch = 0;
		DevExt->PollSlot[i].Irp         = NULL;
		DevExt->PollSlot[i].Urb         = NULL;
		DevExt->PollSlot[i].Active      = 0;
	}
	/* Polling starts STOPPED, for the PnP reason, so nothing reads the
	 * device before START_DEVICE has selected a configuration. */
	DevExt->PollStopMask       = ADAPTOID_STOP_REASON_PNP;
	DevExt->PollRestartPending = 0;

	KeInitializeSpinLock(&DevExt->ReportLock);
	DevExt->ReportHead       = 0;
	DevExt->ReportCount      = 0;
	DevExt->ReportsDropped   = 0;
	DevExt->PendingReads.Flink = &DevExt->PendingReads;
	DevExt->PendingReads.Blink = &DevExt->PendingReads;
	DevExt->PendingReadCount = 0;
}

/* ------------------------------------------------------------------ */
/* the polling engine                                                  */
/* ------------------------------------------------------------------ */

/*
 * Clear one reason from the stop mask and, once NO reasons remain, put both
 * slots back in flight.
 *
 * A slot already active is left alone, so calling this twice does not submit
 * four reads.
 */
void AdaptoidPollStart(PADAPTOID_DEVEXT DevExt, ULONG Reason)
{
	KIRQL irql;
	int submit[ADAPTOID_POLL_SLOTS];
	ULONG i;

	for (i = 0; i < ADAPTOID_POLL_SLOTS; i++) {
		submit[i] = 0;
	}

	KeAcquireSpinLock(&DevExt->PollLock, &irql);
	DevExt->PollStopMask &= ~Reason;
	if (DevExt->PollStopMask == 0) {
		for (i = 0; i < ADAPTOID_POLL_SLOTS; i++) {
			submit[i] = (DevExt->PollSlot[i].Active == 0);
			DevExt->PollSlot[i].Active = 1;
		}
	}
	KeReleaseSpinLock(&DevExt->PollLock, irql);

	/* Submitted outside the lock; the submit itself can complete
	 * synchronously and would deadlock against it. */
	for (i = 0; i < ADAPTOID_POLL_SLOTS; i++) {
		if (submit[i]) {
			AdaptoidPollSubmit(DevExt, i);
		}
	}
}

/*
 * Add a reason to the stop mask and cancel whatever is outstanding.
 *
 * Slot names the slot that is already completing, if any, so it is not
 * cancelled; pass ADAPTOID_POLL_SLOTS to mean "cancel everything", which is
 * what the PnP paths do.
 *
 * Returns non-zero if this was the LAST slot outstanding. The completion
 * path uses that to queue the restart work exactly once no matter which of
 * the two reads failed first.
 */
int AdaptoidPollStop(PADAPTOID_DEVEXT DevExt, ULONG Reason, ULONG Slot)
{
	KIRQL irql;
	PIRP  cancel[ADAPTOID_POLL_SLOTS];
	PVOID urb[ADAPTOID_POLL_SLOTS];
	ULONG i;
	int   last = 0;
	int   any  = 0;

	KeAcquireSpinLock(&DevExt->PollLock, &irql);
	DevExt->PollStopMask |= Reason;
	if (Slot < ADAPTOID_POLL_SLOTS) {
		DevExt->PollSlot[Slot].Active = 0;
		/* The OTHER slot having no IRP means this was the last one. */
		last = (DevExt->PollSlot[Slot ^ 1].Irp == NULL);
	}
	for (i = 0; i < ADAPTOID_POLL_SLOTS; i++) {
		cancel[i] = NULL;
		urb[i]    = NULL;
		if (DevExt->PollSlot[i].Active != 0 &&
		    DevExt->PollSlot[i].CancelLatch == 0) {
			cancel[i] = DevExt->PollSlot[i].Irp;
			urb[i]    = DevExt->PollSlot[i].Urb;
			/* Latch it: the completion path must not free it now. */
			DevExt->PollSlot[i].CancelLatch = 1;
			if (cancel[i] != NULL) {
				any = 1;
			}
		}
	}
	KeReleaseSpinLock(&DevExt->PollLock, irql);

	if (!any) {
		return last;
	}

	for (i = 0; i < ADAPTOID_POLL_SLOTS; i++) {
		if (cancel[i] != NULL) {
			AdaptoidCancelIrp(cancel[i]);
		}
	}

	/*
	 * THE HANDSHAKE. If the latch is still set the completion path has not
	 * run, so it will find the latch set, clear it, and leave the freeing
	 * to us - except that we then drop the reference, because it has not
	 * finished with the IRP yet. If the latch is already CLEAR, completion
	 * got there first and freed nothing, so we free.
	 */
	KeAcquireSpinLock(&DevExt->PollLock, &irql);
	for (i = 0; i < ADAPTOID_POLL_SLOTS; i++) {
		if (cancel[i] != NULL && DevExt->PollSlot[i].CancelLatch != 0) {
			DevExt->PollSlot[i].CancelLatch = 0;
			cancel[i] = NULL;       /* completion will free it */
		}
	}
	KeReleaseSpinLock(&DevExt->PollLock, irql);

	for (i = 0; i < ADAPTOID_POLL_SLOTS; i++) {
		if (cancel[i] != NULL) {
			AdaptoidFreePollIrp(cancel[i], urb[i]);
		}
	}
	return last;
}

/*
 * One read finished.
 *
 * Three things must all hold for a packet to be accepted: the IRP succeeded,
 * the URB succeeded, and the transfer was EXACTLY five bytes. A short read
 * is not a partial packet to be salvaged - the controller state is five
 * bytes or it is nothing.
 *
 * On success the packet goes to the core and the slot resubmits immediately.
 * On failure polling stops and the restart work is queued, once.
 */
void AdaptoidPollComplete(PADAPTOID_DEVEXT DevExt, ULONG Slot,
                          NTSTATUS Status, ULONG Length)
{
	KIRQL irql;
	PIRP  irp;
	PVOID urb;
	ULONG stopmask;
	int   failed;
	int   owns;
	int   last;

	if (Slot >= ADAPTOID_POLL_SLOTS) {
		return;
	}
	failed = (!NT_SUCCESS(Status) || Length != ADAPTOID_POLL_BYTES);

	KeAcquireSpinLock(&DevExt->PollLock, &irql);
	irp = DevExt->PollSlot[Slot].Irp;
	urb = DevExt->PollSlot[Slot].Urb;
	DevExt->PollSlot[Slot].Irp = NULL;
	DevExt->PollSlot[Slot].Urb = NULL;

	owns = (DevExt->PollSlot[Slot].CancelLatch == 0);
	if (!owns) {
		DevExt->PollSlot[Slot].CancelLatch = 0;
	}
	stopmask = DevExt->PollStopMask;
	KeReleaseSpinLock(&DevExt->PollLock, irql);

	if (owns) {
		AdaptoidFreePollIrp(irp, urb);
	}

	if (stopmask == 0 && !failed) {
		/*
		 * The whole point: decode, then immediately put the slot back
		 * in flight so the other one is never alone.
		 *
		 * THE CLOCK IS ADVANCED FROM HERE, which makes the interrupt
		 * pipe the driver's time base - the effect ring and the script
		 * scheduler both run off packets arriving at roughly 8ms. That
		 * is the original's arrangement and it has a property worth
		 * keeping: with no controller attached, nothing ticks.
		 */
		core_on_raw_packet(&DevExt->Core, DevExt->PollSlot[Slot].Buffer);
		core_tick(&DevExt->Core, KeQueryInterruptTime());
		AdaptoidPollSubmit(DevExt, Slot);
		return;
	}

	last = AdaptoidPollStop(DevExt, failed ? ADAPTOID_STOP_REASON_ERROR : 0,
	                        Slot);
	if (last && failed &&
	    NT_SUCCESS(AdaptoidLockAcquire(&DevExt->RemoveLockB))) {
		/*
		 * The remove lock is held ACROSS the queued work, not just around
		 * queueing it; the restart worker releases it. That is what keeps
		 * the device alive until the retry finishes.
		 */
		DevExt->PollRestartPending = 1;
		AdaptoidQueuePollRestart(DevExt);
	}
}

/* ------------------------------------------------------------------ */
/* the report queue and pending reads                                  */
/* ------------------------------------------------------------------ */

/* Take the oldest parked read that cancellation has not claimed. */
PIRP AdaptoidDequeueRead(PADAPTOID_DEVEXT DevExt)
{
	KIRQL irql;
	PIRP  irp = NULL;

	for (;;) {
		PLIST_ENTRY entry = NULL;

		KeAcquireSpinLock(&DevExt->ReportLock, &irql);
		if (DevExt->PendingReads.Flink != &DevExt->PendingReads) {
			entry = DevExt->PendingReads.Flink;
			entry->Blink->Flink = entry->Flink;
			entry->Flink->Blink = entry->Blink;
			DevExt->PendingReadCount--;
		}
		KeReleaseSpinLock(&DevExt->ReportLock, irql);

		if (entry == NULL) {
			return NULL;
		}
		irp = ADAPTOID_IRP_FROM_ENTRY(entry);
		if (AdaptoidClaimIrp(irp)) {
			return irp;
		}
		/* Cancellation won; it owns completing that one. Try the next. */
	}
}

/* Park a read, with the cancel handshake. */
void AdaptoidQueueRead(PADAPTOID_DEVEXT DevExt, PIRP Irp)
{
	KIRQL irql;
	PLIST_ENTRY entry = ADAPTOID_IRP_LIST_ENTRY(Irp);

	KeAcquireSpinLock(&DevExt->ReportLock, &irql);
	entry->Flink = &DevExt->PendingReads;
	entry->Blink = DevExt->PendingReads.Blink;
	DevExt->PendingReads.Blink->Flink = entry;
	DevExt->PendingReads.Blink = entry;
	DevExt->PendingReadCount++;
	KeReleaseSpinLock(&DevExt->ReportLock, irql);
}

/* Fail every parked read. The device going away does this. */
void AdaptoidCancelPendingReads(PADAPTOID_DEVEXT DevExt)
{
	PIRP irp;

	while ((irp = AdaptoidDequeueRead(DevExt)) != NULL) {
		AdaptoidCompleteIrp(irp, STATUS_DELETE_PENDING, 0);
	}
}

/*
 * IOCTL_HID_READ_REPORT. Answer from the queue if anything is waiting there,
 * otherwise park until something arrives.
 *
 * The two halves are deliberately in this order: a report already queued is
 * older than this request, so serving it first keeps reports in order.
 */
NTSTATUS AdaptoidReadReport(PADAPTOID_DEVEXT DevExt, PIRP Irp)
{
	KIRQL irql;
	ADAPTOID_REPORT_NODE node;
	int have = 0;

	KeAcquireSpinLock(&DevExt->ReportLock, &irql);
	if (DevExt->ReportCount > 0) {
		node = DevExt->ReportQueue[DevExt->ReportHead];
		DevExt->ReportHead =
		        (DevExt->ReportHead + 1) % ADAPTOID_REPORT_QUEUE_MAX;
		DevExt->ReportCount--;
		have = 1;
	}
	KeReleaseSpinLock(&DevExt->ReportLock, irql);

	if (have) {
		return AdaptoidCompleteRead(DevExt, Irp, node.Data, node.Length);
	}

	if (!NT_SUCCESS(AdaptoidLockAcquire(&DevExt->RemoveLockB))) {
		return AdaptoidCompleteIrp(Irp, STATUS_DELETE_PENDING, 0),
		       STATUS_DELETE_PENDING;
	}
	AdaptoidQueueRead(DevExt, Irp);
	return STATUS_PENDING;
}

/*
 * The report sink. core_emit has already decided the report is enabled and
 * stripped the length and ID; what arrives here is the payload.
 *
 * Hand it straight to a waiting read if there is one, otherwise queue it.
 *
 * A FULL QUEUE DISCARDS THE NEW REPORT and keeps the old ones, which is the
 * original's policy and the opposite of the notification queue's. See the
 * note in wdm.h.
 */
void AdaptoidReportSink(void *ctx, u8 report_id, const u8 *data, u32 len)
{
	PADAPTOID_DEVEXT DevExt = (PADAPTOID_DEVEXT)ctx;
	KIRQL irql;
	PIRP  irp;
	ULONG i;
	LONG  slot;

	if (len + 1 > CORE_REPORT_MAX_BYTES) {
		return;
	}

	irp = AdaptoidDequeueRead(DevExt);
	if (irp != NULL) {
		UCHAR buf[CORE_REPORT_MAX_BYTES];

		buf[0] = report_id;
		for (i = 0; i < len; i++) {
			buf[1 + i] = data[i];
		}
		AdaptoidCompleteRead(DevExt, irp, buf, (UCHAR)(len + 1));
		AdaptoidLockRelease(&DevExt->RemoveLockB);
		return;
	}

	KeAcquireSpinLock(&DevExt->ReportLock, &irql);
	if (DevExt->ReportCount >= ADAPTOID_REPORT_QUEUE_MAX) {
		DevExt->ReportsDropped++;
		KeReleaseSpinLock(&DevExt->ReportLock, irql);
		return;
	}
	slot = (DevExt->ReportHead + DevExt->ReportCount) %
	       ADAPTOID_REPORT_QUEUE_MAX;
	DevExt->ReportQueue[slot].Length  = (UCHAR)(len + 1);
	DevExt->ReportQueue[slot].Data[0] = report_id;
	for (i = 0; i < len; i++) {
		DevExt->ReportQueue[slot].Data[1 + i] = data[i];
	}
	DevExt->ReportCount++;
	KeReleaseSpinLock(&DevExt->ReportLock, irql);
}

/* ------------------------------------------------------------------ */
/* device naming                                                       */
/* ------------------------------------------------------------------ */

/*
 * Walk one hub looking for the adapter, and write the port path into Path.
 *
 * The path is built OUTERMOST FIRST - this hub's port digit, then whatever
 * the recursion below found - which is the same order the original produces
 * by prepending on the way back up, and the readable one: it reads from the
 * controller inwards.
 *
 * Returns the number of digits written, or -1 if the device is not under
 * this hub. A depth that would not fit is treated as not found rather than
 * truncated, because half a path names the wrong port.
 */
static int FindOnHub(const ADAPTOID_TOPOLOGY *Topo, const WCHAR *HubName,
                     USHORT UsbAddress, char *Path, ULONG PathBytes)
{
	ULONG ports = 0;
	ULONG port;

	if (PathBytes == 0) {
		return -1;
	}
	if (!NT_SUCCESS(Topo->HubPorts(Topo->Context, HubName, &ports))) {
		return -1;
	}

	for (port = 1; port <= ports; port++) {
		ADAPTOID_PORT_INFO info;
		int deeper;

		info.Connected     = 0;
		info.IsHub         = 0;
		info.VendorId      = 0;
		info.ProductId     = 0;
		info.DeviceAddress = 0;
		info.ChildHubName  = NULL;

		if (!NT_SUCCESS(Topo->PortInfo(Topo->Context, HubName, port,
		                               &info))) {
			continue;
		}
		if (!info.Connected) {
			continue;
		}

		if (!info.IsHub) {
			/*
			 * A leaf. It is us only if the vendor, product AND bus
			 * address all match - the address is what distinguishes two
			 * identical adapters, and without it both would be given the
			 * same name.
			 */
			if (info.VendorId == ADAPTOID_VENDOR_ID &&
			    info.ProductId == ADAPTOID_PRODUCT_ID &&
			    info.DeviceAddress == UsbAddress) {
				Path[0] = (char)('0' + (port % 10));
				return 1;
			}
			continue;
		}

		if (info.ChildHubName == NULL) {
			continue;
		}
		deeper = FindOnHub(Topo, info.ChildHubName, UsbAddress,
		                   Path + 1, PathBytes - 1);
		if (deeper >= 0) {
			Path[0] = (char)('0' + (port % 10));
			return deeper + 1;
		}
	}
	return -1;
}

int AdaptoidBuildLocationName(const ADAPTOID_TOPOLOGY *Topo,
                              USHORT UsbAddress, char *Name, ULONG NameBytes)
{
	ULONG index;

	/* Every seam, not just the struct: an extension whose topology was
	 * never installed must be unnameable, not fatal. */
	if (Topo == NULL || Topo->RootHub == NULL || Topo->HubPorts == NULL ||
	    Topo->PortInfo == NULL || Name == NULL || NameBytes < 2) {
		return 0;
	}
	Name[0] = 0;

	for (index = 0; index < ADAPTOID_MAX_CONTROLLERS; index++) {
		const WCHAR *root = NULL;
		int digits;

		if (!NT_SUCCESS(Topo->RootHub(Topo->Context, index, &root)) ||
		    root == NULL) {
			continue;
		}
		/* One byte for the controller letter, one for the NUL. */
		digits = FindOnHub(Topo, root, UsbAddress, Name + 1,
		                   NameBytes - 2);
		if (digits >= 0) {
			Name[0] = (char)('A' + index);
			Name[1 + digits] = 0;
			return 1;
		}
	}
	return 0;
}

/*
 * Set this device's display name, or "?" if it could not be located.
 *
 * THE COPY IS BOUNDED. The original builds the name in pool and then writes
 * it into a ten-byte field with a plain strcpy that has no length parameter
 * at all - past that field sit the firmware value and then the LIST_ENTRY
 * threading this device onto the driver-wide list. See known-defects.txt
 * section 6. Here the name is built straight into the destination, with its
 * size, so there is no second copy to get wrong.
 */
void AdaptoidSetDeviceName(PADAPTOID_DEVEXT DevExt)
{
	if (!AdaptoidBuildLocationName(&DevExt->Topology, DevExt->UsbAddress,
	                               DevExt->Core.device_name,
	                               CORE_DEVICE_NAME_BYTES)) {
		DevExt->Core.device_name[0] = '?';
		DevExt->Core.device_name[1] = 0;
	}
}

/* ------------------------------------------------------------------ */
/* USB port recovery                                                   */
/* ------------------------------------------------------------------ */

/*
 * Try to get the port back.
 *
 * A reset is worth attempting only when the device is STILL CONNECTED but
 * the port has been DISABLED - that is a port the bus driver shut down under
 * a device that is still there. Anything else, including the device having
 * been unplugged, is past recovering, so the port is cycled to force a fresh
 * enumeration and the caller is told not to retry.
 */
NTSTATUS AdaptoidRecoverPort(PADAPTOID_DEVEXT DevExt)
{
	ULONG status = 0;
	NTSTATUS st;

	st = AdaptoidUsbGetPortStatus(DevExt, &status);
	if (NT_SUCCESS(st) &&
	    (status & ADAPTOID_PORT_ENABLED) == 0 &&
	    (status & ADAPTOID_PORT_CONNECTED) != 0) {
		return AdaptoidUsbResetPort(DevExt);
	}

	AdaptoidUsbCyclePort(DevExt);
	return ADAPTOID_STATUS_GAVE_UP;
}

/*
 * The retry ladder, run on a work item because it blocks.
 *
 * Up to three recoveries; the first that succeeds restarts polling and the
 * ladder stops. ADAPTOID_STATUS_GAVE_UP stops it too, WITHOUT cycling again,
 * because the recovery already did.
 *
 * The remove lock this releases was taken by the completion that queued the
 * work, so the device is pinned for the whole ladder and not merely while it
 * was being queued.
 */
void AdaptoidPollRestartWorker(PADAPTOID_DEVEXT DevExt)
{
	NTSTATUS st = STATUS_SUCCESS;
	int tries   = ADAPTOID_RECOVER_TRIES;
	int pending = (DevExt->PollRestartPending != 0);

	DevExt->PollRestartPending = 0;

	while (pending && tries > 0 && st != ADAPTOID_STATUS_GAVE_UP) {
		st = AdaptoidRecoverPort(DevExt);
		if (NT_SUCCESS(st)) {
			AdaptoidPollStart(DevExt, ADAPTOID_STOP_REASON_ERROR);
			pending = 0;
		}
		tries--;
	}

	if (pending && st != ADAPTOID_STATUS_GAVE_UP) {
		AdaptoidUsbCyclePort(DevExt);
	}
	AdaptoidLockRelease(&DevExt->RemoveLockB);
}

/* ------------------------------------------------------------------ */
/* power                                                               */
/* ------------------------------------------------------------------ */

int AdaptoidIsDeviceReady(PADAPTOID_DEVEXT DevExt)
{
	return DevExt != NULL &&
	       DevExt->Started != 0 &&
	       DevExt->Removing == 0 &&
	       DevExt->RemovePending == 0 &&
	       DevExt->StopPending == 0;
}

ULONG AdaptoidDeviceStateFor(PADAPTOID_DEVEXT DevExt, ULONG SystemState)
{
	if (SystemState == ADAPTOID_POWER_S0) {
		return ADAPTOID_POWER_D0;
	}
	/*
	 * NO WAKE ARMED MEANS D3, whatever the capability table says. This is
	 * the rule that is easy to miss: without an outstanding WAIT_WAKE
	 * there is nothing a light sleep buys, so the device goes all the way
	 * off rather than to the capability's D-state.
	 */
	if (DevExt->WaitWakePending == 0) {
		return ADAPTOID_POWER_D3;
	}
	/*
	 * BOUNDS CHECKED, which the original is not: it indexes DeviceState
	 * with the system state straight off the stack location. Nothing
	 * below the driver sends a bad one, so it is not a live bug, but the
	 * check costs nothing and the array is only seven entries.
	 */
	if (SystemState >= ADAPTOID_SYSTEM_STATE_MAX) {
		return ADAPTOID_POWER_D3;
	}
	return DevExt->Capabilities.DeviceState[SystemState];
}

int AdaptoidPrepareDevicePower(PADAPTOID_DEVEXT DevExt, ULONG State)
{
	if (State == ADAPTOID_POWER_D0) {
		/*
		 * Coming up. Nothing may be restarted until the bus driver has
		 * actually applied power, so the work belongs in the
		 * completion - which is what the non-zero return asks for.
		 */
		return 1;
	}
	if (State > ADAPTOID_POWER_D0 && State <= ADAPTOID_POWER_D3) {
		/*
		 * Going down, and it is done HERE, before the IRP is passed
		 * on. Stopping the poll after the bus driver had removed power
		 * would be stopping it against a device that is already gone.
		 */
		DevExt->DevicePowerState = State;
		AdaptoidPollStop(DevExt, ADAPTOID_STOP_REASON_POWER,
		                 ADAPTOID_POLL_SLOTS);
		AdaptoidQuiesceIo(DevExt);
		return 0;
	}
	/* Not a state this driver knows; pass it down bare. */
	return 0;
}

/*
 * The completion for a transition into D0. Running here rather than in
 * AdaptoidPrepareDevicePower is the whole point: the device is powered by
 * the time this is called, so polling can genuinely start again.
 */
static NTSTATUS NTAPI PowerUpComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                                      PVOID Context)
{
	PADAPTOID_DEVEXT dx = (PADAPTOID_DEVEXT)Context;

	UNREFERENCED_PARAMETER(DeviceObject);

	dx->DevicePowerState = ADAPTOID_POWER_D0;
	AdaptoidPollStart(dx, ADAPTOID_STOP_REASON_POWER);
	Irp->IoStatus.Status = STATUS_SUCCESS;
	AdaptoidLockRelease(&dx->RemoveLockB);
	return STATUS_SUCCESS;
}

/*
 * The completion for the DEVICE power IRP this driver asked for on behalf of
 * a SYSTEM power IRP. The system IRP was parked; now that the device has
 * moved, it can be finished.
 */
void NTAPI AdaptoidSystemPowerComplete(PDEVICE_OBJECT DeviceObject,
                                       UCHAR MinorFunction,
                                       POWER_STATE PowerState,
                                       PVOID Context,
                                       PIO_STATUS_BLOCK IoStatus)
{
	PADAPTOID_DEVEXT dx = (PADAPTOID_DEVEXT)Context;
	PIRP             irp;

	UNREFERENCED_PARAMETER(DeviceObject);
	UNREFERENCED_PARAMETER(MinorFunction);
	UNREFERENCED_PARAMETER(PowerState);
	UNREFERENCED_PARAMETER(IoStatus);

	irp = dx->PendingSystemPowerIrp;
	dx->PendingSystemPowerIrp = NULL;
	if (irp == NULL) {
		return;
	}
	PoStartNextPowerIrp(irp);
	IoCopyCurrentIrpStackLocationToNext(irp);
	PoCallDriver(dx->NextDeviceObject, irp);
	AdaptoidLockRelease(&dx->RemoveLockB);
}

/* The completion for an idle transition this driver asked for itself. */
void NTAPI AdaptoidIdlePowerComplete(PDEVICE_OBJECT DeviceObject,
                                     UCHAR MinorFunction,
                                     POWER_STATE PowerState,
                                     PVOID Context, PIO_STATUS_BLOCK IoStatus)
{
	PADAPTOID_DEVEXT dx = (PADAPTOID_DEVEXT)Context;

	UNREFERENCED_PARAMETER(DeviceObject);
	UNREFERENCED_PARAMETER(MinorFunction);
	UNREFERENCED_PARAMETER(PowerState);
	UNREFERENCED_PARAMETER(IoStatus);

	dx->PowerRequestInProgress = 0;
}

NTSTATUS AdaptoidRequestDevicePower(PADAPTOID_DEVEXT DevExt, ULONG State)
{
	DevExt->PowerRequestInProgress = 1;
	return AdaptoidRequestPowerIrp(DevExt, State,
	                               AdaptoidIdlePowerComplete);
}

NTSTATUS AdaptoidUpdateIdlePower(PADAPTOID_DEVEXT DevExt, int GoIdle)
{
	ULONG target;

	if (!AdaptoidIsDeviceReady(DevExt)) {
		return STATUS_DELETE_PENDING;
	}
	if (DevExt->PendingSystemPowerIrp != NULL ||
	    DevExt->PowerRequestInProgress != 0) {
		return STATUS_SUCCESS;
	}
	/*
	 * THE DEAD GATE. Idling needs AbortedPipeCount zero and waking needs
	 * it non-zero, and NOTHING ANYWHERE INCREMENTS IT - so the wake half
	 * is unreachable and the idle half is a formality. Kept because a
	 * replacement that drops it changes when the device powers down.
	 */
	if (GoIdle) {
		if (DevExt->AbortedPipeCount != 0) {
			return STATUS_SUCCESS;
		}
	} else if (DevExt->AbortedPipeCount == 0) {
		return STATUS_SUCCESS;
	}

	target = DevExt->WakeIdleDeviceState;
	if (target == 0 || target == ADAPTOID_POWER_D0 ||
	    target > ADAPTOID_POWER_D3) {
		return STATUS_SUCCESS;
	}
	if (!GoIdle) {
		target = ADAPTOID_POWER_D0;
	}
	return AdaptoidRequestDevicePower(DevExt, target);
}

/*
 * IRP_MN_WAIT_WAKE.
 *
 * Accepted only when the device is OUT of D0 and the machine is no deeper
 * than the state the device can wake from - asking a powered device to arm
 * remote wake is meaningless, and arming for a state past what the hardware
 * supports would silently never fire.
 *
 * The IRP is passed down and WAITED FOR, which is legitimate here and only
 * here: a wake request stays outstanding until the wake happens, so this
 * returns when it has.
 */
NTSTATUS AdaptoidPowerWaitWake(PADAPTOID_DEVEXT DevExt, PIRP Irp)
{
	KEVENT   done;
	NTSTATUS st;

	DevExt->WakeIdleDeviceState = DevExt->Capabilities.DeviceWake;

	if (DevExt->DevicePowerState == ADAPTOID_POWER_D0 ||
	    (LONG)DevExt->Capabilities.DeviceWake >
	    (LONG)DevExt->DevicePowerState) {
		AdaptoidLockRelease(&DevExt->RemoveLockB);
		PoStartNextPowerIrp(Irp);
		AdaptoidCompleteIrp(Irp, STATUS_INVALID_DEVICE_STATE, 0);
		return STATUS_INVALID_DEVICE_STATE;
	}

	DevExt->WaitWakePending = 1;
	KeInitializeEvent(&done, NotificationEvent, FALSE);
	IoCopyCurrentIrpStackLocationToNext(Irp);
	AdaptoidSetCompletionRoutine(Irp, &done);
	PoStartNextPowerIrp(Irp);
	st = PoCallDriver(DevExt->NextDeviceObject, Irp);
	if (st == STATUS_PENDING) {
		KeWaitForSingleObject(&done, Executive, KernelMode, FALSE,
		                      NULL);
	}
	/* The wake has happened, or the request was cancelled. Either way the
	 * device may now need to come back up. */
	AdaptoidUpdateIdlePower(DevExt, 0);
	DevExt->WaitWakePending = 0;
	AdaptoidLockRelease(&DevExt->RemoveLockB);
	return st;
}

/*
 * IRP_MJ_POWER for an adapter.
 *
 * EVERY PATH CALLS PoStartNextPowerIrp BEFORE PoCallDriver. That is the
 * power protocol and it is not optional - the original observes it here and
 * notably does not in its control-device path.
 */
NTSTATUS NTAPI AdaptoidPower(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PADAPTOID_DEVEXT   dx = AdaptoidDevExtOf(DeviceObject);
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
	NTSTATUS           st;

	st = AdaptoidLockAcquire(&dx->RemoveLockB);
	if (!NT_SUCCESS(st)) {
		PoStartNextPowerIrp(Irp);
		AdaptoidCompleteIrp(Irp, st, 0);
		return st;
	}

	if (sl->MinorFunction == IRP_MN_WAIT_WAKE) {
		return AdaptoidPowerWaitWake(dx, Irp);
	}

	if (sl->MinorFunction == IRP_MN_SET_POWER &&
	    sl->Parameters.Power.Type == SystemPowerState) {
		ULONG want = AdaptoidDeviceStateFor(dx,
		        (ULONG)sl->Parameters.Power.State.SystemState);

		if (want != dx->DevicePowerState) {
			/*
			 * PARK THE SYSTEM IRP and ask for the device IRP the
			 * mapping calls for. The system transition is not
			 * finished until the device one is - which is why the
			 * remove lock is NOT released here, and why the
			 * completion does it instead.
			 */
			dx->PendingSystemPowerIrp = Irp;
			return AdaptoidRequestPowerIrp(dx, want,
			                        AdaptoidSystemPowerComplete);
		}
		/* Already in the right state; fall through and pass it down. */
	} else if (sl->MinorFunction == IRP_MN_SET_POWER &&
	           sl->Parameters.Power.Type == DevicePowerState) {
		int completes = AdaptoidPrepareDevicePower(dx,
		        (ULONG)sl->Parameters.Power.State.DeviceState);

		IoCopyCurrentIrpStackLocationToNext(Irp);
		if (completes) {
			IoSetCompletionRoutine(Irp, PowerUpComplete, dx,
			                       TRUE, TRUE, TRUE);
		}
		PoStartNextPowerIrp(Irp);
		st = PoCallDriver(dx->NextDeviceObject, Irp);
		if (!completes) {
			/* With no completion routine nothing else will release
			 * the lock, so it is released here. */
			AdaptoidLockRelease(&dx->RemoveLockB);
		}
		return st;
	}

	IoCopyCurrentIrpStackLocationToNext(Irp);
	PoStartNextPowerIrp(Irp);
	st = PoCallDriver(dx->NextDeviceObject, Irp);
	AdaptoidLockRelease(&dx->RemoveLockB);
	return st;
}

/* ------------------------------------------------------------------ */
/* the device enable, and its keep-alive window                        */
/* ------------------------------------------------------------------ */

/*
 * One wire format for the idle command, shared with the deferred drain -
 * core_effect_send_idle emits it through the same vendor seam this layer
 * installs, so there is no second copy of the setup packet to drift.
 */
void AdaptoidSendIdleCommand(PADAPTOID_DEVEXT DevExt)
{
	core_effect_send_idle(&DevExt->Core);
}

/*
 * The completion of the full start sequence: send the same short kick the
 * already-running path sends, so both routes end at the same register write.
 * Returning non-zero holds the vendor slot, because this has started more
 * work on it.
 */
static int EnableSecondStage(PADAPTOID_DEVEXT DevExt)
{
	ADAPTOID_SETUP setup;

	setup.bmRequestType = ADAPTOID_VENDOR_OUT;
	setup.bRequest      = ADAPTOID_ENABLE_KICK_REQUEST;
	setup.wValue        = ADAPTOID_ENABLE_KICK_VALUE;
	setup.wIndex        = ADAPTOID_ENABLE_KICK_INDEX;
	AdaptoidVendorSend(DevExt, &setup, 0, NULL, NULL);
	return 1;
}

NTSTATUS AdaptoidSetDeviceEnable(PADAPTOID_DEVEXT DevExt, int On)
{
	ADAPTOID_SETUP setup;
	ULONGLONG      now;

	if (!AdaptoidVendorTryClaim(DevExt)) {
		/*
		 * THE ASYMMETRY IS DELIBERATE. Switching off is deferred so it
		 * cannot be lost - a missed "off" leaves a motor running -
		 * while switching on is simply refused, because the next
		 * effect tick will ask again.
		 */
		if (!On) {
			DevExt->Core.claim_idle_command = 1;
		}
		return STATUS_DEVICE_BUSY;
	}

	if (!On) {
		AdaptoidSendIdleCommand(DevExt);
		return STATUS_SUCCESS;
	}

	now = KeQueryInterruptTime();
	if (DevExt->Core.keepalive_time <= now &&
	    now <= DevExt->Core.keepalive_time + ADAPTOID_KEEPALIVE_100NS) {
		/* Still inside the window: the adapter is already running, so
		 * one short kick is enough and it is not re-initialised. */
		setup.bmRequestType = ADAPTOID_VENDOR_OUT;
		setup.bRequest      = ADAPTOID_ENABLE_KICK_REQUEST;
		setup.wValue        = ADAPTOID_ENABLE_KICK_VALUE;
		setup.wIndex        = ADAPTOID_ENABLE_KICK_INDEX;
		AdaptoidVendorSend(DevExt, &setup, 0, NULL, NULL);
		return STATUS_SUCCESS;
	}

	/* Outside it: the full start, whose completion sends the kick. */
	DevExt->Core.keepalive_time = now;
	setup.bmRequestType = ADAPTOID_VENDOR_OUT;
	setup.bRequest      = ADAPTOID_ENABLE_START_REQUEST;
	setup.wValue        = ADAPTOID_ENABLE_START_VALUE;
	setup.wIndex        = ADAPTOID_ENABLE_START_INDEX;
	AdaptoidVendorSend(DevExt, &setup, 0, NULL, EnableSecondStage);
	return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* the script scheduler DPC                                            */
/* ------------------------------------------------------------------ */

/*
 * THE DEPTH COUNTER IS NOT A BOOLEAN, and both guards are one-sided: the
 * increment is skipped while it is negative and the decrement while it is
 * non-positive. AdaptoidDevExtInit latches it at -1, so the DPC can never
 * lift it on its own and a script cannot own the stick until something else
 * does. Faithful to drv_ScriptSchedulerDpc (00017910).
 */
void NTAPI AdaptoidScriptDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
	PADAPTOID_DEVEXT dx = (PADAPTOID_DEVEXT)Context;
	KIRQL            irql;

	UNREFERENCED_PARAMETER(Dpc);
	UNREFERENCED_PARAMETER(Arg1);
	UNREFERENCED_PARAMETER(Arg2);

	KeAcquireSpinLock(&dx->ScriptLock, &irql);
	if (dx->ScriptDepth >= 0) {
		dx->ScriptDepth++;
	}
	KeReleaseSpinLock(&dx->ScriptLock, irql);

	core_sched_run(&dx->Sched, KeQueryInterruptTime());

	KeAcquireSpinLock(&dx->ScriptLock, &irql);
	if (dx->ScriptDepth > 0) {
		dx->ScriptDepth--;
	}
	KeReleaseSpinLock(&dx->ScriptLock, irql);
}

/*
 * Ask the adapter where it is on the bus.
 *
 * Vendor request 0x75 returns two bytes and the FIRST is this device's own
 * USB address. It is the key AdaptoidBuildLocationName walks the hub tree
 * looking for, and the only thing that tells two identical adapters apart -
 * vendor and product cannot.
 *
 * SYNCHRONOUS, which is why it may only be called from AdaptoidStartDevice:
 * everything else on this path runs too high to wait.
 *
 * THE ORIGINAL READS THE BYTE THROUGH A SIGNED CHAR, so an address with bit
 * 7 set would come out negative and never match any port. Harmless in
 * practice - USB addresses run 1 to 127 - but there is no reason to copy it,
 * and this does not.
 */
void AdaptoidQueryFirmwareInfo(PADAPTOID_DEVEXT DevExt)
{
	ADAPTOID_SETUP setup;
	UCHAR          reply[2];

	if (!AdaptoidVendorTryClaim(DevExt)) {
		return;
	}
	reply[0] = 0;
	reply[1] = 0;
	setup.bmRequestType = ADAPTOID_VENDOR_IN;
	setup.bRequest      = ADAPTOID_REQUEST_BUS_ADDRESS;
	setup.wValue        = 0;
	setup.wIndex        = 0;

	KeInitializeEvent(&DevExt->Vendor.Done, NotificationEvent, FALSE);
	AdaptoidVendorSend(DevExt, &setup, sizeof(reply), reply, NULL);
	KeWaitForSingleObject(&DevExt->Vendor.Done, Executive, KernelMode,
	                      FALSE, NULL);

	DevExt->UsbAddress = reply[0];
}

/*
 * The minidriver's OWN IRP_MJ_DEVICE_CONTROL, installed before
 * HidRegisterMinidriver so that hidclass calls it - not to be confused with
 * AdaptoidDeviceControl, the triage wrapper installed afterwards.
 *
 * It does nothing but pass the request to the bus driver. hidclass has
 * already answered every code it understands by the time it gets here, and
 * the driver has no device-control codes of its own on this path - its
 * private surface is IRP_MJ_INTERNAL_DEVICE_CONTROL and the control device.
 */
NTSTATUS NTAPI AdaptoidPassThroughDeviceControl(PDEVICE_OBJECT DeviceObject,
                                                PIRP Irp)
{
	PADAPTOID_DEVEXT dx = AdaptoidDevExtOf(DeviceObject);

	IoCopyCurrentIrpStackLocationToNext(Irp);
	return IofCallDriver(dx->NextDeviceObject, Irp);
}

/*
 * Give up on an outstanding vendor request.
 *
 * CANCELLING THE URB IRP IS NOT ENOUGH ON ITS OWN - the slot has to be given
 * back too, and only the completion knows whether it already has. So this
 * asks, and lets the completion do the releasing, exactly as the original
 * does.
 */
void AdaptoidCancelVendorRequest(PADAPTOID_DEVEXT DevExt)
{
	PIRP  irp;
	KIRQL irql;

	KeAcquireSpinLock(&DevExt->Vendor.Lock, &irql);
	irp = (PIRP)DevExt->Vendor.UrbIrp;
	KeReleaseSpinLock(&DevExt->Vendor.Lock, irql);

	if (irp != NULL) {
		AdaptoidCancelIrp(irp);
	}
}

/*
 * Tell user mode that this adapter's interface came up or went down.
 *
 * IT GOES ON THE DRIVER-WIDE NOTIFICATION QUEUE, not to the adapter that
 * changed - core_registry_set_live posts it - which is why an event carries
 * no device identity and a listener has to re-enumerate to find out what
 * actually changed. See ioctl.h.
 */
void AdaptoidNotifyInterfaceChange(PADAPTOID_DEVEXT DevExt, int Live)
{
	PADAPTOID_CDO_EXT cx = AdaptoidControlDeviceExt();

	if (cx == NULL) {
		return;
	}
	core_registry_set_live(&cx->Registry, &DevExt->Registration, Live);
}

/* ------------------------------------------------------------------ */
/* the wiring                                                          */
/*                                                                     */
/* Every subsystem below this driver is OS-free and was tested on its  */
/* own. This is where they are joined to each other and to Windows -   */
/* the seams filled in, the clocks connected, the events routed. It is */
/* short, and until it existed the driver polled a controller and did  */
/* nothing else with it.                                               */
/* ------------------------------------------------------------------ */

/*
 * The core issuing a vendor transfer.
 *
 * THE SLOT IS ALREADY CLAIMED by the time this runs - core.c takes it
 * through AdaptoidCoreVendorClaim before it builds the request, exactly as
 * the original claims before composing its setup packet. So this only
 * submits, and the completion routes the answer back.
 */
static int AdaptoidCoreVendorComplete(PADAPTOID_DEVEXT DevExt)
{
	/*
	 * Returning non-zero holds the slot when the core started more work.
	 * core_vendor_completed says whether it did.
	 */
	return core_vendor_completed(&DevExt->Core,
	                             NT_SUCCESS(DevExt->Vendor.LastStatus),
	                             DevExt->CoreVendorReply,
	                             DevExt->Vendor.LastInformation,
	                             KeQueryInterruptTime());
}

static int AdaptoidCoreVendor(void *ctx, const core_vendor_req *req)
{
	PADAPTOID_DEVEXT dx = (PADAPTOID_DEVEXT)ctx;
	ADAPTOID_SETUP   setup;
	ULONG            len = req->wLength;

	if (len > sizeof(dx->CoreVendorReply)) {
		len = sizeof(dx->CoreVendorReply);
	}
	setup.bmRequestType = req->bmRequestType;
	setup.bRequest      = req->bRequest;
	setup.wValue        = req->wValue;
	setup.wIndex        = req->wIndex;

	return NT_SUCCESS(AdaptoidVendorSend(dx, &setup, len,
	                                     len ? dx->CoreVendorReply : NULL,
	                                     AdaptoidCoreVendorComplete));
}

static int AdaptoidCoreVendorClaim(void *ctx)
{
	return AdaptoidVendorTryClaim((PADAPTOID_DEVEXT)ctx);
}

/*
 * The blocking transport, for the raw N64 transaction alone. PASSIVE_LEVEL
 * only, which is why nothing else uses it - see core_vendor_sync_fn.
 */
static int AdaptoidCoreVendorSync(void *ctx, const core_vendor_req *req,
                                  u8 *data, u32 len)
{
	PADAPTOID_DEVEXT dx = (PADAPTOID_DEVEXT)ctx;
	ADAPTOID_SETUP   setup;

	if (!AdaptoidVendorTryClaim(dx)) {
		return 0;
	}
	setup.bmRequestType = req->bmRequestType;
	setup.bRequest      = req->bRequest;
	setup.wValue        = req->wValue;
	setup.wIndex        = req->wIndex;

	KeInitializeEvent(&dx->Vendor.Done, NotificationEvent, FALSE);
	if (!NT_SUCCESS(AdaptoidVendorSend(dx, &setup, len, data, NULL))) {
		return 0;
	}
	KeWaitForSingleObject(&dx->Vendor.Done, Executive, KernelMode, FALSE,
	                      NULL);
	return NT_SUCCESS(dx->Vendor.LastStatus);
}

/*
 * A script's events, on their way to becoming real HID input.
 *
 * THIS IS THE POINT OF THE WHOLE DRIVER. A script calls _key, and because
 * the event lands in the keyboard report state machine rather than in a
 * user-mode injection API, every application sees a genuine keystroke.
 *
 * The three that are not input - a fault, _debug, an interface change - go
 * to the control device's notification queue for whoever is listening.
 */
static void AdaptoidScriptEvent(void *ctx, u32 type, u32 arg1, u32 arg2)
{
	PADAPTOID_DEVEXT  dx = (PADAPTOID_DEVEXT)ctx;
	PADAPTOID_CDO_EXT cx;

	switch (type) {
	case CORE_EVENT_KEY:
		core_hid_key_event(&dx->Core, arg1, arg2 != 0);
		return;
	case CORE_EVENT_MOUSE_BUTTON:
		core_hid_mouse_button(&dx->Core, arg1, arg2 != 0);
		return;
	case CORE_EVENT_MOUSE_REL:
		core_hid_mouse_move(&dx->Core, (s32)arg1, (s32)arg2, 0);
		return;
	default:
		break;
	}

	/*
	 * Everything else is for user mode. The queue is driver-wide and
	 * lives on the control device, which may not exist - a script can be
	 * running with no client listening, and that is not an error.
	 */
	cx = AdaptoidControlDeviceExt();
	if (cx != NULL) {
		core_notify_post(&cx->Registry.notify, type, arg1, arg2);
	}
}

/*
 * A thread asked to be woken at a particular time.
 *
 * RELATIVE, AND NEGATIVE, which is how the kernel spells "this long from
 * now" as against "at this absolute time". A wake already past arms for the
 * next instant rather than for a time in the past.
 */
static void AdaptoidScriptArm(void *ctx, u64 wake_time)
{
	PADAPTOID_DEVEXT dx  = (PADAPTOID_DEVEXT)ctx;
	ULONGLONG        now = KeQueryInterruptTime();
	LARGE_INTEGER    due;

	due.QuadPart = (wake_time > now) ? -(LONGLONG)(wake_time - now) : 0;
	KeSetTimer(&dx->ScriptTimer, due, &dx->ScriptDpc);
}

/* Script thread stacks. Pool, because their size comes from the script. */
static void *AdaptoidSchedAlloc(void *ctx, u32 bytes)
{
	UNREFERENCED_PARAMETER(ctx);
	return ExAllocatePoolWithTag(NonPagedPool, bytes, ADAPTOID_POOL_TAG);
}

static void AdaptoidSchedFree(void *ctx, void *block)
{
	UNREFERENCED_PARAMETER(ctx);
	if (block != NULL) {
		ExFreePool(block);
	}
}

/* The packet, on its way to any script that wants it. */
static void AdaptoidInputHook(void *ctx, const u8 *raw, u64 now)
{
	PADAPTOID_DEVEXT dx = (PADAPTOID_DEVEXT)ctx;

	core_sched_on_input(&dx->Sched, raw, now);
}

/* And the clock. */
static void AdaptoidTickHook(void *ctx, u64 now)
{
	PADAPTOID_DEVEXT dx = (PADAPTOID_DEVEXT)ctx;

	core_sched_run(&dx->Sched, now);
}

/* The two per-device IOCTL seams the control device forwards through. */
static u32 AdaptoidIoctlVendor(void *ctx, const u8 *setup, u8 *data,
                               u32 data_len)
{
	PADAPTOID_DEVEXT dx = (PADAPTOID_DEVEXT)ctx;
	core_vendor_req  req;

	req.bmRequestType = setup[0];
	req.bRequest      = setup[1];
	req.wValue        = (u16)(setup[2] | ((u16)setup[3] << 8));
	req.wIndex        = (u16)(setup[4] | ((u16)setup[5] << 8));
	req.wLength       = (u16)data_len;

	return AdaptoidCoreVendorSync(dx, &req, data, data_len)
	       ? CORE_ST_SUCCESS : CORE_ST_DEVICE_BUSY;
}

static void AdaptoidIoctlEnable(void *ctx, int on)
{
	AdaptoidSetDeviceEnable((PADAPTOID_DEVEXT)ctx, on);
}

/* And the two the SDK command block uses. */
static int AdaptoidCmdClaim(void *ctx)
{
	return AdaptoidVendorTryClaim((PADAPTOID_DEVEXT)ctx);
}

static u32 AdaptoidCmdXfer(void *ctx, const u8 *setup, u8 *data, u32 len,
                           int keep)
{
	PADAPTOID_DEVEXT dx = (PADAPTOID_DEVEXT)ctx;
	ADAPTOID_SETUP   s;

	s.bmRequestType = setup[0];
	s.bRequest      = setup[1];
	s.wValue        = (USHORT)(setup[2] | ((USHORT)setup[3] << 8));
	s.wIndex        = (USHORT)(setup[4] | ((USHORT)setup[5] << 8));

	KeInitializeEvent(&dx->Vendor.Done, NotificationEvent, FALSE);
	AdaptoidVendorSend(dx, &s, len, data, NULL);
	KeWaitForSingleObject(&dx->Vendor.Done, Executive, KernelMode, FALSE,
	                      NULL);

	/*
	 * keep says another transfer follows and the slot must survive into
	 * it. The completion has already released it, so it is taken again -
	 * which is not the original's mechanism but has the same effect and
	 * cannot deadlock if the caller never sends the second half.
	 */
	if (keep) {
		AdaptoidVendorTryClaim(dx);
	}
	return NT_SUCCESS(dx->Vendor.LastStatus) ? CORE_ST_SUCCESS
	                                         : CORE_ST_DEVICE_BUSY;
}

/*
 * Join one adapter to everything.
 *
 * ORDER MATTERS in two places. The core has to exist before anything is
 * installed into it, and the registry entry has to be filled before it is
 * published - the moment it is on the list, a control-device request can
 * find it.
 */
void AdaptoidWireDevice(PADAPTOID_DEVEXT DevExt)
{
	core_init(&DevExt->Core, AdaptoidReportSink, DevExt);

	/* The transport, in its three shapes. */
	core_set_vendor(&DevExt->Core, AdaptoidCoreVendor, DevExt);
	core_set_vendor_claim(&DevExt->Core, AdaptoidCoreVendorClaim);
	core_set_vendor_sync(&DevExt->Core, AdaptoidCoreVendorSync);

	/* The clock and the packet. */
	core_set_input_hook(&DevExt->Core, AdaptoidInputHook, DevExt);
	core_set_tick_hook(&DevExt->Core, AdaptoidTickHook, DevExt);

	/* The script engine. */
	core_sched_init(&DevExt->Sched, AdaptoidSchedAlloc, AdaptoidSchedFree,
	                DevExt);
	core_sched_set_core(&DevExt->Sched, &DevExt->Core);
	core_sched_set_arm(&DevExt->Sched, AdaptoidScriptArm, DevExt);
	core_sched_set_event_sink(&DevExt->Sched, AdaptoidScriptEvent, DevExt);

	/* This adapter's row in the driver-wide registry. */
	DevExt->Registration.handle    = (u32)(ULONG_PTR)DevExt->Self;
	DevExt->Registration.cs        = &DevExt->Core;
	DevExt->Registration.sched     = &DevExt->Sched;
	DevExt->Registration.live      = 0;
	DevExt->Registration.vendor    = AdaptoidIoctlVendor;
	DevExt->Registration.enable    = AdaptoidIoctlEnable;
	DevExt->Registration.cmd_claim = AdaptoidCmdClaim;
	DevExt->Registration.cmd_xfer  = AdaptoidCmdXfer;
	DevExt->Registration.os_ctx    = DevExt;
}

/* ------------------------------------------------------------------ */
/* the control device object                                           */
/* ------------------------------------------------------------------ */

PADAPTOID_CDO_EXT AdaptoidControlDeviceExt(void)
{
	if (g_ControlDevice == NULL) {
		return NULL;
	}
	return (PADAPTOID_CDO_EXT)g_ControlDevice->DeviceExtension;
}

/*
 * THE REFERENCE COUNT IS INCREMENTED ONLY ON SUCCESS, which the original
 * does not do: drv_CreateControlDevice bumps it unconditionally, so an
 * adapter arriving while creation fails still counts as a user of a device
 * that was never made. See known-defects.txt.
 */
NTSTATUS AdaptoidCreateControlDevice(PDRIVER_OBJECT DriverObject)
{
	NTSTATUS       st = STATUS_SUCCESS;
	PDEVICE_OBJECT dev = NULL;
	UNICODE_STRING name, link;

	ExAcquireFastMutex(&g_ControlMutex);
	if (g_ControlDevice == NULL) {
		RtlInitUnicodeString(&name, ADAPTOID_CDO_NAME);
		st = IoCreateDevice(DriverObject, sizeof(ADAPTOID_CDO_EXT),
		                    &name, ADAPTOID_CDO_DEVICE_TYPE, 0, FALSE,
		                    &dev);
		if (NT_SUCCESS(st)) {
			RtlInitUnicodeString(&link, ADAPTOID_CDO_LINK);
			st = IoCreateSymbolicLink(&link, &name);
			if (!NT_SUCCESS(st)) {
				IoDeleteDevice(dev);
			} else {
				PADAPTOID_CDO_EXT cx =
				      (PADAPTOID_CDO_EXT)dev->DeviceExtension;

				cx->Magic[0] = ADAPTOID_CDO_MAGIC0;
				cx->Magic[1] = ADAPTOID_CDO_MAGIC1;
				cx->Magic[2] = ADAPTOID_CDO_MAGIC2;
				cx->Self      = dev;
				cx->OpenCount = 0;
				KeInitializeSpinLock(&cx->Lock);
				core_registry_init(&cx->Registry);
				core_cmd_channel_init(&cx->Channel);
				AdaptoidNotifyInit(cx);

				/* METHOD_BUFFERED on every private code, so
				 * the object has to be buffered too. */
				dev->Flags |= DO_BUFFERED_IO;
				g_ControlDevice = dev;
				dev->Flags &= ~DO_DEVICE_INITIALIZING;
			}
		}
	}
	if (NT_SUCCESS(st)) {
		g_ControlRefCount++;
	}
	ExReleaseFastMutex(&g_ControlMutex);
	return st;
}

/*
 * Delete the singleton if nothing needs it any more - no adapter holding a
 * reference AND no handle open. Called from both sides, because either can
 * be the one that reaches zero last: the original tests the same pair in
 * drv_ControlDeviceClose and in the adapter teardown.
 */
void AdaptoidControlMaybeDelete(void)
{
	PDEVICE_OBJECT dev = NULL;

	ExAcquireFastMutex(&g_ControlMutex);
	if (g_ControlRefCount == 0 && g_ControlDevice != NULL &&
	    ((PADAPTOID_CDO_EXT)g_ControlDevice->DeviceExtension)->OpenCount
	     == 0) {
		dev = g_ControlDevice;
		/*
		 * THE POINTER IS CLEARED UNDER THE MUTEX so nothing can find
		 * the device while it is being torn down. The original clears
		 * it in the same order, which is what opens the window its
		 * drv_AcquireControlDeviceExt leaks a remove lock into - here
		 * nothing takes a lock merely to look the device up, so there
		 * is no lock to leak.
		 */
		g_ControlDevice = NULL;
	}
	ExReleaseFastMutex(&g_ControlMutex);

	if (dev != NULL) {
		PADAPTOID_CDO_EXT cx = (PADAPTOID_CDO_EXT)dev->DeviceExtension;
		UNICODE_STRING    link;

		RtlInitUnicodeString(&link, ADAPTOID_CDO_LINK);
		IoDeleteSymbolicLink(&link);
		AdaptoidCancelNotifications(cx, NULL);
		core_notify_flush(&cx->Registry.notify);
		IoDeleteDevice(dev);
	}
}

void AdaptoidReleaseControlDevice(void)
{
	ExAcquireFastMutex(&g_ControlMutex);
	if (g_ControlRefCount > 0) {
		g_ControlRefCount--;
	}
	ExReleaseFastMutex(&g_ControlMutex);
	AdaptoidControlMaybeDelete();
}

/* ------------------------------------------------------------------ */
/* the notification waiter                                             */
/* ------------------------------------------------------------------ */

/*
 * The three core_notify seams. The queue owns the ordering and the cap; what
 * is here is only "is this request still ours" and "hand it the bytes".
 */
static int NotifyClaim(void *ctx, core_notify_waiter *w)
{
	UNREFERENCED_PARAMETER(ctx);
	/* The same interlocked claim the report queue uses: whoever clears
	 * the cancel routine owns completing the request. */
	return AdaptoidClaimIrp((PIRP)w->request);
}

/*
 * An event reaches user mode as THREE LITTLE-ENDIAN DWORDS - type, then the
 * two arguments - which is the whole of the twelve bytes function 0x818
 * hands back. Written a byte at a time rather than as three u32 stores so
 * that the layout does not depend on the host's alignment rules.
 */
static void NotifyDeliver(void *ctx, core_notify_waiter *w, u32 type,
                          u32 arg1, u32 arg2)
{
	PIRP   irp = (PIRP)w->request;
	UCHAR *out;

	UNREFERENCED_PARAMETER(ctx);
	out = (UCHAR *)ADAPTOID_IRP_BUFFER(irp);
	if (out != NULL) {
		u32 i;
		u32 word[3];

		word[0] = type;
		word[1] = arg1;
		word[2] = arg2;
		for (i = 0; i < 3; i++) {
			out[i * 4 + 0] = (UCHAR)(word[i]);
			out[i * 4 + 1] = (UCHAR)(word[i] >> 8);
			out[i * 4 + 2] = (UCHAR)(word[i] >> 16);
			out[i * 4 + 3] = (UCHAR)(word[i] >> 24);
		}
	}
	AdaptoidCompleteIrp(irp, STATUS_SUCCESS, CORE_NOTIFY_BYTES);
}

static void NotifyAbort(void *ctx, core_notify_waiter *w)
{
	UNREFERENCED_PARAMETER(ctx);
	AdaptoidCompleteIrp((PIRP)w->request, STATUS_CANCELLED, 0);
}

void AdaptoidNotifyInit(PADAPTOID_CDO_EXT CdoExt)
{
	core_notify_init(&CdoExt->Registry.notify, NotifyClaim, NotifyDeliver,
	                 NotifyAbort, CdoExt);
}

static void NTAPI NotifyCancelRoutine(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PADAPTOID_CDO_EXT cx =
	        (PADAPTOID_CDO_EXT)DeviceObject->DeviceExtension;

	IoReleaseCancelSpinLock(Irp->CancelIrql);
	AdaptoidCancelNotifications(cx, Irp);
}

NTSTATUS AdaptoidWaitNotification(PADAPTOID_CDO_EXT CdoExt, PIRP Irp)
{
	core_notify_waiter *w = ADAPTOID_IRP_WAITER(Irp);
	KIRQL               irql;

	w->request = Irp;

	KeAcquireSpinLock(&CdoExt->Lock, &irql);
	/*
	 * MARKED PENDING BEFORE THE CANCEL ROUTINE GOES ON, because from the
	 * moment it does another thread may complete this IRP.
	 */
	IoMarkIrpPending(Irp);
	IoSetCancelRoutine(Irp, (PVOID)NotifyCancelRoutine);
	core_notify_wait(&CdoExt->Registry.notify, w);
	KeReleaseSpinLock(&CdoExt->Lock, irql);

	/*
	 * PENDING EITHER WAY. core_notify_wait completes the IRP itself
	 * through NotifyDeliver when an event was already queued, but
	 * IoMarkIrpPending has been called by then and returning anything
	 * else after that is a protocol violation.
	 *
	 * THE ORIGINAL VIOLATES IT. drv_IoctlWaitNotification marks the IRP
	 * pending and then, if it finds the request already cancelled,
	 * returns STATUS_CANCELLED. See known-defects.txt.
	 */
	return STATUS_PENDING;
}

void AdaptoidCancelNotifications(PADAPTOID_CDO_EXT CdoExt, PIRP Irp)
{
	PFILE_OBJECT owner = NULL;
	KIRQL        irql;

	if (CdoExt == NULL) {
		return;
	}
	/*
	 * Irp SELECTS WHICH ONES. NULL cancels every waiter; otherwise only
	 * the waiters belonging to the same FILE OBJECT - that is, the same
	 * user-mode handle - are cancelled, which is what makes a close
	 * affect one client and not all of them.
	 */
	if (Irp != NULL) {
		owner = IoGetCurrentIrpStackLocation(Irp)->FileObject;
	}

	for (;;) {
		core_notify_waiter *w;
		core_notify_waiter *victim = NULL;

		KeAcquireSpinLock(&CdoExt->Lock, &irql);
		for (w = CdoExt->Registry.notify.waiters.flink;
		     w != &CdoExt->Registry.notify.waiters; w = w->flink) {
			PIRP parked = (PIRP)w->request;

			if (owner != NULL &&
			    IoGetCurrentIrpStackLocation(parked)->FileObject !=
			    owner) {
				continue;
			}
			/* Unlinked under the lock, one per pass, so the
			 * loop terminates however the claims go. */
			core_notify_cancel(&CdoExt->Registry.notify, w);
			victim = w;
			break;
		}
		KeReleaseSpinLock(&CdoExt->Lock, irql);

		if (victim == NULL) {
			break;
		}
		/*
		 * CLAIMED AND COMPLETED OUTSIDE THE LOCK. A waiter that cannot
		 * be claimed is already being completed by whoever did claim
		 * it, so it is dropped here rather than completed twice - and
		 * because it is dropped after core_notify_cancel unlinked it,
		 * no event is consumed on its behalf.
		 */
		if (AdaptoidClaimIrp((PIRP)victim->request)) {
			AdaptoidCompleteIrp((PIRP)victim->request,
			                    STATUS_CANCELLED, 0);
		}
	}
}

/* ------------------------------------------------------------------ */
/* the control device's four dispatch entry points                     */
/* ------------------------------------------------------------------ */

/*
 * IRP_MJ_CREATE. A handle is being opened on \\.\Wish_NA1.
 *
 * REFUSED WITH NO ADAPTER PRESENT. The control device exists as soon as one
 * adapter has ever arrived, but opening it while none is live gets
 * STATUS_DELETE_PENDING - so a client cannot hold a handle across the last
 * unplug and expect it to keep working.
 *
 * TWO RESETS, ON DIFFERENT SCHEDULES. The FIRST open puts every adapter's
 * stick tuning back to its defaults; EVERY open clears the keep-alive and
 * emulated-Pak state. So opening the configurator silently discards stick
 * tuning a previous client left behind, which a replacement that preserved
 * it across sessions would get visibly wrong.
 */
NTSTATUS NTAPI AdaptoidControlCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PADAPTOID_CDO_EXT cx = (PADAPTOID_CDO_EXT)DeviceObject->DeviceExtension;
	core_device_entry *d;
	LONG              was;

	if (cx == NULL) {
		AdaptoidCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
		return STATUS_DELETE_PENDING;
	}
	if (cx->Registry.live_count == 0) {
		AdaptoidCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
		return STATUS_DELETE_PENDING;
	}

	ExAcquireFastMutex(&g_ControlMutex);
	was = cx->OpenCount++;
	ExReleaseFastMutex(&g_ControlMutex);

	if (was == 0) {
		for (d = cx->Registry.devices.flink;
		     d != &cx->Registry.devices; d = d->flink) {
			if (d->cs != NULL) {
				d->cs->stick_clip    = CORE_STICK_CLIP_DEFAULT;
				d->cs->stick_stretch = CORE_STICK_STRETCH_DEF;
			}
		}
	}
	for (d = cx->Registry.devices.flink; d != &cx->Registry.devices;
	     d = d->flink) {
		if (d->cs != NULL) {
			d->cs->keepalive_time    = 0;
			d->cs->emu_pak_present   = 0;
		}
	}

	AdaptoidCompleteIrp(Irp, STATUS_SUCCESS, 0);
	return STATUS_SUCCESS;
}

/*
 * IRP_MJ_CLEANUP. The handle is going away but the file object is still
 * alive, which is exactly when parked requests belonging to it must be
 * released - so this cancels only THIS handle's notification waiters.
 */
NTSTATUS NTAPI AdaptoidControlCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	AdaptoidCancelNotifications(
	        (PADAPTOID_CDO_EXT)DeviceObject->DeviceExtension, Irp);
	AdaptoidCompleteIrp(Irp, STATUS_SUCCESS, 0);
	return STATUS_SUCCESS;
}

/*
 * IRP_MJ_CLOSE.
 *
 * THE DEVICE OUTLIVES THE LAST ADAPTER IF A HANDLE IS STILL OPEN. Deletion
 * needs BOTH counts at zero, and either side can be the one that reaches
 * zero last - so the same test lives here and in AdaptoidReleaseControlDevice
 * rather than only in the adapter path.
 */
NTSTATUS NTAPI AdaptoidControlClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PADAPTOID_CDO_EXT cx = (PADAPTOID_CDO_EXT)DeviceObject->DeviceExtension;

	AdaptoidCancelNotifications(cx, Irp);

	ExAcquireFastMutex(&g_ControlMutex);
	if (cx->OpenCount > 0) {
		cx->OpenCount--;
	}
	ExReleaseFastMutex(&g_ControlMutex);

	AdaptoidControlMaybeDelete();
	AdaptoidCompleteIrp(Irp, STATUS_SUCCESS, 0);
	return STATUS_SUCCESS;
}

/*
 * IRP_MJ_DEVICE_CONTROL. Eleven driver-wide codes and a forward for the
 * per-device ones, all of which is core_ctl_dispatch's; what is here is the
 * IRP, and the one code that cannot be answered synchronously.
 */
NTSTATUS NTAPI AdaptoidControlIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PADAPTOID_CDO_EXT  cx = (PADAPTOID_CDO_EXT)DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
	core_ioctl         req;
	u32                info = 0;
	u32                st;

	req.code    = sl->Parameters.DeviceIoControl.IoControlCode;
	req.in      = (const u8 *)ADAPTOID_IRP_BUFFER(Irp);
	req.in_len  = sl->Parameters.DeviceIoControl.InputBufferLength;
	req.out     = (u8 *)ADAPTOID_IRP_BUFFER(Irp);
	req.out_len = sl->Parameters.DeviceIoControl.OutputBufferLength;

	/*
	 * THE WAITER IS PARKED BEFORE THE DISPATCH, not after, because
	 * core_notify_wait may deliver an event and complete the IRP inside
	 * the call. Nothing may touch the IRP after that, which is why the
	 * pending case returns immediately.
	 */
	if (CORE_IOCTL_FN(req.code) == CORE_CTL_WAIT_NOTIFY &&
	    req.in_len == 0 && req.out_len == CORE_NOTIFY_BYTES) {
		return AdaptoidWaitNotification(cx, Irp);
	}

	st = core_ctl_dispatch(&cx->Registry, &req, NULL,
	                       KeQueryInterruptTime(), &info);
	AdaptoidCompleteIrp(Irp, (NTSTATUS)st, info);
	return (NTSTATUS)st;
}

/*
 * IRP_MJ_READ and IRP_MJ_WRITE - the SDK command-block channel.
 *
 * ONE HANDLER FOR BOTH, as the original has, because the two directions
 * share the block and differ only in which pass they run.
 */
NTSTATUS NTAPI AdaptoidControlReadWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PADAPTOID_CDO_EXT  cx = (PADAPTOID_CDO_EXT)DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
	u8                *buf = (u8 *)ADAPTOID_IRP_BUFFER(Irp);
	u32                info = 0;
	u32                st;
	ULONG              len;

	if (cx == NULL) {
		AdaptoidCompleteIrp(Irp, STATUS_DELETE_PENDING, 0);
		return STATUS_DELETE_PENDING;
	}

	if (sl->MajorFunction == IRP_MJ_WRITE) {
		len = sl->Parameters.Write.Length;
		st  = core_cmd_write(&cx->Registry, &cx->Channel, buf, len,
		                     KeQueryInterruptTime(), &info);
	} else {
		len = sl->Parameters.Read.Length;
		st  = core_cmd_read(&cx->Registry, &cx->Channel, buf, len,
		                    KeQueryInterruptTime(), &info);
	}
	AdaptoidCompleteIrp(Irp, (NTSTATUS)st, info);
	return (NTSTATUS)st;
}
