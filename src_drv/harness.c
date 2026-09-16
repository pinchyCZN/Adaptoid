/*
 * harness.c - the user-mode test harness for wishk300.
 *
 * Builds core.c and wdm.c as an ordinary console program so the driver logic
 * can be exercised without a VM, a driver install, or a bugcheck. Supplies the
 * kernel routines wdm.c calls, and a report sink that prints.
 *
 * MUST NOT include <windows.h>. kstub.h defines NTSTATUS, ULONG, DEVICE_OBJECT
 * and friends itself; pulling in the real headers collides with all of them.
 * Keep this file to the C runtime.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wdm.h"

/* ------------------------------------------------------------------ */
/* The fake clock                                                      */
/* ------------------------------------------------------------------ */

/*
 * The highest-value stub in the file. In the kernel this is whatever the HAL
 * says; here it is a variable the test advances by hand, which makes the
 * script scheduler, the effect ring and the keep-alive fully deterministic and
 * steppable. Nothing else gives that.
 */
static ULONGLONG g_interrupt_time_100ns = 0;

ULONGLONG KeQueryInterruptTime(void)
{
	return g_interrupt_time_100ns;
}

static void clock_advance_ms(unsigned ms)
{
	g_interrupt_time_100ns += (ULONGLONG)ms * 10000ULL;
}

/* ------------------------------------------------------------------ */
/* Kernel stubs                                                        */
/* ------------------------------------------------------------------ */

static unsigned long g_pool_allocs = 0;
static unsigned long g_pool_frees  = 0;

PVOID ExAllocatePoolWithTag(ULONG PoolType, ULONG NumberOfBytes, ULONG Tag)
{
	(void)PoolType;
	(void)Tag;
	g_pool_allocs++;
	return malloc((size_t)NumberOfBytes);
}

void ExFreePool(PVOID P)
{
	if (P != NULL) {
		g_pool_frees++;
		free(P);
	}
}

/* Single-threaded harness: plain operations are sufficient and keep the
 * behaviour observable. */
LONG InterlockedIncrement(LONG volatile *Addend) { return ++(*Addend); }
LONG InterlockedDecrement(LONG volatile *Addend) { return --(*Addend); }

LONG InterlockedExchange(LONG volatile *Target, LONG Value)
{
	LONG old = *Target;
	*Target = Value;
	return old;
}

void KeInitializeSpinLock(PKSPIN_LOCK SpinLock)
{
	if (SpinLock) {
		*SpinLock = 0;
	}
}
KIRQL KfAcquireSpinLock(PKSPIN_LOCK SpinLock)    { (void)SpinLock; return 0; }

void KfReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL NewIrql)
{
	(void)SpinLock;
	(void)NewIrql;
}

void KeInitializeEvent(PKEVENT Event, ULONG Type, BOOLEAN State)
{
	(void)Type;
	if (Event) {
		Event->Signalled = State ? 1 : 0;
	}
}

LONG KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait)
{
	LONG old = 0;
	(void)Increment;
	(void)Wait;
	if (Event) {
		old = Event->Signalled;
		Event->Signalled = 1;
	}
	return old;
}

/*
 * Stands in for hidclass. Records the registration so the harness can size the
 * minidriver extension exactly as hidclass would.
 */
static HID_MINIDRIVER_REGISTRATION g_registration;
static int                         g_registered = 0;

