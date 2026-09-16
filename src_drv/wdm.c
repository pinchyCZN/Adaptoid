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

void AdaptoidReportSink(void *ctx, u8 report_id, const u8 *data, u32 len)
{
	PADAPTOID_DEVEXT devext = (PADAPTOID_DEVEXT)ctx;

	(void)devext;
	(void)report_id;
	(void)data;
	(void)len;

	/*
	 * TODO: hand the report to hidclass by completing a pending read IRP.
	 * This is the seam the original calls drv_SubmitHidReport, and it is
	 * deliberately the ONLY point at which core.c touches the OS.
	 */
}

/* ------------------------------------------------------------------ */
/* Load and unload                                                     */
/* ------------------------------------------------------------------ */

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject,
                           PUNICODE_STRING RegistryPath)
{
	HID_MINIDRIVER_REGISTRATION reg;
	ULONG                       i;
	PDRIVER_DISPATCH           *mj;

	/*
	 * Register as a HID minidriver so hidclass stacks on top of us. This is
	 * the whole reason the original can present a composite keyboard, mouse
	 * and joystick device; see ../docs/replacement-architecture.txt section 2.
	 */
	mj = DriverObject->MajorFunction;

	for (i = 0; i < IRP_MJ_MAXIMUM_FUNCTION + 1; i++) {
		mj[i] = 0;
	}

	mj[IRP_MJ_CREATE]                  = AdaptoidCreate;
	mj[IRP_MJ_CLOSE]                   = AdaptoidClose;
	mj[IRP_MJ_CLEANUP]                 = AdaptoidCleanup;
	mj[IRP_MJ_READ]                    = AdaptoidRead;
	mj[IRP_MJ_WRITE]                   = AdaptoidWrite;
	mj[IRP_MJ_DEVICE_CONTROL]          = AdaptoidDeviceControl;
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

	return HidRegisterMinidriver(&reg);
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

NTSTATUS NTAPI AdaptoidCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	(void)DeviceObject; (void)Irp;
	return STATUS_SUCCESS;                  /* TODO */
}

NTSTATUS NTAPI AdaptoidClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	(void)DeviceObject; (void)Irp;
	return STATUS_SUCCESS;                  /* TODO */
}

NTSTATUS NTAPI AdaptoidCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	(void)DeviceObject; (void)Irp;
	return STATUS_SUCCESS;                  /* TODO */
}

NTSTATUS NTAPI AdaptoidRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	(void)DeviceObject; (void)Irp;
	return STATUS_NOT_SUPPORTED;            /* TODO */
}

NTSTATUS NTAPI AdaptoidWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	(void)DeviceObject; (void)Irp;
	return STATUS_NOT_SUPPORTED;            /* TODO */
}

NTSTATUS NTAPI AdaptoidDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	(void)DeviceObject; (void)Irp;
	/* TODO: the private configuration surface.
	 * See ../docs/ioctl-surface.txt. */
	return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS NTAPI AdaptoidIntDeviceControl(PDEVICE_OBJECT DeviceObject,
                                        PIRP Irp)
{
	(void)DeviceObject; (void)Irp;
	/* TODO: the hidclass contract, including GET_REPORT_DESCRIPTOR, which must
	 * return the composite descriptor from ../docs/hid-descriptor.txt. */
	return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS NTAPI AdaptoidPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	(void)DeviceObject; (void)Irp;
	/* TODO: START_DEVICE fetches descriptors, selects the configuration and
	 * keeps the interrupt IN pipe; see ../docs/driver-lifecycle.txt 3.1. */
	return STATUS_SUCCESS;
}

NTSTATUS NTAPI AdaptoidPower(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	(void)DeviceObject; (void)Irp;
	return STATUS_SUCCESS;                  /* TODO */
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
#endif /* !ADAPTOID_USERMODE */
