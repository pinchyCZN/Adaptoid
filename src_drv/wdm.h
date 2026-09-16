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
typedef struct _ADAPTOID_DEVEXT {
	PDEVICE_OBJECT  Self;
	PDEVICE_OBJECT  NextDeviceObject;
	PDEVICE_OBJECT  PhysicalDeviceObject;

	/* All the OS-free state lives here. */
	core_state      Core;

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

#endif /* ADAPTOID_WDM_H */
