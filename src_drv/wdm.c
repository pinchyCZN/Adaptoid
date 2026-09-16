/*
 * wdm.c - the OS-facing layer of wishk300.
 *
 * SKELETON. Every entry point below has the signature the kernel will call it
 * with and a body that is a stub. The shape is the deliverable of this pass,
 * not the behaviour.
 *
 * Reference for what each of these must eventually do:
 *   ../docs/driver-lifecycle.txt   DriverEntry, AddDevice, PnP, power
 *   ../docs/ioctl-surface.txt      the private IOCTL surface
 *   ../docs/usb-transport.txt      the vendor protocol and the poll engine
 *   ../docs/hid-descriptor.txt     what GET_REPORT_DESCRIPTOR must return
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
	mj[IRP_MJ_PNP]                     = AdaptoidPnp;
	mj[IRP_MJ_POWER]                   = AdaptoidPower;
	DriverObject->DriverUnload         = AdaptoidUnload;

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

void NTAPI AdaptoidUnload(PDRIVER_OBJECT DriverObject)
{
	(void)DriverObject;
	/* TODO: tear down anything DriverEntry created. */
}

NTSTATUS NTAPI AdaptoidAddDevice(PDRIVER_OBJECT DriverObject,
                                 PDEVICE_OBJECT FunctionalDeviceObject)
{
	PHID_DEVICE_EXTENSION hidext;
	PADAPTOID_DEVEXT      devext;

	(void)DriverObject;

	if (FunctionalDeviceObject == 0) {
		return STATUS_INVALID_PARAMETER;
	}

	/* The two-level hop: hidclass owns DeviceExtension, we own
	 * MiniDeviceExtension. Confusing the two makes every offset wrong. */
	hidext = (PHID_DEVICE_EXTENSION)FunctionalDeviceObject->DeviceExtension;
	if (hidext == 0) {
		return STATUS_UNSUCCESSFUL;
	}

	devext = (PADAPTOID_DEVEXT)hidext->MiniDeviceExtension;
	if (devext == 0) {
		return STATUS_UNSUCCESSFUL;
	}

	devext->Self                 = FunctionalDeviceObject;
	devext->NextDeviceObject     = hidext->NextDeviceObject;
	devext->PhysicalDeviceObject = hidext->PhysicalDeviceObject;
	devext->Started              = 0;

	core_init(&devext->Core, AdaptoidReportSink, devext);

	return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */







NTSTATUS NTAPI AdaptoidIntDeviceControl(PDEVICE_OBJECT DeviceObject,
                                        PIRP Irp)
{
	(void)DeviceObject; (void)Irp;
	/* TODO: the hidclass contract, including GET_REPORT_DESCRIPTOR, which must
	 * return the composite descriptor from ../docs/hid-descriptor.txt. */
	return STATUS_INVALID_DEVICE_REQUEST;
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
NTSTATUS AdaptoidVendorSubmitUrb(PADAPTOID_DEVEXT DevExt,
                                 const ADAPTOID_SETUP *Setup,
                                 ULONG TransferLength, PVOID TransferBuffer)
{
	UNREFERENCED_PARAMETER(DevExt);
	UNREFERENCED_PARAMETER(Setup);
	UNREFERENCED_PARAMETER(TransferLength);
	UNREFERENCED_PARAMETER(TransferBuffer);
	return STATUS_NOT_IMPLEMENTED;
}
/*
 * The OS edge of the poll loop and the read queue. Like
 * AdaptoidVendorSubmitUrb these have two definitions selected by
 * ADAPTOID_USERMODE - harness.c supplies observable ones - and one origin
 * row each; see origin.txt.
 *
 * STAGE FOUR: the URB build needs usbdi.h and there is no device to send it
 * to yet.
 */
NTSTATUS AdaptoidPollSubmit(PADAPTOID_DEVEXT DevExt, ULONG Slot)
{
	UNREFERENCED_PARAMETER(DevExt);
	UNREFERENCED_PARAMETER(Slot);
	return STATUS_NOT_IMPLEMENTED;
}

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

void AdaptoidQueuePollRestart(PADAPTOID_DEVEXT DevExt)
{
	UNREFERENCED_PARAMETER(DevExt);
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
		AdaptoidStartNextPowerIrp(Irp);
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
	AdaptoidSetDeviceName(DevExt);
	AdaptoidPollStart(DevExt, ADAPTOID_STOP_REASON_PNP);
	AdaptoidEnableInterface(DevExt);
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

NTSTATUS AdaptoidFetchDeviceDescriptor(PADAPTOID_DEVEXT DevExt)
{
	UNREFERENCED_PARAMETER(DevExt);
	return STATUS_SUCCESS;
}

NTSTATUS AdaptoidSelectConfiguration(PADAPTOID_DEVEXT DevExt)
{
	UNREFERENCED_PARAMETER(DevExt);
	return STATUS_SUCCESS;
}

void AdaptoidSetDeviceName(PADAPTOID_DEVEXT DevExt)
{ UNREFERENCED_PARAMETER(DevExt); }



void AdaptoidQuiesceIo(PADAPTOID_DEVEXT DevExt)
{ UNREFERENCED_PARAMETER(DevExt); }

void AdaptoidUnconfigureDevice(PADAPTOID_DEVEXT DevExt)
{ UNREFERENCED_PARAMETER(DevExt); }

void AdaptoidAbortPipes(PADAPTOID_DEVEXT DevExt)
{ UNREFERENCED_PARAMETER(DevExt); }

void AdaptoidFreeDeviceResources(PADAPTOID_DEVEXT DevExt)
{ UNREFERENCED_PARAMETER(DevExt); }

void AdaptoidEnableInterface(PADAPTOID_DEVEXT DevExt)
{ UNREFERENCED_PARAMETER(DevExt); }

void AdaptoidRegistryRemove(PADAPTOID_DEVEXT DevExt)
{ UNREFERENCED_PARAMETER(DevExt); }

void AdaptoidSetCompletionRoutine(PIRP Irp, PVOID Event)
{ UNREFERENCED_PARAMETER(Irp); UNREFERENCED_PARAMETER(Event); }

void AdaptoidStartNextPowerIrp(PIRP Irp)
{ UNREFERENCED_PARAMETER(Irp); }

NTSTATUS NTAPI AdaptoidControlCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{ UNREFERENCED_PARAMETER(DeviceObject);
  return AdaptoidCompleteIrp(Irp, STATUS_SUCCESS, 0), STATUS_SUCCESS; }

NTSTATUS NTAPI AdaptoidControlCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{ UNREFERENCED_PARAMETER(DeviceObject);
  return AdaptoidCompleteIrp(Irp, STATUS_SUCCESS, 0), STATUS_SUCCESS; }

NTSTATUS NTAPI AdaptoidControlClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{ UNREFERENCED_PARAMETER(DeviceObject);
  return AdaptoidCompleteIrp(Irp, STATUS_SUCCESS, 0), STATUS_SUCCESS; }

NTSTATUS NTAPI AdaptoidControlIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{ UNREFERENCED_PARAMETER(DeviceObject);
  return AdaptoidCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0),
	     STATUS_NOT_SUPPORTED; }

NTSTATUS NTAPI AdaptoidControlReadWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{ UNREFERENCED_PARAMETER(DeviceObject);
  return AdaptoidCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0),
	     STATUS_NOT_SUPPORTED; }

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

NTSTATUS NTAPI AdaptoidPower(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PADAPTOID_DEVEXT dx = AdaptoidDevExtOf(DeviceObject);

	AdaptoidStartNextPowerIrp(Irp);
	IoSkipCurrentIrpStackLocation(Irp);
	return IofCallDriver(dx->NextDeviceObject, Irp);
}

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
		/* The whole point: decode, then immediately put the slot back in
		 * flight so the other one is never alone. */
		core_on_raw_packet(&DevExt->Core, DevExt->PollSlot[Slot].Buffer);
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