NTSTATUS HidRegisterMinidriver(PHID_MINIDRIVER_REGISTRATION Registration)
{
	if (Registration == NULL) {
		return STATUS_INVALID_PARAMETER;
	}
	if (Registration->Revision != HID_REVISION) {
		return STATUS_INVALID_PARAMETER;
	}
	g_registration = *Registration;
	g_registered   = 1;
	return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* The harness report sink                                             */
/* ------------------------------------------------------------------ */

static unsigned long g_reports_seen = 0;

static const char *report_name(u8 id)
{
	switch (id) {
	case CORE_REPORT_JOYSTICK: return "joystick";
	case CORE_REPORT_KEYBOARD: return "keyboard";
	case CORE_REPORT_MOUSE:    return "mouse";
	default:                   return "unknown";
	}
}

static void harness_sink(void *ctx, u8 report_id, const u8 *data, u32 len)
{
	u32 i;

	(void)ctx;
	g_reports_seen++;

	printf("  [t=%6llu ms] report %u (%-8s) len %2u :",
	       (unsigned long long)(g_interrupt_time_100ns / 10000ULL),
	       (unsigned)report_id, report_name(report_id), (unsigned)len);
	for (i = 0; i < len; i++) {
		printf(" %02X", data[i]);
	}
	printf("\n");
}

/* ------------------------------------------------------------------ */
/* Simulated device bring-up                                           */
/* ------------------------------------------------------------------ */

/*
 * Stands in for what hidclass does between DriverEntry and AddDevice: create
 * the FDO, allocate HID_DEVICE_EXTENSION, and allocate the minidriver
 * extension at the size the registration asked for.
 */
static PADAPTOID_DEVEXT simulate_add_device(DEVICE_OBJECT *fdo,
                                            HID_DEVICE_EXTENSION *hidext,
                                            DEVICE_OBJECT *pdo,
                                            DEVICE_OBJECT *lower)
{
	NTSTATUS status;
	void    *mini;

	mini = calloc(1, (size_t)g_registration.DeviceExtensionSize);
	if (mini == NULL) {
		return NULL;
	}

	hidext->PhysicalDeviceObject = pdo;
	hidext->NextDeviceObject     = lower;
	hidext->MiniDeviceExtension  = mini;

	fdo->DeviceExtension = hidext;
	fdo->NextDevice      = NULL;
	fdo->Flags           = 0;

	status = AdaptoidAddDevice(g_registration.DriverObject, fdo);
	if (!NT_SUCCESS(status)) {
		printf("AddDevice failed: 0x%08lX\n", (unsigned long)status);
		free(mini);
		return NULL;
	}
	return (PADAPTOID_DEVEXT)mini;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
	DRIVER_OBJECT        driver;
	UNICODE_STRING       regpath;
	DEVICE_OBJECT        fdo, pdo, lower;
	HID_DEVICE_EXTENSION hidext;
	PADAPTOID_DEVEXT     devext;
	NTSTATUS             status;
	int                  i;
	int                  count;

	static const u8 packets[][CORE_RAW_PACKET_BYTES] = {
		{ 0x00, 0x00, 0x00, 0x00, 0x00 },   /* neutral         */
		{ 0x80, 0x00, 0x00, 0x00, 0x00 },   /* one button      */
		{ 0x00, 0x00, 0x40, 0xC0, 0x00 },   /* stick deflected */
		{ 0x00, 0x20, 0x00, 0x00, 0x01 }    /* another button  */
	};

	count = (int)(sizeof(packets) / sizeof(packets[0]));

	printf("wishk300 test harness\n");
	printf("=====================\n\n");

	memset(&driver,  0, sizeof(driver));
	memset(&regpath, 0, sizeof(regpath));
	memset(&fdo,     0, sizeof(fdo));
	memset(&pdo,     0, sizeof(pdo));
	memset(&lower,   0, sizeof(lower));
	memset(&hidext,  0, sizeof(hidext));

	/* 1. Load. */
	status = DriverEntry(&driver, &regpath);
	printf("DriverEntry            : 0x%08lX  (registered=%d, "
	       "devext=%lu bytes)\n",
	       (unsigned long)status, g_registered,
	       (unsigned long)g_registration.DeviceExtensionSize);
	if (!NT_SUCCESS(status) || !g_registered) {
		return 1;
	}

	/* 2. Enumerate one device. */
	devext = simulate_add_device(&fdo, &hidext, &pdo, &lower);
	if (devext == NULL) {
		return 1;
	}
	printf("AddDevice              : ok\n");

	/*
	 * 3. Redirect the core output at the harness. In the driver this stays
	 *    AdaptoidReportSink, which completes a pending HID read IRP. Swapping
	 *    it here is the whole point of the seam.
	 */
	core_init(&devext->Core, harness_sink, devext);
	printf("sink                   : harness (devices_mask=0x%X)\n\n",
	       (unsigned)devext->Core.devices_mask);

	/* 4. Drive it. */
	printf("feeding %d raw packets:\n", count);
	for (i = 0; i < count; i++) {
		clock_advance_ms(16);              /* ~1/64 s, the script time base */
		core_tick(&devext->Core, KeQueryInterruptTime());
		core_on_raw_packet(&devext->Core, packets[i]);
	}

	/* 5. Report. */
	printf("\nsummary\n");
	printf("  reports emitted      : %lu\n",
	       (unsigned long)devext->Core.reports_emitted);
	printf("  reports seen by sink : %lu\n", g_reports_seen);
	printf("  pool allocs / frees  : %lu / %lu\n", g_pool_allocs, g_pool_frees);
	printf("  final clock          : %llu ms\n",
	       (unsigned long long)(g_interrupt_time_100ns / 10000ULL));

	if (g_reports_seen != devext->Core.reports_emitted) {
		printf("\nFAIL: sink count does not match emitted count\n");
		free(hidext.MiniDeviceExtension);
		return 1;
	}

	AdaptoidUnload(&driver);
	free(hidext.MiniDeviceExtension);

	printf("\nok\n");
	return 0;
}
