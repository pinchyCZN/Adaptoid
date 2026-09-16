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
