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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wdm.h"
#include "script.h"
#include "sched.h"
#include "ioctl.h"

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

/*
 * Everything prints through hlog so a growing suite stays readable and so a
 * run can be captured for comparison.
 *
 *     -v            print everything, not just failures and summaries
 *     --log FILE    tee all output to FILE
 *     --trace FILE  write the numeric trace to FILE, nothing else
 *
 * THE TRACE IS THE POINT. The effect engine produces a sequence of numbers
 * over time, and the only convincing way to check it is to dump the whole
 * sequence and diff it against the same sequence emulated out of
 * wishk201.sys. Spot-checking a few values would not catch a phase error or
 * an off-by-one in the ring.
 */
static FILE *g_log;
static FILE *g_trace;
static int   g_verbose;

/* Normal output: suppressed by default unless it is a result line. */
static void hlog(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);

	if (g_log) {
		va_start(ap, fmt);
		vfprintf(g_log, fmt, ap);
		va_end(ap);
	}
}

/* Chatter: only when -v is given. Keeps a long run legible. */
static void hverbose(const char *fmt, ...)
{
	va_list ap;

	if (!g_verbose) {
		return;
	}
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);

	if (g_log) {
		va_start(ap, fmt);
		vfprintf(g_log, fmt, ap);
		va_end(ap);
	}
}

/*
 * One trace record. Written only to the trace file, never to the console,
 * so the file is a clean column of numbers that diff can work on.
 */
static void htrace(const char *fmt, ...)
{
	va_list ap;

	if (!g_trace) {
		return;
	}
	va_start(ap, fmt);
	vfprintf(g_trace, fmt, ap);
	va_end(ap);
}

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

	hverbose("  [t=%6llu ms] report %u (%-8s) len %2u :",
	       (unsigned long long)(g_interrupt_time_100ns / 10000ULL),
	       (unsigned)report_id, report_name(report_id), (unsigned)len);
	for (i = 0; i < len; i++) {
		hverbose(" %02X", data[i]);
	}
	hverbose("\n");
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
		hlog("AddDevice failed: 0x%08lX\n", (unsigned long)status);
		free(mini);
		return NULL;
	}
	return (PADAPTOID_DEVEXT)mini;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/*
 * THE EXPECTED VALUES WERE READ OUT OF wishk201.sys ITSELF, by emulating
 * drv_N64PakAddrCrc5 at 000136f0 and drv_N64PakDataCrc8 at 00013730 in
 * Ghidra and capturing what they returned. They are not copied from the
 * decompiler output and not taken on trust from a reference elsewhere,
 * so they are an independent check on the reimplementation rather than a
 * restatement of it.
 *
 * The address CRC was additionally verified EXHAUSTIVELY over all 2048
 * block addresses against the same emulation; the eight kept here are a
 * spread worth having in the file. The data CRC was checked over 64
 * pseudo-random blocks as well as these five.
 */

static const struct {
	u16 address;
	u16 expect;
} PAK_ADDR_VECTORS[] = {
	{ 0x0000, 0x0000 },
	{ 0x0020, 0x0035 },
	{ 0x0040, 0x005F },
	{ 0x0060, 0x006A },
	{ 0x0100, 0x0116 },
	{ 0x1234, 0x1230 },   /* low 5 bits of the input are discarded */
	{ 0x7FE0, 0x7FEC },
	{ 0xFFE0, 0xFFED }
};

static int test_pak_crcs(void)
{
	u8  block[CORE_PAK_BLOCK_BYTES];
	int bad = 0;
	int i;
	u16 got16;
	u8  got8;

	for (i = 0; i < (int)(sizeof(PAK_ADDR_VECTORS)
		                  / sizeof(PAK_ADDR_VECTORS[0])); i++) {
		got16 = core_pak_addr_encode(PAK_ADDR_VECTORS[i].address);
		if (got16 != PAK_ADDR_VECTORS[i].expect) {
			hlog("  FAIL addr_encode(0x%04X) = 0x%04X, want 0x%04X\n",
			       (unsigned)PAK_ADDR_VECTORS[i].address,
			       (unsigned)got16,
			       (unsigned)PAK_ADDR_VECTORS[i].expect);
			bad++;
		}
	}

	/* all zero */
	memset(block, 0x00, sizeof(block));
	got8 = core_pak_data_crc8(block, CORE_PAK_BLOCK_BYTES);
	if (got8 != 0x00) {
		hlog("  FAIL data_crc8(zeros) = 0x%02X, want 0x00\n", got8);
		bad++;
	}

	/* all ones */
	memset(block, 0xFF, sizeof(block));
	got8 = core_pak_data_crc8(block, CORE_PAK_BLOCK_BYTES);
	if (got8 != 0x0A) {
		hlog("  FAIL data_crc8(ones) = 0x%02X, want 0x0A\n", got8);
		bad++;
	}

	/* 00..1F ascending */
	for (i = 0; i < CORE_PAK_BLOCK_BYTES; i++) {
		block[i] = (u8)i;
	}
	got8 = core_pak_data_crc8(block, CORE_PAK_BLOCK_BYTES);
	if (got8 != 0x33) {
		hlog("  FAIL data_crc8(ascending) = 0x%02X, want 0x33\n", got8);
		bad++;
	}

	/* descending from 0x80 */
	for (i = 0; i < CORE_PAK_BLOCK_BYTES; i++) {
		block[i] = (u8)(0x80 - i);
	}
	got8 = core_pak_data_crc8(block, CORE_PAK_BLOCK_BYTES);
	if (got8 != 0x91) {
		hlog("  FAIL data_crc8(descending) = 0x%02X, want 0x91\n", got8);
		bad++;
	}

	/* a single set bit in the first byte - catches a wrong bit order */
	memset(block, 0x00, sizeof(block));
	block[0] = 0x01;
	got8 = core_pak_data_crc8(block, CORE_PAK_BLOCK_BYTES);
	if (got8 != 0x04) {
		hlog("  FAIL data_crc8(one bit) = 0x%02X, want 0x04\n", got8);
		bad++;
	}

	hlog("Controller Pak CRCs    : %s (%d vectors)\n",
	       bad ? "FAIL" : "ok",
	       (int)(sizeof(PAK_ADDR_VECTORS) / sizeof(PAK_ADDR_VECTORS[0])) + 5);
	return bad;
}

/*
 * Joystick decode and packing.
 *
 * Every expected value below was produced by EMULATING drv_BuildJoystickReport
 * at 00015150 in Ghidra against a fabricated device extension - probe state 2,
 * script state -1, stretch 10, clip 75 - and reading the packed bytes back out
 * of the report buffer at the point it hands them to drv_SubmitJoystickReport.
 * So these are the original behaviour, not a restatement of this code.
 */

static const struct {
	s8  x;
	s8  y;
	u8  b0, b1, b2;      /* expected packed bytes 0..2 */
	const char *what;
} STICK_VECTORS[] = {
	{    0,    0, 0x00, 0x00, 0x00, "centre"            },
	{   65,   65, 0xB0, 0x04, 0xB5, "diagonal corner"   },
	{  -65,  -65, 0x50, 0x0B, 0x4B, "opposite corner"   },
	{   80,    0, 0xB0, 0x04, 0x00, "full right"        },
	{    0,   80, 0x00, 0x00, 0xB5, "full up"           },
	{   40,  -40, 0xBE, 0xE2, 0x2B, "half deflection"   },
	{  127, -128, 0xB0, 0x04, 0xB5, "8-bit negate wrap" }
};

/*
 * One raw button bit at a time. Index 0 is bit 7 of the low byte at +4 and
 * index 15 is bit 0 of the high byte at +3.
 *
 * INDICES 8 AND 9 ARE THE DELIBERATE DIVERGENCE. The original maps both to
 * HID button 1, because its table holds the filler value 0 there and its loop
 * does not test for it - Defect 1 in ../docs/known-defects.txt. This
 * implementation uses the sentinel that note recommends, so it expects no
 * button at all. Everything else matches the original exactly.
 */
static const struct {
	int index;
	u16 expect;
	const char *note;
} BUTTON_VECTORS[] = {
	{  0, 0x0001, "A"        }, {  1, 0x0008, "B"       },
	{  2, 0x0200, "Z"        }, {  3, 0x0100, "Start"   },
	{  4, 0x0400, "D-up"     }, {  5, 0x0800, "D-down"  },
	{  6, 0x1000, "D-left"   }, {  7, 0x2000, "D-right" },
	{  8, 0x0000, "Reset - original gives 0x0001"  },
	{  9, 0x0000, "unused - original gives 0x0001" },
	{ 10, 0x0040, "L"        }, { 11, 0x0080, "R"       },
	{ 12, 0x0020, "C-up"     }, { 13, 0x0002, "C-down"  },
	{ 14, 0x0010, "C-left"   }, { 15, 0x0004, "C-right" }
};

static int test_joystick_report(void)
{
	core_state cs;
	u8  raw[CORE_RAW_PACKET_BYTES];
	u8  out[CORE_REPORT_MAX_BYTES];
	int bad = 0;
	int i, n;

	n = (int)(sizeof(STICK_VECTORS) / sizeof(STICK_VECTORS[0]));
	for (i = 0; i < n; i++) {
		core_init(&cs, 0, 0);
		raw[CORE_RAW_X]          = (u8)STICK_VECTORS[i].x;
		raw[CORE_RAW_Y]          = (u8)STICK_VECTORS[i].y;
		raw[CORE_RAW_STATUS]     = CORE_STATUS_VALID;
		raw[CORE_RAW_BUTTONS_HI] = 0;
		raw[CORE_RAW_BUTTONS_LO] = 0;

		if (!core_decode(&cs, raw)) {
			hlog("  FAIL stick %s: rejected a valid packet\n",
			       STICK_VECTORS[i].what);
			bad++;
			continue;
		}
		core_pack_joystick(&cs, out);
		if (out[0] != STICK_VECTORS[i].b0 ||
		    out[1] != STICK_VECTORS[i].b1 ||
		    out[2] != STICK_VECTORS[i].b2) {
			hlog("  FAIL stick %-18s got %02X %02X %02X, "
			       "want %02X %02X %02X\n",
			       STICK_VECTORS[i].what, out[0], out[1], out[2],
			       STICK_VECTORS[i].b0, STICK_VECTORS[i].b1,
			       STICK_VECTORS[i].b2);
			bad++;
		}
	}

	n = (int)(sizeof(BUTTON_VECTORS) / sizeof(BUTTON_VECTORS[0]));
	for (i = 0; i < n; i++) {
		int idx = BUTTON_VECTORS[i].index;

		core_init(&cs, 0, 0);
		raw[CORE_RAW_X]          = 0;
		raw[CORE_RAW_Y]          = 0;
		raw[CORE_RAW_STATUS]     = CORE_STATUS_VALID;
		raw[CORE_RAW_BUTTONS_HI] = (u8)((idx >= 8) ? (0x80 >> (idx - 8)) : 0);
		raw[CORE_RAW_BUTTONS_LO] = (u8)((idx <  8) ? (0x80 >> idx) : 0);

		if (!core_decode(&cs, raw)) {
			hlog("  FAIL button %d: rejected a valid packet\n", idx);
			bad++;
			continue;
		}
		if (cs.buttons != BUTTON_VECTORS[i].expect) {
			hlog("  FAIL button %-2d (%s) got 0x%04X, want 0x%04X\n",
			       idx, BUTTON_VECTORS[i].note,
			       (unsigned)cs.buttons,
			       (unsigned)BUTTON_VECTORS[i].expect);
			bad++;
		}
	}

	/* Every bit at once: 14 real buttons, and the two filler bits silent. */
	core_init(&cs, 0, 0);
	raw[CORE_RAW_X]          = 0;
	raw[CORE_RAW_Y]          = 0;
	raw[CORE_RAW_STATUS]     = CORE_STATUS_VALID;
	raw[CORE_RAW_BUTTONS_HI] = 0xFF;
	raw[CORE_RAW_BUTTONS_LO] = 0xFF;
	if (!core_decode(&cs, raw) || cs.buttons != 0x3FFF) {
		hlog("  FAIL all buttons: got 0x%04X, want 0x3FFF\n",
		       (unsigned)cs.buttons);
		bad++;
	}

	/* A status byte that fails the validity test must be rejected. */
	core_init(&cs, 0, 0);
	raw[CORE_RAW_STATUS] = 0x00;
	if (core_decode(&cs, raw)) {
		hlog("  FAIL status: accepted an invalid status byte\n");
		bad++;
	}
	raw[CORE_RAW_STATUS] = 0x84;          /* bit 2 set, must be clear */
	if (core_decode(&cs, raw)) {
		hlog("  FAIL status: accepted 0x84\n");
		bad++;
	}

	hlog("Joystick report        : %s (%d vectors)\n",
	       bad ? "FAIL" : "ok",
	       (int)(sizeof(STICK_VECTORS) / sizeof(STICK_VECTORS[0]))
	       + (int)(sizeof(BUTTON_VECTORS) / sizeof(BUTTON_VECTORS[0])) + 3);
	return bad;
}

/* ------------------------------------------------------------------ */
/* A fake vendor bus                                                   */
/* ------------------------------------------------------------------ */

/*
 * Stands in for the USB control pipe. Transfers are NOT completed inside the
 * callback - they are parked and completed by harness_pump - because the real
 * transport is asynchronous and completing inline would re-enter the core
 * from within its own seam. Modelling that properly is the point: it is the
 * difference between testing the state machine and testing a straight line.
 *
 * It also holds only one transfer at a time, like the single vendor slot the
 * original guards with VendorSlotState.
 */
#define BUS_LOG_MAX 16

static core_vendor_req g_bus_log[BUS_LOG_MAX];
static int             g_bus_count;
static core_vendor_req g_bus_cur;
static int             g_bus_busy;
static u8              g_bus_reply[CORE_PROBE_REPLY_BYTES];
static int             g_bus_fail_next;

static void bus_reset(const u8 *reply)
{
	int i;
	g_bus_count     = 0;
	g_bus_busy      = 0;
	g_bus_fail_next = 0;
	for (i = 0; i < CORE_PROBE_REPLY_BYTES; i++) {
		g_bus_reply[i] = reply ? reply[i] : 0;
	}
}

static int harness_vendor(void *ctx, const core_vendor_req *req)
{
	(void)ctx;
	if (g_bus_busy) {
		return 0;                       /* the slot is occupied */
	}
	if (g_bus_count < BUS_LOG_MAX) {
		g_bus_log[g_bus_count] = *req;
	}
	g_bus_count++;
	g_bus_cur  = *req;
	g_bus_busy = 1;
	return 1;
}

/* Complete whatever is outstanding until the core stops asking. */
static void harness_pump(core_state *cs)
{
	int guard = 0;

	while (g_bus_busy && guard++ < BUS_LOG_MAX * 2) {
		int is_in = (g_bus_cur.bmRequestType & 0x80) != 0;
		g_bus_busy = 0;
		if (g_bus_fail_next) {
			g_bus_fail_next = 0;
			core_probe_complete(cs, 0, 0, 0);
		} else if (is_in) {
			core_probe_complete(cs, 1, g_bus_reply,
			                    CORE_PROBE_REPLY_BYTES);
		} else {
			core_probe_complete(cs, 1, 0, 0);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Accessory probe tests                                               */
/* ------------------------------------------------------------------ */

/*
 * The expected transfer sequence, read out of drv_AccessoryProbeStep1
 * through drv_AccessoryProbeClassify2 in wishk201.sys.
 */
static const core_vendor_req PROBE_EXPECTED[] = {
	{ 0x40, 0x72, 0x0038, 0x0015, 0 },
	{ 0x40, 0x72, 0x0039, 0x0012, 0 },
	{ 0x40, 0x72, 0x003A, 0x0023, 0 },
	{ 0x40, 0x72, 0xFF20, 0x0004, 0 },
	{ 0x40, 0x72, 0xFD23, 0x0000, 0 },
	{ 0xC0, 0x21, 0x0000, 0x0000, 4 }
};

static int probe_check_prefix(int n)
{
	int i, bad = 0;

	for (i = 0; i < n; i++) {
		const core_vendor_req *g = &g_bus_log[i];
		const core_vendor_req *w = &PROBE_EXPECTED[i];
		if (g->bmRequestType != w->bmRequestType ||
		    g->bRequest != w->bRequest ||
		    g->wValue != w->wValue ||
		    g->wIndex != w->wIndex ||
		    g->wLength != w->wLength) {
			hlog("  FAIL probe step %d: got %02X/%02X v=%04X i=%04X l=%u, "
			       "want %02X/%02X v=%04X i=%04X l=%u\n", i + 1,
			       g->bmRequestType, g->bRequest, g->wValue, g->wIndex,
			       (unsigned)g->wLength,
			       w->bmRequestType, w->bRequest, w->wValue, w->wIndex,
			       (unsigned)w->wLength);
			bad++;
		}
	}
	return bad;
}

static int test_accessory_probe(void)
{
	core_state cs;
	int bad = 0;
	/* [0] count, [1] status, [2] and [3] the reversed 05 00 identifier. */
	u8 good[CORE_PROBE_REPLY_BYTES]    = { 0x03, 0x01, 0x00, 0x05 };
	u8 wrongid[CORE_PROBE_REPLY_BYTES] = { 0x03, 0x01, 0x00, 0x99 };
	u8 shortr[CORE_PROBE_REPLY_BYTES]  = { 0x01, 0x00, 0x00, 0x00 };

	/* 1. Identified on the first read: six transfers, state FOUND_1. */
	core_init(&cs, 0, 0);
	core_set_vendor(&cs, harness_vendor, 0);
	bus_reset(good);
	core_probe_start(&cs);
	harness_pump(&cs);
	bad += probe_check_prefix(6);
	if (g_bus_count != 6) {
		hlog("  FAIL probe: %d transfers, want 6\n", g_bus_count);
		bad++;
	}
	if (cs.accessory_state != CORE_ACC_FOUND_1) {
		hlog("  FAIL probe: state %u, want %u\n",
		       cs.accessory_state, CORE_ACC_FOUND_1);
		bad++;
	}
	if (cs.accessory_status != 0x01) {
		hlog("  FAIL probe: status 0x%02X, want 0x01\n",
		       cs.accessory_status);
		bad++;
	}

	/* 2. Wrong identifier: reselect with 0x00C6, read again, then FOUND_2. */
	core_init(&cs, 0, 0);
	core_set_vendor(&cs, harness_vendor, 0);
	bus_reset(wrongid);
	core_probe_start(&cs);
	/* answer the first read badly, the second one well */
	while (g_bus_busy) {
		int is_in = (g_bus_cur.bmRequestType & 0x80) != 0;
		g_bus_busy = 0;
		if (is_in && g_bus_count > 6) {
			core_probe_complete(&cs, 1, good, CORE_PROBE_REPLY_BYTES);
		} else if (is_in) {
			core_probe_complete(&cs, 1, wrongid, CORE_PROBE_REPLY_BYTES);
		} else {
			core_probe_complete(&cs, 1, 0, 0);
		}
	}
	if (g_bus_count != 8) {
		hlog("  FAIL probe reselect: %d transfers, want 8\n", g_bus_count);
		bad++;
	} else if (g_bus_log[6].bRequest != 0x74 || g_bus_log[6].wValue != 0x00C6) {
		hlog("  FAIL probe reselect: transfer 7 is %02X v=%04X, "
		       "want 74 v=00C6\n",
		       g_bus_log[6].bRequest, g_bus_log[6].wValue);
		bad++;
	}
	if (cs.accessory_state != CORE_ACC_FOUND_2) {
		hlog("  FAIL probe reselect: state %u, want %u\n",
		       cs.accessory_state, CORE_ACC_FOUND_2);
		bad++;
	}

	/* 3. Never identified: nine transfers, last one 0x74 0x00A6, UNKNOWN. */
	core_init(&cs, 0, 0);
	core_set_vendor(&cs, harness_vendor, 0);
	bus_reset(wrongid);
	core_probe_start(&cs);
	harness_pump(&cs);
	if (g_bus_count != 9) {
		hlog("  FAIL probe giveup: %d transfers, want 9\n", g_bus_count);
		bad++;
	} else if (g_bus_log[8].bRequest != 0x74 || g_bus_log[8].wValue != 0x00A6) {
		hlog("  FAIL probe giveup: transfer 9 is %02X v=%04X, "
		       "want 74 v=00A6\n",
		       g_bus_log[8].bRequest, g_bus_log[8].wValue);
		bad++;
	}
	if (cs.accessory_state != CORE_ACC_UNKNOWN) {
		hlog("  FAIL probe giveup: state %u, want %u\n",
		       cs.accessory_state, CORE_ACC_UNKNOWN);
		bad++;
	}

	/*
	 * 4. A byte count other than 3 abandons the probe outright rather than
	 *    reselecting - state goes back to NEEDED so a later poll retries.
	 */
	core_init(&cs, 0, 0);
	core_set_vendor(&cs, harness_vendor, 0);
	bus_reset(shortr);
	core_probe_start(&cs);
	harness_pump(&cs);
	if (g_bus_count != 6) {
		hlog("  FAIL probe short: %d transfers, want 6\n", g_bus_count);
		bad++;
	}
	if (cs.accessory_state != CORE_ACC_NEEDED) {
		hlog("  FAIL probe short: state %u, want %u\n",
		       cs.accessory_state, CORE_ACC_NEEDED);
		bad++;
	}

	/* 5. A transport that refuses leaves the probe retryable. */
	core_init(&cs, 0, 0);
	core_set_vendor(&cs, harness_vendor, 0);
	bus_reset(good);
	g_bus_busy = 1;                     /* slot already occupied */
	core_probe_start(&cs);
	if (cs.accessory_state != CORE_ACC_NEEDED) {
		hlog("  FAIL probe busy: state %u, want %u\n",
		       cs.accessory_state, CORE_ACC_NEEDED);
		bad++;
	}

	/* 6. A failed transfer mid-sequence does the same. */
	core_init(&cs, 0, 0);
	core_set_vendor(&cs, harness_vendor, 0);
	bus_reset(good);
	core_probe_start(&cs);
	g_bus_fail_next = 1;
	harness_pump(&cs);
	if (cs.accessory_state != CORE_ACC_NEEDED) {
		hlog("  FAIL probe error: state %u, want %u\n",
		       cs.accessory_state, CORE_ACC_NEEDED);
		bad++;
	}

	hlog("Accessory probe        : %s (6 scenarios)\n", bad ? "FAIL" : "ok");
	return bad;
}

/* ------------------------------------------------------------------ */
/* Effect engine tests                                                 */
/* ------------------------------------------------------------------ */

static int popcount32(const u8 *p)
{
	int i, n = 0;

	for (i = 0; i < CORE_EFFECT_WINDOW; i++) {
		if (p[i >> 3] & (1u << (i & 7))) {
			n++;
		}
	}
	return n;
}

static void effect_arm_constant(core_state *cs, s8 magnitude)
{
	core_init(cs, 0, 0);
	cs->effect[0].type       = CORE_FX_CONSTANT;
	cs->effect[0].running    = 1;
	cs->effect[0].start_tick = 0;
	cs->effect[0].duration   = CORE_FX_INFINITE;
	cs->effect[0].axis[0].periodic.magnitude = magnitude;
	cs->effect[0].axis[1].periodic.magnitude = magnitude;
}

static int test_effect_engine(void)
{
	core_state cs;
	u8  pay[CORE_EFFECT_PAYLOAD];
	u8  pay2[CORE_EFFECT_PAYLOAD];
	int bad = 0;
	int i, n, prev;

	/*
	 * 1. The corrected sine. The ORIGINAL returns 870 at angle 0 because its
	 *    interpolation weights are swapped - verified by emulating
	 *    drv_SineLerp, which gives 870, 435 at 250 and 1 at 499 before
	 *    jumping to 1740. This implementation fixes that, so 0 is the
	 *    expected answer and disagreeing with the original here is correct.
	 */
	if (core_sine_lerp(0) != 0) {
		hlog("  FAIL sine(0) = %d, want 0 (original gives 870, the bug)\n",
		     core_sine_lerp(0));
		bad++;
	}
	if (core_sine_lerp(9000) != 10000) {
		hlog("  FAIL sine(9000) = %d, want 10000\n", core_sine_lerp(9000));
		bad++;
	}
	/* 45 degrees: 10000*sin = 7071, table interpolation gives 7070. */
	if (core_sine_lerp(4500) != 7070) {
		hlog("  FAIL sine(4500) = %d, want 7070\n", core_sine_lerp(4500));
		bad++;
	}
	prev = -1;
	for (i = 0; i <= 9000; i += 25) {
		n = core_sine_lerp(i);
		if (n < prev) {
			hlog("  FAIL sine not monotonic at %d: %d after %d\n",
			     i, n, prev);
			bad++;
			break;
		}
		prev = n;
	}

	/* 2. Waveform shapes, at a period of 100 ticks. */
	{
		core_effect_slot s;
		memset(&s, 0, sizeof(s));
		core_init(&cs, 0, 0);
		s.running  = 1;
		s.duration = CORE_FX_INFINITE;
		s.axis[0].periodic.magnitude = 100;
		s.axis[0].periodic.period    = 100;

		/* Square is UNIPOLAR - full for the first half, nothing after. */
		s.type = CORE_FX_SQUARE;
		if (core_effect_axis_value(&cs, &s, 0, 0) <= 0 ||
		    core_effect_axis_value(&cs, &s, 0, 60) != 0) {
			hlog("  FAIL square: %d at 0, %d at 60 (want >0 then 0)\n",
			     core_effect_axis_value(&cs, &s, 0, 0),
			     core_effect_axis_value(&cs, &s, 0, 60));
			bad++;
		}
		/* Sawtooth up runs from negative to positive across the period. */
		s.type = CORE_FX_SAWTOOTH_UP;
		if (core_effect_axis_value(&cs, &s, 0, 0) >= 0 ||
		    core_effect_axis_value(&cs, &s, 0, 99) <= 0) {
			hlog("  FAIL sawtooth up: %d at 0, %d at 99\n",
			     core_effect_axis_value(&cs, &s, 0, 0),
			     core_effect_axis_value(&cs, &s, 0, 99));
			bad++;
		}
		/* Sawtooth down is its mirror. */
		s.type = CORE_FX_SAWTOOTH_DOWN;
		if (core_effect_axis_value(&cs, &s, 0, 0) <= 0 ||
		    core_effect_axis_value(&cs, &s, 0, 99) >= 0) {
			hlog("  FAIL sawtooth down: %d at 0, %d at 99\n",
			     core_effect_axis_value(&cs, &s, 0, 0),
			     core_effect_axis_value(&cs, &s, 0, 99));
			bad++;
		}
		/* Everything is clamped to +/-0x7F however large the magnitude. */
		s.type = CORE_FX_CONSTANT;
		s.axis[0].periodic.magnitude = 127;
		if (core_effect_axis_value(&cs, &s, 0, 0) != CORE_FX_CLAMP) {
			hlog("  FAIL clamp: %d, want %d\n",
			     core_effect_axis_value(&cs, &s, 0, 0), CORE_FX_CLAMP);
			bad++;
		}
	}

	/* 3. Nothing running means no pulses at all. */
	core_init(&cs, 0, 0);
	core_effect_window(&cs, 0, pay);
	if (popcount32(pay) != 0) {
		hlog("  FAIL idle: %d pulses, want 0\n", popcount32(pay));
		bad++;
	}

	/*
	 * 4. The payload bitmap, against the ORIGINAL.
	 *
	 * These were produced by emulating drv_EffectEvaluate at 00013f10 with a
	 * single constant-force slot on both axes, the motor calibration
	 * drv_AddDevice seeds, the dither state zeroed and time starting at 0.
	 *
	 * NOTE THE DENSITY IS NOT MONOTONIC: it peaks at 31 of 32 pulses around
	 * magnitude 60 and then FALLS as the magnitude climbs. That is the
	 * dither, which arms whenever intensity exceeds 100 and suppresses more
	 * of the burst the higher the intensity goes. It looks like a bug and is
	 * not - the original does exactly the same, which is why these vectors
	 * come from the binary rather than from an assumption about what a
	 * modulator ought to do.
	 */
	{
		static const struct {
			s8  magnitude;
			u8  expect[CORE_EFFECT_PAYLOAD];
			int pulses;
		} FX[] = {
			{   0, { 0x00, 0x00, 0x00, 0x00 },  0 },
			{  20, { 0x56, 0x55, 0x4A, 0x29 }, 14 },
			{  40, { 0xFF, 0xBF, 0x77, 0xB7 }, 27 },
			{  60, { 0xFF, 0xFE, 0xFF, 0xFF }, 31 },
			{  80, { 0x1F, 0xFE, 0xFD, 0xFB }, 26 },
			{ 100, { 0x07, 0x7E, 0xFC, 0xF9 }, 21 },
			{ 120, { 0x07, 0x3E, 0xFC, 0xF8 }, 19 },
			{ 127, { 0x07, 0x1E, 0x7C, 0xF8 }, 17 }
		};
		int k;

		for (k = 0; k < (int)(sizeof(FX) / sizeof(FX[0])); k++) {
			effect_arm_constant(&cs, FX[k].magnitude);
			core_effect_window(&cs, 0, pay);
			n = popcount32(pay);
			htrace("magnitude %3d -> %02X %02X %02X %02X (%d pulses)\n",
			       FX[k].magnitude, pay[0], pay[1], pay[2], pay[3], n);
			if (memcmp(pay, FX[k].expect, CORE_EFFECT_PAYLOAD) != 0) {
				hlog("  FAIL effect mag %3d: got %02X %02X %02X %02X (%d), "
				     "want %02X %02X %02X %02X (%d)\n",
				     FX[k].magnitude, pay[0], pay[1], pay[2], pay[3], n,
				     FX[k].expect[0], FX[k].expect[1],
				     FX[k].expect[2], FX[k].expect[3], FX[k].pulses);
				bad++;
			}
		}
	}

	/* 5. Deterministic: the same state must give the same bitmap. */
	effect_arm_constant(&cs, 64);
	core_effect_window(&cs, 0, pay);
	effect_arm_constant(&cs, 64);
	core_effect_window(&cs, 0, pay2);
	if (memcmp(pay, pay2, sizeof(pay)) != 0) {
		hlog("  FAIL determinism: %02X%02X%02X%02X vs %02X%02X%02X%02X\n",
		     pay[0], pay[1], pay[2], pay[3],
		     pay2[0], pay2[1], pay2[2], pay2[3]);
		bad++;
	}

	/* 6. A finite effect stops contributing once its duration elapses. */
	core_init(&cs, 0, 0);
	cs.effect[0].type       = CORE_FX_CONSTANT;
	cs.effect[0].running    = 1;
	cs.effect[0].start_tick = 0;
	cs.effect[0].duration   = 4;
	cs.effect[0].axis[0].periodic.magnitude = 127;
	cs.effect[0].axis[1].periodic.magnitude = 127;
	core_effect_window(&cs, 100, pay);      /* long after it ended */
	if (popcount32(pay) != 0) {
		hlog("  FAIL expiry: %d pulses after the duration\n",
		     popcount32(pay));
		bad++;
	}

	/*
	 * 7. The type 0x40 condition, against the ORIGINAL.
	 *
	 * Emulated out of the pre-pass at the top of drv_EffectEvaluate with one
	 * running tuning slot, both axes carrying the same parameters, and the
	 * stick placed by hand. Both the computed output byte and the payload it
	 * leads to are checked, so a correct condition feeding a wrong evaluator
	 * would still be caught.
	 */
	{
		static const struct {
			s8  center, pos_coeff, neg_coeff, pos_sat, neg_sat, dead;
			s16 stick_x, stick_y;
			s8  out_x, out_y;
			u8  expect[CORE_EFFECT_PAYLOAD];
		} COND[] = {
		  {  0, 100, 100,   0,   0,  0,     0,    0,    0,   0,
			 { 0x00, 0x00, 0x00, 0x00 } },
		  {  0, 100, 100,   0,   0,  0,   600,    0,   50,   0,
			 { 0xBD, 0x56, 0xAD, 0xAA } },
		  {  0, 100, 100,   0,   0,  0,  -600,    0,  -50,   0,
			 { 0xBD, 0x56, 0xAD, 0xAA } },
		  {  0, 100, 100,   0,   0, 50,   600,    0,   10,   0,
			 { 0x10, 0x82, 0x10, 0x84 } },
		  {  0, 100, 100,  20,   0,  0,   600,    0,   20,   0,
			 { 0x24, 0x22, 0x22, 0x22 } },
		  {  0, 100, 100,   0, -20,  0,  -600,    0,  -20,   0,
			 { 0x24, 0x22, 0x22, 0x22 } },
		  { 40, 100, 100,   0,   0,  0,   600,    0,   18, -31,
			 { 0xDE, 0xAA, 0x56, 0xAA } },
		  {  0,  64, 127,   0,   0, 10,  1200, -900,   58, -85,
			 { 0x3F, 0xFE, 0xFD, 0xFB } },
		  {  0, 127, 127,   0,   0,  0,  1200, 1200,  127, 127,
			 { 0x07, 0x1E, 0x7C, 0xF8 } },
		  /*
		   * A NEGATIVE dead band behaves exactly like its positive twin,
		   * because the original takes the magnitude before comparing.
		   * Without these three the abs step was unreachable by the
		   * suite - a deliberate break went undetected until they were
		   * added.
		   */
		  {  0, 100, 100,   0,   0, -50,   600,    0,   10,   0,
			 { 0x10, 0x82, 0x10, 0x84 } },
		  {  0, 100, 100,   0,   0, -50,  -600,    0,  -10,   0,
			 { 0x10, 0x82, 0x10, 0x84 } },
		  /* A negative centre mirrors the whole response. */
		  {-40, 100, 100,   0,   0,   0,  -600,    0,  -18,  31,
			 { 0xDE, 0xAA, 0x56, 0xAA } }
		};
		int k, a;

		for (k = 0; k < (int)(sizeof(COND) / sizeof(COND[0])); k++) {
			core_init(&cs, 0, 0);
			cs.effect[0].type       = CORE_FX_TUNING;
			cs.effect[0].running    = 1;
			cs.effect[0].start_tick = 0;
			cs.effect[0].duration   = CORE_FX_INFINITE;
			for (a = 0; a < CORE_EFFECT_AXES; a++) {
				core_effect_condition *cd = &cs.effect[0].axis[a].condition;
				cd->center         = COND[k].center;
				cd->positive_coeff = COND[k].pos_coeff;
				cd->negative_coeff = COND[k].neg_coeff;
				cd->positive_sat   = COND[k].pos_sat;
				cd->negative_sat   = COND[k].neg_sat;
				cd->dead_band      = COND[k].dead;
				cd->output         = 0;
			}
			cs.stick_x = COND[k].stick_x;
			cs.stick_y = COND[k].stick_y;

			core_effect_window(&cs, 0, pay);

			if (cs.effect[0].axis[0].condition.output != COND[k].out_x ||
			    cs.effect[0].axis[1].condition.output != COND[k].out_y) {
				hlog("  FAIL condition %d: out %d,%d want %d,%d\n", k,
				     cs.effect[0].axis[0].condition.output,
				     cs.effect[0].axis[1].condition.output,
				     COND[k].out_x, COND[k].out_y);
				bad++;
			}
			if (memcmp(pay, COND[k].expect, CORE_EFFECT_PAYLOAD) != 0) {
				hlog("  FAIL condition %d payload: got %02X %02X %02X %02X, "
				     "want %02X %02X %02X %02X\n", k,
				     pay[0], pay[1], pay[2], pay[3],
				     COND[k].expect[0], COND[k].expect[1],
				     COND[k].expect[2], COND[k].expect[3]);
				bad++;
			}
			htrace("condition %d -> out %4d %4d  %02X %02X %02X %02X\n", k,
			       cs.effect[0].axis[0].condition.output,
			       cs.effect[0].axis[1].condition.output,
			       pay[0], pay[1], pay[2], pay[3]);
		}

		/*
		 * With strength 0 the pre-pass does not run at all, so a stale
		 * output byte is left untouched. The evaluator multiplies by
		 * strength, so it still contributes nothing.
		 */
		core_init(&cs, 0, 0);
		cs.effect[0].type    = CORE_FX_TUNING;
		cs.effect[0].running = 1;
		cs.effect[0].duration = CORE_FX_INFINITE;
		cs.effect[0].axis[0].condition.positive_coeff = 127;
		cs.effect[0].axis[0].condition.output = 99;
		cs.stick_x = 1200;
		cs.tune_strength = 0;
		core_effect_window(&cs, 0, pay);
		if (cs.effect[0].axis[0].condition.output != 99) {
			hlog("  FAIL condition strength 0: output was rewritten to %d\n",
			     cs.effect[0].axis[0].condition.output);
			bad++;
		}
		if (popcount32(pay) != 0) {
			hlog("  FAIL condition strength 0: %d pulses, want 0\n",
			     popcount32(pay));
			bad++;
		}
	}

	/*
	 * 8. THE RING, against the ORIGINAL.
	 *
	 * A realistic call sequence emulated out of drv_EffectEvaluate against a
	 * single persistent device, so the ring and the dither state carry over
	 * between calls exactly as they would in the driver.
	 *
	 * The point of the ring is the two ret=1 rows: nothing about the effect
	 * changed, the re-evaluated pulses still agree with what was stored, so
	 * the controller already holds a correct bitmap and NO TRANSFER HAPPENS.
	 * The payload is left untouched on those calls, which is why they expect
	 * the buffer to still hold the marker written before the call.
	 */
	{
		static const struct {
			int look;
			s32 tick;
			int set_mag;        /* -1 to leave the effect alone */
			int ret;
			s32 count;
			u8  expect[CORE_EFFECT_PAYLOAD];
		} RING[] = {
		  { 1,  0, 40, 0, 32, { 0xFF, 0xBF, 0x77, 0xB7 } },
		  { 1,  1, -1, 1, 32, { 0, 0, 0, 0 } },   /* unchanged: no send */
		  { 1,  2, -1, 1, 32, { 0, 0, 0, 0 } },   /* still unchanged    */
		  { 1,  3, 90, 0, 35, { 0xC7, 0x7F, 0xFF, 0xCF } },
		  { 1,  4, -1, 0, 36, { 0xE5, 0xEF, 0x7F, 0xFF } },
		  { 0, 32, -1, 0, 64, { 0xDF, 0xFF, 0xF7, 0x7F } }
		};
		int k, got;

		core_init(&cs, 0, 0);
		cs.effect[0].type       = CORE_FX_CONSTANT;
		cs.effect[0].running    = 1;
		cs.effect[0].start_tick = 0;
		cs.effect[0].duration   = CORE_FX_INFINITE;

		for (k = 0; k < (int)(sizeof(RING) / sizeof(RING[0])); k++) {
			if (RING[k].set_mag >= 0) {
				cs.effect[0].axis[0].periodic.magnitude = (s8)RING[k].set_mag;
				cs.effect[0].axis[1].periodic.magnitude = (s8)RING[k].set_mag;
			}
			memset(pay, 0, sizeof(pay));
			got = core_effect_evaluate(&cs, RING[k].look, RING[k].tick, pay);

			if (got != RING[k].ret) {
				hlog("  FAIL ring %d: returned %d, want %d\n",
				     k, got, RING[k].ret);
				bad++;
			}
			if (cs.ring_count != RING[k].count) {
				hlog("  FAIL ring %d: count %d, want %d\n",
				     k, (int)cs.ring_count, (int)RING[k].count);
				bad++;
			}
			if (memcmp(pay, RING[k].expect, CORE_EFFECT_PAYLOAD) != 0) {
				hlog("  FAIL ring %d payload: got %02X %02X %02X %02X, "
				     "want %02X %02X %02X %02X\n", k,
				     pay[0], pay[1], pay[2], pay[3],
				     RING[k].expect[0], RING[k].expect[1],
				     RING[k].expect[2], RING[k].expect[3]);
				bad++;
			}
			htrace("ring %d: look %d tick %2d -> ret %d count %2d "
			       "%02X %02X %02X %02X\n", k, RING[k].look, RING[k].tick,
			       got, (int)cs.ring_count, pay[0], pay[1], pay[2], pay[3]);
		}

		/* The idle motor-stop path drops every precomputed tick. */
		core_effect_ring_reset(&cs);
		if (cs.ring_count != 0) {
			hlog("  FAIL ring reset: count %d, want 0\n", (int)cs.ring_count);
			bad++;
		}
	}

	/* 9. A trace of one window, for diffing against the original. */
	effect_arm_constant(&cs, 64);
	for (i = 0; i < CORE_EFFECT_WINDOW; i++) {
		s32 intensity = core_effect_intensity(&cs, i);
		int pulse = core_effect_pulse(&cs, intensity, 0);
		htrace("tick %2d intensity %5d acc %3d pulse %d\n",
		       i, intensity, cs.accumulator, pulse);
	}

	hlog("Effect engine          : %s (9 groups)\n", bad ? "FAIL" : "ok");
	return bad;
}

/* ------------------------------------------------------------------ */
/* The effect send chain                                               */
/* ------------------------------------------------------------------ */

/*
 * Drive the chain to exhaustion: complete whatever is outstanding and let
 * each completion issue its successor, exactly as USB completions would.
 */
static void effect_pump(core_state *cs, u64 now)
{
	int guard = 0;

	while (g_bus_busy && guard++ < BUS_LOG_MAX) {
		g_bus_busy = 0;
		if (!core_effect_complete(cs, now)) {
			break;
		}
	}
}

static void effect_arm_chain(core_state *cs)
{
	core_init(cs, 0, 0);
	core_set_vendor(cs, harness_vendor, 0);
	cs->effect[0].type       = CORE_FX_CONSTANT;
	cs->effect[0].running    = 1;
	cs->effect[0].start_tick = 0;
	cs->effect[0].duration   = CORE_FX_INFINITE;
	cs->effect[0].axis[0].periodic.magnitude = 40;
	cs->effect[0].axis[1].periodic.magnitude = 40;
	bus_reset(0);
}

/*
 * The keep-alive decision, against the ORIGINAL. Emulated out of
 * drv_EffectKeepAlive with the clock forced to a chosen value: within three
 * seconds of the last poke it goes straight to the periodic send, otherwise
 * it pokes the register again and restamps.
 */
static int test_effect_chain(void)
{
	core_state cs;
	int bad = 0;
	int k;

	static const struct {
		u64  last;          /* keepalive_time going in  */
		u64  now;
		int  pokes;         /* does it poke the register */
		u64  after;         /* keepalive_time coming out */
		const char *what;
	} KA[] = {
		{          0, 100000000, 1, 100000000, "never poked"       },
		{  100000000, 110000000, 0, 100000000, "one second later"  },
		{  100000000, 129000000, 0, 100000000, "2.9s, still fresh" },
		{  100000000, 130000000, 0, 100000000, "exactly 3s"        },
		{  100000000, 131000000, 1, 131000000, "3.1s, stale"       },
		{  200000000, 100000000, 1, 100000000, "clock went back"   }
	};

	/* 1. The full chain from cold: bitmap, poke, periodic, then stop. */
	effect_arm_chain(&cs);
	core_effect_tick(&cs, 100000000);
	effect_pump(&cs, 100000000);
	if (g_bus_count != 3) {
		hlog("  FAIL chain: %d transfers, want 3\n", g_bus_count);
		bad++;
	} else if (g_bus_log[0].bRequest != CORE_FX_CMD_TICK ||
	           g_bus_log[1].bRequest != CORE_FX_CMD_KEEPALIVE ||
	           g_bus_log[2].bRequest != CORE_FX_CMD_PERIODIC) {
		hlog("  FAIL chain order: %02X %02X %02X, want %02X %02X %02X\n",
		     g_bus_log[0].bRequest, g_bus_log[1].bRequest,
		     g_bus_log[2].bRequest, CORE_FX_CMD_TICK,
		     CORE_FX_CMD_KEEPALIVE, CORE_FX_CMD_PERIODIC);
		bad++;
	} else if (g_bus_log[1].wValue != CORE_FX_KEEPALIVE_VALUE ||
	           g_bus_log[1].wIndex != CORE_FX_KEEPALIVE_INDEX) {
		hlog("  FAIL keepalive packet: val=%04X idx=%04X, want %04X %04X\n",
		     g_bus_log[1].wValue, g_bus_log[1].wIndex,
		     CORE_FX_KEEPALIVE_VALUE, CORE_FX_KEEPALIVE_INDEX);
		bad++;
	}
	/* The window sent as 0x36 must be the one the ring holds. */
	if (g_bus_count >= 1 && g_bus_log[0].wValue == 0 &&
	    g_bus_log[0].wIndex == 0) {
		hlog("  FAIL chain: the tick transfer carried an empty bitmap\n");
		bad++;
	}

	/* 2. The keep-alive decision itself. */
	for (k = 0; k < (int)(sizeof(KA) / sizeof(KA[0])); k++) {
		int poked;

		effect_arm_chain(&cs);
		cs.keepalive_time = KA[k].last;
		core_effect_tick(&cs, KA[k].now);
		effect_pump(&cs, KA[k].now);

		poked = (g_bus_count >= 2 &&
		         g_bus_log[1].bRequest == CORE_FX_CMD_KEEPALIVE) ? 1 : 0;
		if (poked != KA[k].pokes) {
			hlog("  FAIL keepalive %s: %s, want %s\n", KA[k].what,
			     poked ? "poked" : "skipped",
			     KA[k].pokes ? "poked" : "skipped");
			bad++;
		}
		if (cs.keepalive_time != KA[k].after) {
			hlog("  FAIL keepalive %s: stamp %llu, want %llu\n", KA[k].what,
			     (unsigned long long)cs.keepalive_time,
			     (unsigned long long)KA[k].after);
			bad++;
		}
		/* The periodic send closes every round either way. */
		if (g_bus_count != (KA[k].pokes ? 3 : 2)) {
			hlog("  FAIL keepalive %s: %d transfers\n",
			     KA[k].what, g_bus_count);
			bad++;
		}
		htrace("keepalive %-18s last=%10llu now=%10llu -> %s, %d transfers\n",
		       KA[k].what, (unsigned long long)KA[k].last,
		       (unsigned long long)KA[k].now,
		       poked ? "poke" : "skip", g_bus_count);
	}

	/*
	 * 3. A waiting timer tick preempts the chain: both the keep-alive and
	 *    the periodic step defer to it rather than carrying on.
	 */
	effect_arm_chain(&cs);
	core_effect_tick(&cs, 100000000);
	cs.claim_effect_tick = 1;
	g_bus_busy = 0;
	core_effect_complete(&cs, 100000000);
	if (cs.claim_effect_tick != 0) {
		hlog("  FAIL preempt: the claim was not consumed\n");
		bad++;
	}

	/* 4. With nothing playing there is nothing to send. */
	core_init(&cs, 0, 0);
	core_set_vendor(&cs, harness_vendor, 0);
	bus_reset(0);
	core_effect_tick(&cs, 100000000);
	if (g_bus_count != 1) {
		hlog("  FAIL idle: %d transfers, want 1\n", g_bus_count);
		bad++;
	}
	effect_pump(&cs, 100000000);

	hlog("Effect send chain      : %s (4 groups)\n", bad ? "FAIL" : "ok");
	return bad;
}

/* ------------------------------------------------------------------ */
/* Pak insert and remove                                               */
/* ------------------------------------------------------------------ */

/* The single vendor slot: claimable only while nothing is in flight. */
static int harness_claim(void *ctx)
{
	(void)ctx;
	return g_bus_busy ? 0 : 1;
}

/* A device that is past its probe and already playing something. */
static void pak_arm(core_state *cs, s32 effect_state, u8 prev_status)
{
	core_init(cs, 0, 0);
	core_set_vendor(cs, harness_vendor, 0);
	core_set_vendor_claim(cs, harness_claim);
	cs->accessory_state = CORE_ACC_FOUND_1;
	cs->effect_state    = effect_state;
	cs->prev_status     = prev_status;

	/*
	 * SOMETHING MUST BE PLAYING for the hook to send a window. The update
	 * path evaluates the ring BEFORE testing the idle count, so with no
	 * effect armed the idle count reaches the window length during that
	 * very evaluation and the hook sends the motor-stop instead. That is
	 * the original's ordering, not a quirk here - a pak change while
	 * nothing is rumbling legitimately means "stop the motor".
	 */
	cs->effect[0].type       = CORE_FX_CONSTANT;
	cs->effect[0].running    = 1;
	cs->effect[0].start_tick = 0;
	cs->effect[0].duration   = CORE_FX_INFINITE;
	cs->effect[0].axis[0].periodic.magnitude = 40;
	cs->effect[0].axis[1].periodic.magnitude = 40;
	bus_reset(0);
}

/* One poll, carrying a status byte and nothing else. */
static void pak_poll(core_state *cs, u8 status)
{
	u8 raw[CORE_RAW_PACKET_BYTES];

	raw[CORE_RAW_X]          = 0;
	raw[CORE_RAW_Y]          = 0;
	raw[CORE_RAW_STATUS]     = status;
	raw[CORE_RAW_BUTTONS_HI] = 0;
	raw[CORE_RAW_BUTTONS_LO] = 0;
	core_on_raw_packet(cs, raw);
}

#define PAK_IN  (CORE_STATUS_VALID | CORE_STATUS_PAK_PRESENT)
#define PAK_OUT (CORE_STATUS_VALID)

static int test_pak_change(void)
{
	core_state cs;
	int bad = 0;

	/* 1. Insert while the engine is sending: fires 0x34, state promotes. */
	pak_arm(&cs, CORE_FX_STATE_PERIODIC, PAK_OUT);
	pak_poll(&cs, PAK_IN);
	if (g_bus_count != 1 || g_bus_log[0].bRequest != CORE_FX_CMD_PAK_INSERT) {
		hlog("  FAIL pak insert: %d transfers, first %02X, want 1 and %02X\n",
		     g_bus_count, g_bus_count ? g_bus_log[0].bRequest : 0,
		     CORE_FX_CMD_PAK_INSERT);
		bad++;
	}
	if (cs.effect_state != CORE_FX_STATE_PAK) {
		hlog("  FAIL pak insert: state %d, want %d\n",
		     (int)cs.effect_state, CORE_FX_STATE_PAK);
		bad++;
	}

	/*
	 * 2. REMOVE while merely sending does NOT fire - it only promotes the
	 *    state. That asymmetry is the original's; see core_on_raw_packet.
	 */
	pak_arm(&cs, CORE_FX_STATE_PERIODIC, PAK_IN);
	pak_poll(&cs, PAK_OUT);
	if (g_bus_count != 0) {
		hlog("  FAIL pak remove in periodic: %d transfers, want 0\n",
		     g_bus_count);
		bad++;
	}
	if (cs.effect_state != CORE_FX_STATE_PAK) {
		hlog("  FAIL pak remove in periodic: state %d, want %d\n",
		     (int)cs.effect_state, CORE_FX_STATE_PAK);
		bad++;
	}

	/* 3. From the pak state, a remove does fire, with 0x35. */
	pak_arm(&cs, CORE_FX_STATE_PAK, PAK_IN);
	pak_poll(&cs, PAK_OUT);
	if (g_bus_count != 1 || g_bus_log[0].bRequest != CORE_FX_CMD_PAK_REMOVE) {
		hlog("  FAIL pak remove: %d transfers, first %02X, want 1 and %02X\n",
		     g_bus_count, g_bus_count ? g_bus_log[0].bRequest : 0,
		     CORE_FX_CMD_PAK_REMOVE);
		bad++;
	}

	/* 4. Before the engine starts sending, nothing is interrupted. */
	pak_arm(&cs, CORE_FX_STATE_TICK, PAK_OUT);
	pak_poll(&cs, PAK_IN);
	if (g_bus_count != 0) {
		hlog("  FAIL pak in tick state: %d transfers, want 0\n",
		     g_bus_count);
		bad++;
	}

	/* 5. No change in the bit means no hook, however often it is polled. */
	pak_arm(&cs, CORE_FX_STATE_PAK, PAK_IN);
	pak_poll(&cs, PAK_IN);
	pak_poll(&cs, PAK_IN);
	if (g_bus_count != 0) {
		hlog("  FAIL pak steady: %d transfers, want 0\n", g_bus_count);
		bad++;
	}

	/*
	 * 6. A busy slot defers the work rather than dropping it, and the
	 *    deferred drain picks it up once the slot frees.
	 */
	pak_arm(&cs, CORE_FX_STATE_PAK, PAK_OUT);
	g_bus_busy = 1;                     /* something already in flight */
	pak_poll(&cs, PAK_IN);
	if (g_bus_count != 0) {
		hlog("  FAIL pak deferred: %d transfers while busy, want 0\n",
		     g_bus_count);
		bad++;
	}
	if (!cs.claim_pak_insert) {
		hlog("  FAIL pak deferred: the claim was not recorded\n");
		bad++;
	}
	g_bus_busy = 0;
	core_effect_run_deferred(&cs, 100000000);
	if (g_bus_count != 1 ||
	    g_bus_log[0].bRequest != CORE_FX_CMD_PAK_INSERT) {
		hlog("  FAIL pak deferred: drain issued %d transfers, first %02X\n",
		     g_bus_count, g_bus_count ? g_bus_log[0].bRequest : 0);
		bad++;
	}
	if (cs.claim_pak_insert) {
		hlog("  FAIL pak deferred: the claim was not cleared\n");
		bad++;
	}

	/*
	 * 7. Once the engine has been idle long enough the hook stops the motor
	 *    instead of sending a window, and throws the ring away because it
	 *    describes silence.
	 */
	pak_arm(&cs, CORE_FX_STATE_PAK, PAK_OUT);
	cs.effect[0].running = 0;           /* nothing playing -> goes idle */
	cs.ring_count        = 32;
	pak_poll(&cs, PAK_IN);
	if (g_bus_count != 1 || g_bus_log[0].bRequest != CORE_FX_CMD_STOP) {
		hlog("  FAIL pak idle: %d transfers, first %02X, want 1 and %02X\n",
		     g_bus_count, g_bus_count ? g_bus_log[0].bRequest : 0,
		     CORE_FX_CMD_STOP);
		bad++;
	}
	if (cs.ring_count != 0) {
		hlog("  FAIL pak idle: ring count %d, want 0\n", (int)cs.ring_count);
		bad++;
	}

	/* 8. The motor-stop completion puts the engine back to the tick state. */
	core_effect_update_complete(&cs);
	if (cs.effect_state != CORE_FX_STATE_TICK) {
		hlog("  FAIL update complete: state %d, want %d\n",
		     (int)cs.effect_state, CORE_FX_STATE_TICK);
		bad++;
	}

	/*
	 * 9. Deferred work runs in priority order: a waiting timer tick beats
	 *    an insert, which beats a remove.
	 */
	pak_arm(&cs, CORE_FX_STATE_PAK, PAK_OUT);
	cs.claim_pak_remove  = 1;
	cs.claim_pak_insert  = 1;
	cs.claim_effect_tick = 1;
	core_effect_run_deferred(&cs, 100000000);
	if (g_bus_count < 1 || g_bus_log[0].bRequest != CORE_FX_CMD_TICK) {
		hlog("  FAIL deferred priority: first was %02X, want %02X\n",
		     g_bus_count ? g_bus_log[0].bRequest : 0, CORE_FX_CMD_TICK);
		bad++;
	}

	hlog("Pak insert and remove  : %s (9 groups)\n", bad ? "FAIL" : "ok");
	return bad;
}

/* ------------------------------------------------------------------ */
/* On-controller tuning mode                                           */
/* ------------------------------------------------------------------ */

/*
 * Every expectation below was emulated out of drv_BuildJoystickReport with
 * the button bytes and stick placed by hand and TuneMode seeded, then read
 * back out of the device extension.
 */
static int test_tune_mode(void)
{
	core_state cs;
	int bad = 0;
	int k;

	static const struct {
		u8  hi, lo;
		s32 x, y;
		s32 mode_in;
		s32 mode, period, duty, comp, strength;
		const char *what;
	} TUNE[] = {
	  {0x30, 0x30,   0,   0, 0,  3,  250, 50, 50, 100, "enter with Start"   },
	  {0xB0, 0x20,   0,   0, 0,  3,  250, 50, 50, 100, "enter with Reset"   },
	  {0x30, 0x30,  75,   0, 3,  3, 1000, 50, 50, 100, "stick full right"   },
	  {0x30, 0x30, -75,   0, 3,  3,    0, 50, 50, 100, "stick full left"    },
	  {0x30, 0x30,   0,  75, 3,  3,  250,  0,100, 100, "full up: duty 0"    },
	  {0x30, 0x30,   0, -75, 3,  3,  250,100,  0, 100, "full down: duty 100"},
	  {0x30, 0x38,   0,   0, 3,  3,  250, 50, 50, 100, "D-up preset"        },
	  {0x30, 0x34,   0,   0, 3,  3,  250, 50, 50,   0, "D-down preset"      },
	  {0x30, 0x31,   0,   0, 3,  3,  250, 50, 50,  25, "D-right preset"     },
	  {0x30, 0x35,   0,   0, 3,  3,  250, 50, 50,  12, "D-down and right"   },
	  {0x00, 0x00,   0,   0, 3,  1,  250, 50, 50, 100, "release: held drops"},
	  {0x00, 0x80,   0,   0, 1,  0,  250, 50, 50, 100, "press A: exit"      },
	  {0x20, 0x30,   0,   0, 0,  0,  500, 50, 50, 100, "only L: no entry"   }
	};

	for (k = 0; k < (int)(sizeof(TUNE) / sizeof(TUNE[0])); k++) {
		core_init(&cs, 0, 0);
		cs.tune_mode = TUNE[k].mode_in;
		/*
		 * The two stick cases that move the period need it seeded, since
		 * entry only zeroes it when the mode was not already active.
		 */
		if (TUNE[k].mode_in != 0) {
			cs.tune_period = 250;
		}
		core_tune_update(&cs, TUNE[k].hi, TUNE[k].lo, TUNE[k].x, TUNE[k].y);

		if (cs.tune_mode != TUNE[k].mode) {
			hlog("  FAIL tune %-20s mode %d, want %d\n",
			     TUNE[k].what, (int)cs.tune_mode, (int)TUNE[k].mode);
			bad++;
		}
		if (cs.tune_period != TUNE[k].period) {
			hlog("  FAIL tune %-20s period %d, want %d\n",
			     TUNE[k].what, (int)cs.tune_period, (int)TUNE[k].period);
			bad++;
		}
		if (cs.tune_duty != TUNE[k].duty ||
		    cs.tune_duty_complement != TUNE[k].comp) {
			hlog("  FAIL tune %-20s duty %d/%d, want %d/%d\n",
			     TUNE[k].what, (int)cs.tune_duty,
			     (int)cs.tune_duty_complement,
			     (int)TUNE[k].duty, (int)TUNE[k].comp);
			bad++;
		}
		if (cs.tune_strength != TUNE[k].strength) {
			hlog("  FAIL tune %-20s strength %d, want %d\n",
			     TUNE[k].what, (int)cs.tune_strength,
			     (int)TUNE[k].strength);
			bad++;
		}
		htrace("tune %-22s -> mode %d period %5d duty %3d comp %3d str %3d\n",
		       TUNE[k].what, (int)cs.tune_mode, (int)cs.tune_period,
		       (int)cs.tune_duty, (int)cs.tune_duty_complement,
		       (int)cs.tune_strength);
	}

	/* Leaving the mode kicks the engine so the new calibration takes hold. */
	core_init(&cs, 0, 0);
	cs.tune_mode = CORE_TUNE_ACTIVE;
	if (!core_tune_update(&cs, 0x00, 0x80, 0, 0)) {
		hlog("  FAIL tune exit did not ask for a kick\n");
		bad++;
	}

	/*
	 * While tuning, the motor is driven from the calibration and not from
	 * the effect set - so a window comes out even with nothing playing,
	 * which is the whole point of being able to feel the adjustment.
	 */
	{
		u8 pay[CORE_EFFECT_PAYLOAD];
		int idle_pulses, tuning_pulses;

		core_init(&cs, 0, 0);
		core_effect_window(&cs, 0, pay);
		idle_pulses = popcount32(pay);

		core_init(&cs, 0, 0);
		cs.tune_mode = CORE_TUNE_ACTIVE;
		core_effect_window(&cs, 0, pay);
		tuning_pulses = popcount32(pay);

		if (idle_pulses != 0) {
			hlog("  FAIL tune: idle produced %d pulses\n", idle_pulses);
			bad++;
		}
		if (tuning_pulses == 0) {
			hlog("  FAIL tune: tuning mode produced no pulses\n");
			bad++;
		}
		htrace("tune drive: idle %d pulses, tuning %d pulses\n",
		       idle_pulses, tuning_pulses);
	}

	hlog("Tuning mode            : %s (%d vectors)\n", bad ? "FAIL" : "ok",
	     (int)(sizeof(TUNE) / sizeof(TUNE[0])) + 2);
	return bad;
}

/* ------------------------------------------------------------------ */
/* The script interpreter                                              */
/* ------------------------------------------------------------------ */

/*
 * Each program below computes a value into the accumulator and stores it to
 * global 0, so the result is observable in MEMORY rather than in a return
 * value. That matters for how these were verified: the same bytecode was run
 * through drv_ScriptExecute under emulation and the resulting global read
 * back, which is robust in a way that reading the status out of the original
 * is not - its exit paths call the allocator, the spinlocks and the event
 * queue, and stubbing those perturbs the register the status comes back in.
 *
 * So the EXPECTED VALUES here are the original's, byte for byte. The status
 * codes are checked separately below against the documented table.
 */
#define SCRIPT_VARS 4

static u32 script_run_prog(const u32 *code, s32 count, int *status_out)
{
	core_script vm;
	u32 vars[SCRIPT_VARS + CORE_SCRIPT_LOCALS];
	int i;

	for (i = 0; i < SCRIPT_VARS + CORE_SCRIPT_LOCALS; i++) {
		vars[i] = 0;
	}
	core_script_init(&vm, code, count, vars, SCRIPT_VARS);
	i = core_script_run(&vm);
	if (status_out) {
		*status_out = i;
	}
	return vars[0];
}

/* load &global0; push; <body>; stora; ret */
#define PROLOGUE 0x110, 0x20000000u, 0x093
#define EPILOGUE 0x231, 0x082

static const u32 P00[] = {PROLOGUE, 0x110,42, EPILOGUE};
static const u32 P01[] = {PROLOGUE, 0x110,10,0x093,0x110,3,0x253, EPILOGUE};
static const u32 P02[] = {PROLOGUE, 0x110,10,0x093,0x110,3,0x252, EPILOGUE};
static const u32 P03[] = {PROLOGUE, 0x110,10,0x093,0x110,3,0x254, EPILOGUE};
static const u32 P04[] = {PROLOGUE, 0x110,17,0x093,0x110,5,0x255, EPILOGUE};
static const u32 P05[] = {PROLOGUE, 0x110,17,0x093,0x110,5,0x256, EPILOGUE};
static const u32 P06[] = {PROLOGUE, 0x110,0xFFFFFFF7u,0x093,0x110,2,0x255, EPILOGUE};
static const u32 P07[] = {PROLOGUE, 0x110,0xFFFFFFF7u,0x093,0x110,2,0x256, EPILOGUE};
static const u32 P08[] = {PROLOGUE, 0x110,1,0x093,0x110,4,0x250, EPILOGUE};
static const u32 P09[] = {PROLOGUE, 0x110,0xFFFFFF00u,0x093,0x110,4,0x251, EPILOGUE};
static const u32 P10[] = {PROLOGUE, 0x110,5,0x093,0x110,3,0x262, EPILOGUE};
static const u32 P11[] = {PROLOGUE, 0x110,3,0x093,0x110,5,0x262, EPILOGUE};
static const u32 P12[] = {PROLOGUE, 0x110,0xFFFFFFFBu,0x093,0x110,3,0x263, EPILOGUE};
static const u32 P13[] = {PROLOGUE, 0x110,5,0x093,0x110,5,0x264, EPILOGUE};
static const u32 P14[] = {PROLOGUE, 0x110,0,0x093,0x110,7,0x266, EPILOGUE};
static const u32 P15[] = {PROLOGUE, 0x110,0,0x093,0x110,7,0x267, EPILOGUE};
static const u32 P16[] = {PROLOGUE, 0x110,5,0x040, EPILOGUE};
static const u32 P17[] = {PROLOGUE, 0x110,5,0x041, EPILOGUE};
static const u32 P18[] = {PROLOGUE, 0x110,5,0x042, EPILOGUE};
static const u32 P19[] = {PROLOGUE, 0x110,1,0x172,2,0x110,111, EPILOGUE};
static const u32 P20[] = {PROLOGUE, 0x110,0,0x172,2,0x110,111,0x110,222, EPILOGUE};
static const u32 P21[] = {PROLOGUE, 0x110,9,0x093,0x110,4,0x095, EPILOGUE};

static int test_script(void)
{
	int bad = 0;
	int k, status;
	u32 got;

	static const struct {
		const u32 *code;
		s32        count;
		s32        expect;
		const char *what;
	} PROGS[] = {
	  {P00, (s32)(sizeof(P00)/4),   42, "load immediate"   },
	  {P01, (s32)(sizeof(P01)/4),    7, "10 - 3"           },
	  {P02, (s32)(sizeof(P02)/4),   13, "10 + 3"           },
	  {P03, (s32)(sizeof(P03)/4),   30, "10 * 3"           },
	  {P04, (s32)(sizeof(P04)/4),    3, "17 / 5"           },
	  {P05, (s32)(sizeof(P05)/4),    2, "17 % 5"           },
	  {P06, (s32)(sizeof(P06)/4),   -4, "-9 / 2"           },
	  {P07, (s32)(sizeof(P07)/4),   -1, "-9 % 2"           },
	  {P08, (s32)(sizeof(P08)/4),   16, "1 << 4"           },
	  {P09, (s32)(sizeof(P09)/4),  -16, "-256 >> 4"        },
	  {P10, (s32)(sizeof(P10)/4),    1, "5 > 3"            },
	  {P11, (s32)(sizeof(P11)/4),    0, "3 > 5"            },
	  {P12, (s32)(sizeof(P12)/4),    1, "-5 < 3"           },
	  {P13, (s32)(sizeof(P13)/4),    1, "5 >= 5"           },
	  {P14, (s32)(sizeof(P14)/4),    0, "0 && 7"           },
	  {P15, (s32)(sizeof(P15)/4),    1, "0 || 7"           },
	  {P16, (s32)(sizeof(P16)/4),    0, "!5"               },
	  {P17, (s32)(sizeof(P17)/4),   -6, "~5"               },
	  {P18, (s32)(sizeof(P18)/4),   -5, "-5"               },
	  {P19, (s32)(sizeof(P19)/4),  111, "branch not taken" },
	  {P20, (s32)(sizeof(P20)/4),  222, "branch taken"     },
	  /*
	   * swap leaves the wrong value where stora expects an index, so the
	   * store is REJECTED and the global keeps its zero. Kept because it
	   * pins down exactly that: a bad index stores nothing at all.
	   */
	  {P21, (s32)(sizeof(P21)/4),    0, "swap, then bad index" }
	};

	for (k = 0; k < (int)(sizeof(PROGS) / sizeof(PROGS[0])); k++) {
		got = script_run_prog(PROGS[k].code, PROGS[k].count, &status);
		if ((s32)got != PROGS[k].expect) {
			hlog("  FAIL script %-20s global0 = %d, want %d\n",
			     PROGS[k].what, (s32)got, (s32)PROGS[k].expect);
			bad++;
		}
		htrace("script %-22s -> global0 %6d status %2d\n",
		       PROGS[k].what, (s32)got, status);
	}

	/* Status codes, against the table in ../docs/script-bytecode.txt 6.4. */
	{
		static const u32 S_TERM[]    = {0x082};
		static const u32 S_DIV[]     = {0x110,1,0x093,0x110,0,0x255};
		static const u32 S_MOD[]     = {0x110,1,0x093,0x110,0,0x256};
		static const u32 S_UNDER[]   = {0x294};
		static const u32 S_ILLEGAL[] = {0x999};
		static const u32 S_LOOP[]    = {0x170,0xFFFFFFFEu};
		static const u32 S_BADVAR[]  = {0x110,0x20000004u,0x020};
		static const u32 S_BADLOC[]  = {0x110,0x300000C8u,0x020};
		/* 0x181 takes its target from the OPERAND, not the accumulator. */
		static const u32 S_NATIVE[]  = {0x181,0x40000009u};
		/* 0x080 is the one that calls through the accumulator. */
		static const u32 S_NATIVEA[] = {0x110,0x40000009u,0x080};
		static const u32 S_RANGE[]   = {0x110,42};
		static const u32 S_OVER[]    = {0x190,201};

		static const struct {
			const u32 *code;
			s32        count;
			int        expect;
			const char *what;
		} ST[] = {
		  {S_TERM,    1, CORE_SCRIPT_TERMINATED, "ret on empty stack"},
		  {S_DIV,     6, CORE_SCRIPT_DIV_ZERO,   "divide by zero"    },
		  {S_MOD,     6, CORE_SCRIPT_DIV_ZERO,   "modulo by zero"    },
		  {S_UNDER,   1, CORE_SCRIPT_UNDERFLOW,  "stack underflow"   },
		  {S_ILLEGAL, 1, CORE_SCRIPT_ILLEGAL,    "illegal opcode"    },
		  {S_LOOP,    2, CORE_SCRIPT_BUDGET_OUT, "infinite loop"     },
		  {S_BADVAR,  3, CORE_SCRIPT_BAD_VAR,    "global out of range"},
		  {S_BADLOC,  3, CORE_SCRIPT_BAD_VAR,    "local 200"         },
		  {S_NATIVE,  2, CORE_SCRIPT_BAD_NATIVE, "call native by operand"},
		  {S_NATIVEA, 3, CORE_SCRIPT_BAD_NATIVE, "call native by acc"   },
		  {S_RANGE,   2, CORE_SCRIPT_RANGE,      "ran off the end"   },
		  {S_OVER,    2, CORE_SCRIPT_OVERFLOW,   "stack overflow"    }
		};

		for (k = 0; k < (int)(sizeof(ST) / sizeof(ST[0])); k++) {
			script_run_prog(ST[k].code, ST[k].count, &status);
			if (status != ST[k].expect) {
				hlog("  FAIL script status %-20s got %d, want %d\n",
				     ST[k].what, status, ST[k].expect);
				bad++;
			}
			htrace("script status %-22s -> %2d\n", ST[k].what, status);
		}
	}

	/* The budget really is spent, not just tested. */
	{
		core_script vm;
		u32 vars[SCRIPT_VARS + CORE_SCRIPT_LOCALS];
		static const u32 loop[] = {0x170, 0xFFFFFFFEu};

		core_script_init(&vm, loop, 2, vars, SCRIPT_VARS);
		core_script_run(&vm);
		if (vm.budget > 0) {
			hlog("  FAIL script budget: %d left after a runaway loop\n",
			     (int)vm.budget);
			bad++;
		}
	}

	hlog("Script interpreter     : %s (%d programs)\n", bad ? "FAIL" : "ok",
	     (int)(sizeof(PROGS) / sizeof(PROGS[0])) + 12);
	return bad;
}

/* ------------------------------------------------------------------ */
/* the script scheduler                                                */
/* ------------------------------------------------------------------ */

/*
 * Allocation accounting. Every test ends by unloading and asserting that
 * the scheduler handed everything back, which is the whole reason the
 * allocator is a seam rather than a direct call.
 */
static int      g_sched_live;
static unsigned g_sched_total;
static int      g_sched_fail_in;   /* fail the Nth call from now; 0 = never */

static void *sched_test_alloc(void *ctx, u32 bytes)
{
	void *p;

	(void)ctx;
	if (g_sched_fail_in > 0 && --g_sched_fail_in == 0) {
		return 0;
	}
	p = malloc(bytes != 0 ? bytes : 1);
	if (p != 0) {
		g_sched_live++;
		g_sched_total++;
	}
	return p;
}

static void sched_test_free(void *ctx, void *block)
{
	(void)ctx;
	if (block != 0) {
		g_sched_live--;
		free(block);
	}
}

static u64 g_arm_time;
static int g_arm_calls;
static int g_cancel_calls;

/* A wake time of 0 is the cancel, which is what core_sched_unload arms for.
 * Counted apart, because every load tears down first and would otherwise
 * show up as a spurious arm. */
static void sched_test_arm(void *ctx, u64 wake)
{
	(void)ctx;
	if (wake == 0) {
		g_cancel_calls++;
		return;
	}
	g_arm_time = wake;
	g_arm_calls++;
}

#define SCHED_EVLOG 256
static struct {
	u32 type;
	u32 a1;
	u32 a2;
} g_evlog[SCHED_EVLOG];
static int g_evcount;

static void sched_test_event(void *ctx, u32 type, u32 a1, u32 a2)
{
	(void)ctx;
	if (g_evcount < SCHED_EVLOG) {
		g_evlog[g_evcount].type = type;
		g_evlog[g_evcount].a1   = a1;
		g_evlog[g_evcount].a2   = a2;
	}
	g_evcount++;
}

static void sched_reset_log(void)
{
	g_evcount      = 0;
	g_arm_calls    = 0;
	g_cancel_calls = 0;
	g_arm_time     = 0;
}

/*
 * The test builtin library. The real one is drv_ScriptNativeCall and is not
 * ported yet; these four exist to drive the scheduler paths that only a
 * native can reach.
 *
 *   0x40000001  _sleep   - wake this thread acc units from now, yield
 *   0x40000002  _mark    - post a debug event carrying the thread id
 *   0x40000003  _reenter - call the scheduler from inside the scheduler
 *   0x40000004  _spawn   - queue a second thread at acc, running at pc 0
 */
static u32 g_spawn_pc;

static int sched_test_native(void *ctx, core_script *vm, u32 id)
{
	core_sched *s = (core_sched *)ctx;

	switch (id) {
	case 0x40000001u:
		s->current->wake_time = s->sched_time + vm->acc;
		return CORE_SCRIPT_SLEEPING;

	case 0x40000002u:
		core_sched_post_event(s, CORE_EVENT_DEBUG, s->current->thread_id,
		                      (u32)s->thread_count);
		return CORE_SCRIPT_RUNNING;

	case 0x40000003u:
		core_sched_run(s, vm->acc);
		return CORE_SCRIPT_RUNNING;

	case 0x40000004u: {
		core_sched_thread *t = core_sched_thread_alloc(s, 0);

		if (t != 0) {
			t->pc        = (s32)g_spawn_pc;
			t->wake_time = vm->acc;
			core_sched_queue(s, t);
		}
		return CORE_SCRIPT_RUNNING;
	}

	case 0x40000005u:
		/*
		 * Report which handler is running and what its first argument
		 * was. The argument is read out of the LIVE local region, not
		 * the thread's saved stack: sched_execute unpacked it there.
		 */
		core_sched_post_event(s, CORE_EVENT_DEBUG, vm->acc,
		                      vm->vars[vm->var_count]);
		return CORE_SCRIPT_RUNNING;

	default:
		return CORE_SCRIPT_BAD_NATIVE;
	}
}

static void sched_test_open(core_sched *s)
{
	core_sched_init(s, sched_test_alloc, sched_test_free, 0);
	core_sched_set_arm(s, sched_test_arm, 0);
	core_sched_set_event_sink(s, sched_test_event, 0);
	core_sched_set_native(s, sched_test_native, s);
	s->device_tag = 0xABCDu;
	sched_reset_log();
}

static int sched_expect(int cond, const char *what, long got, long want,
                        int *bad)
{
	if (!cond) {
		hlog("  FAIL sched %-28s got %ld, want %ld\n", what, got, want);
		(*bad)++;
		return 0;
	}
	htrace("sched %-30s %ld\n", what, got);
	return 1;
}

static int test_sched(void)
{
	int bad    = 0;
	int groups = 0;

	/* ret on an empty stack terminates, which is how an entry function ends. */
	static const u32 PROG_RET[]   = {0x082};
	/* _mark, then terminate. */
	static const u32 PROG_MARK[]  = {0x181, 0x40000002u, 0x082};
	/* acc = 2000; _sleep; _mark; terminate. */
	static const u32 PROG_SLEEP[] = {0x110, 2000, 0x181, 0x40000001u,
		                             0x181, 0x40000002u, 0x082};
	/* An unconditional branch to itself: burns the whole budget. */
	static const u32 PROG_LOOP[]  = {0x170, 0xFFFFFFFEu};

	/* ---- 1. load runs thread 0 immediately ------------------------- */
	{
		core_sched s;

		g_sched_live  = 0;
		g_sched_total = 0;
		sched_test_open(&s);

		sched_expect(core_sched_load(&s, PROG_RET, 1, 4, 1000) == 1,
		             "load succeeds", 1, 1, &bad);
		sched_expect(s.thread_count == 0, "thread_count after load",
		             s.thread_count, 0, &bad);
		sched_expect(s.next_thread_id == 1, "thread 0 got id 1",
		             (long)s.next_thread_id, 1, &bad);
		/* Load credits a full bucket; thread 0 spends exactly one token. */
		sched_expect(s.budget == CORE_SCRIPT_BUDGET - 1, "budget after one op",
		             s.budget, CORE_SCRIPT_BUDGET - 1, &bad);
		sched_expect(g_arm_calls == 0, "no timer armed, list empty",
		             g_arm_calls, 0, &bad);
		sched_expect(s.fault_thread == 0, "no fault", 0, 0, &bad);

		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	/* ---- 2. the ready list runs in wake-time order ------------------ */
	{
		core_sched s;
		core_sched_thread *t;
		static const u64 WAKE[3] = {3000, 1000, 2000};
		int i;

		g_sched_live = 0;
		sched_test_open(&s);
		core_sched_load(&s, PROG_MARK, 3, 4, 100);
		sched_reset_log();

		/* ids 2, 3, 4 queued at 3000, 1000, 2000 */
		for (i = 0; i < 3; i++) {
			t = core_sched_thread_alloc(&s, 0);
			t->pc        = 0;
			t->wake_time = WAKE[i];
			core_sched_queue(&s, t);
		}
		core_sched_run(&s, 5000);

		sched_expect(g_evcount == 3, "three threads ran", g_evcount, 3, &bad);
		if (g_evcount == 3) {
			sched_expect(g_evlog[0].a1 == 3, "earliest wake ran first",
			             (long)g_evlog[0].a1, 3, &bad);
			sched_expect(g_evlog[1].a1 == 4, "then the middle one",
			             (long)g_evlog[1].a1, 4, &bad);
			sched_expect(g_evlog[2].a1 == 2, "then the latest",
			             (long)g_evlog[2].a1, 2, &bad);
		}
		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	/* ---- 3. equal wake times keep their arrival order --------------- */
	{
		core_sched s;
		core_sched_thread *a, *b, *c;

		g_sched_live = 0;
		sched_test_open(&s);
		core_sched_load(&s, PROG_MARK, 3, 4, 100);
		sched_reset_log();

		a = core_sched_thread_alloc(&s, 0); a->wake_time = 500;
		b = core_sched_thread_alloc(&s, 0); b->wake_time = 500;
		c = core_sched_thread_alloc(&s, 0); c->wake_time = 500;
		core_sched_queue(&s, a);
		core_sched_queue(&s, b);
		core_sched_queue(&s, c);
		core_sched_run(&s, 600);

		if (sched_expect(g_evcount == 3, "three equal-time threads ran",
		                 g_evcount, 3, &bad)) {
			sched_expect(g_evlog[0].a1 == a->thread_id &&
			             g_evlog[1].a1 == b->thread_id &&
			             g_evlog[2].a1 == c->thread_id,
			             "FIFO among equal wake times",
			             (long)g_evlog[0].a1, (long)a->thread_id, &bad);
		}
		core_sched_unload(&s);
		groups++;
	}

	/* ---- 4. a thread that is not due is left alone, and armed ------- */
	{
		core_sched s;
		core_sched_thread *t;

		g_sched_live = 0;
		sched_test_open(&s);
		core_sched_load(&s, PROG_MARK, 3, 4, 100);
		sched_reset_log();

		t = core_sched_thread_alloc(&s, 0);
		t->wake_time = 9000;
		core_sched_queue(&s, t);
		core_sched_run(&s, 5000);

		sched_expect(g_evcount == 0, "future thread did not run",
		             g_evcount, 0, &bad);
		sched_expect(s.thread_count == 1, "it is still queued",
		             s.thread_count, 1, &bad);
		sched_expect(g_arm_calls == 1, "the timer was armed once",
		             g_arm_calls, 1, &bad);
		sched_expect(g_arm_time == 9000, "armed for its wake time",
		             (long)g_arm_time, 9000, &bad);
		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	/* ---- 5. sleep yields, requeues, and resumes where it stopped ---- */
	{
		core_sched s;

		g_sched_live = 0;
		sched_test_open(&s);
		/* Thread 0 IS the sleeper: load runs a pass, and that pass reaches
		 * _sleep(2000) and yields. */
		core_sched_load(&s, PROG_SLEEP, 7, 4, 1000);

		sched_expect(g_evcount == 0, "nothing posted before the sleep ends",
		             g_evcount, 0, &bad);
		sched_expect(s.thread_count == 1, "the sleeper is back on the list",
		             s.thread_count, 1, &bad);
		sched_expect(g_arm_time == 3000, "armed for now + 2000",
		             (long)g_arm_time, 3000, &bad);

		core_sched_run(&s, 3000);
		sched_expect(g_evcount == 1, "it resumed and posted",
		             g_evcount, 1, &bad);
		sched_expect(s.thread_count == 0, "and then terminated",
		             s.thread_count, 0, &bad);
		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	/* ---- 6. budget exhaustion is a fault, not a pause --------------- */
	{
		core_sched s;

		g_sched_live = 0;
		sched_test_open(&s);
		sched_reset_log();
		core_sched_load(&s, PROG_LOOP, 2, 4, 0);

		sched_expect(g_evcount == 1, "one event posted", g_evcount, 1, &bad);
		if (g_evcount >= 1) {
			sched_expect(g_evlog[0].type == CORE_EVENT_FAULT,
			             "it is a fault event", (long)g_evlog[0].type,
			             CORE_EVENT_FAULT, &bad);
			sched_expect(g_evlog[0].a1 == CORE_SCRIPT_BUDGET_OUT,
			             "carrying status 10", (long)g_evlog[0].a1,
			             CORE_SCRIPT_BUDGET_OUT, &bad);
			sched_expect(g_evlog[0].a2 == 0xABCDu, "and the device tag",
			             (long)g_evlog[0].a2, 0xABCD, &bad);
		}
		sched_expect(s.thread_count == 0, "the thread was NOT requeued",
		             s.thread_count, 0, &bad);
		sched_expect(s.fault_thread != 0, "it is in the post-mortem slot",
		             s.fault_thread != 0, 1, &bad);
		sched_expect(s.fault_status == CORE_SCRIPT_BUDGET_OUT,
		             "post-mortem status", s.fault_status,
		             CORE_SCRIPT_BUDGET_OUT, &bad);
		sched_expect(s.fault_vars != 0, "globals were snapshotted",
		             s.fault_vars != 0, 1, &bad);
		sched_expect(g_arm_calls == 0, "nothing left to arm for",
		             g_arm_calls, 0, &bad);

		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	/* ---- 7. a re-entrant call defers instead of recursing ----------- */
	{
		core_sched s;
		core_sched_thread *t;
		/* The spawner at word 0, and at word 9 the body the spawned
		 * thread runs: _mark then terminate. */
		static const u32 PROG_BOTH[] = {0x110, 4000, 0x181, 0x40000004u,
			                            0x110, 4000, 0x181, 0x40000003u,
			                            0x082,
			                            0x181, 0x40000002u, 0x082};

		g_sched_live = 0;
		{
			sched_test_open(&s);
			g_spawn_pc = 9;
			core_sched_load(&s, PROG_BOTH, 12, 4, 100);
			sched_reset_log();

			t = core_sched_thread_alloc(&s, 0);
			t->pc        = 0;
			t->wake_time = 1000;
			core_sched_queue(&s, t);
			core_sched_run(&s, 1000);

			sched_expect(g_evcount == 1,
			             "the deferred pass ran the spawned thread",
			             g_evcount, 1, &bad);
			sched_expect(s.sched_time == 4000,
			             "the pass picked up the pending time",
			             (long)s.sched_time, 4000, &bad);
			sched_expect(s.running == 0, "the latch was released",
			             s.running, 0, &bad);
			sched_expect(s.thread_count == 0, "both threads are done",
			             s.thread_count, 0, &bad);
			core_sched_unload(&s);
			sched_expect(g_sched_live == 0, "all memory returned",
			             g_sched_live, 0, &bad);
		}
		groups++;
	}

	/* ---- 8. a full event ring drops its oldest ---------------------- */
	{
		core_sched s;
		int i;

		g_sched_live = 0;
		sched_test_open(&s);
		core_sched_load(&s, PROG_RET, 1, 4, 100);
		sched_reset_log();

		for (i = 0; i < 150; i++) {
			core_sched_post_event(&s, CORE_EVENT_DEBUG, (u32)i, 0);
		}
		sched_expect(s.event_count == CORE_SCHED_EVENTS, "ring is full",
		             s.event_count, CORE_SCHED_EVENTS, &bad);
		sched_expect(s.events_dropped == 50, "fifty were dropped",
		             (long)s.events_dropped, 50, &bad);

		core_sched_run(&s, 200);
		sched_expect(g_evcount == CORE_SCHED_EVENTS, "a hundred delivered",
		             g_evcount, CORE_SCHED_EVENTS, &bad);
		if (g_evcount == CORE_SCHED_EVENTS) {
			sched_expect(g_evlog[0].a1 == 50, "the oldest survivor is 50",
			             (long)g_evlog[0].a1, 50, &bad);
			sched_expect(g_evlog[99].a1 == 149, "the newest is 149",
			             (long)g_evlog[99].a1, 149, &bad);
		}
		core_sched_unload(&s);
		groups++;
	}

	/* ---- 9. the free pool is reused, but only when it fits ---------- */
	{
		core_sched s;
		core_sched_thread *t;
		unsigned before;

		g_sched_live = 0;
		sched_test_open(&s);
		core_sched_load(&s, PROG_RET, 1, 4, 100);   /* thread 0 terminated */

		before = g_sched_total;
		t = core_sched_thread_alloc(&s, 0);
		sched_expect(g_sched_total == before,
		             "a retired node was reused",
		             (long)(g_sched_total - before), 0, &bad);
		sched_expect(t->stack_size == CORE_SCHED_MIN_STACK,
		             "with its original capacity", t->stack_size,
		             CORE_SCHED_MIN_STACK, &bad);
		t->wake_time = 100;
		core_sched_queue(&s, t);
		core_sched_run(&s, 100);                    /* retires it again */

		before = g_sched_total;
		t = core_sched_thread_alloc(&s, 64);
		sched_expect(g_sched_total == before + 1,
		             "a node too small was not reused",
		             (long)(g_sched_total - before), 1, &bad);
		sched_expect(t->stack_size == 64, "the new one is the size asked for",
		             t->stack_size, 64, &bad);
		t->wake_time = 100;
		core_sched_queue(&s, t);
		core_sched_run(&s, 100);

		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	/* ---- 10. a failed allocation during load fails cleanly ---------- */
	{
		core_sched s;
		int i;

		for (i = 1; i <= 3; i++) {
			g_sched_live    = 0;
			sched_test_open(&s);
			g_sched_fail_in = i;        /* fail the i'th allocation */
			sched_expect(core_sched_load(&s, PROG_RET, 1, 4, 100) == 0,
			             "load reports failure", 0, 0, &bad);
			g_sched_fail_in = 0;
			sched_expect(g_sched_live == 0, "and leaks nothing",
			             g_sched_live, 0, &bad);
			core_sched_unload(&s);
		}
		groups++;
	}

	hlog("Script scheduler       : %s (%d groups)\n", bad ? "FAIL" : "ok",
	     groups);
	return bad;
}

/* ------------------------------------------------------------------ */
/* script input binding                                                */
/* ------------------------------------------------------------------ */

/*
 * Every handler slot gets its own five-word body in one generated program:
 *
 *     word 0            0x082          ret, so thread 0 from the load ends
 *     1 + slot*5        0x110, slot    acc = the slot number
 *     1 + slot*5 + 2    0x181, _report post an event (acc, arg0)
 *     1 + slot*5 + 4    0x082          ret
 *
 * so a single debug event says both which slot fired and what its first
 * argument was.
 */
#define BIND_SLOTS  20
#define BIND_WORDS  (1 + BIND_SLOTS * 5)

static u32 g_bind_code[BIND_WORDS];

static void bind_build_code(void)
{
	int k;

	g_bind_code[0] = 0x082;
	for (k = 0; k < BIND_SLOTS; k++) {
		u32 *p = &g_bind_code[1 + k * 5];

		p[0] = 0x110;
		p[1] = (u32)k;
		p[2] = 0x181;
		p[3] = 0x40000005u;
		p[4] = 0x082;
	}
}

/* Bind every slot, or unbind one by passing it as except. */
static void bind_all(core_sched *s, int except)
{
	int k;

	for (k = 0; k < BIND_SLOTS; k++) {
		s->vm.vars[k] = (k == except)
		              ? 0u
		              : (CORE_TAG_CODE | (u32)(1 + k * 5));
	}
}

static void bind_raw(u8 *raw, int x, int y, u32 btn_word)
{
	raw[CORE_RAW_X]          = (u8)x;
	raw[CORE_RAW_Y]          = (u8)y;
	raw[CORE_RAW_STATUS]     = CORE_STATUS_VALID;
	raw[CORE_RAW_BUTTONS_HI] = (u8)(btn_word & 0xFFu);
	raw[CORE_RAW_BUTTONS_LO] = (u8)((btn_word >> 8) & 0xFFu);
}

static int test_input_bind(void)
{
	int bad    = 0;
	int groups = 0;
	core_sched s;
	u8 raw[CORE_RAW_PACKET_BYTES];

	bind_build_code();

	/* ---- 1. nothing changed queues nothing ------------------------- */
	{
		g_sched_live = 0;
		sched_test_open(&s);
		core_sched_load(&s, g_bind_code, BIND_WORDS, BIND_SLOTS, 100);
		bind_all(&s, -1);

		bind_raw(raw, 0, 0, 0);
		core_sched_on_input(&s, raw, 200);     /* first packet: a change */
		sched_reset_log();
		core_sched_on_input(&s, raw, 300);     /* identical */

		sched_expect(g_evcount == 0, "identical packet queues nothing",
		             g_evcount, 0, &bad);
		sched_expect(s.thread_count == 0, "and leaves no threads",
		             s.thread_count, 0, &bad);
		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	/* ---- 2. one button press, and the order handlers run in -------- */
	{
		g_sched_live = 0;
		sched_test_open(&s);
		core_sched_load(&s, g_bind_code, BIND_WORDS, BIND_SLOTS, 100);
		bind_all(&s, -1);

		bind_raw(raw, 0, 0, 0);
		core_sched_on_input(&s, raw, 200);
		sched_reset_log();

		bind_raw(raw, 0, 0, 0x8000u);          /* slot 0 is bit 15 */
		core_sched_on_input(&s, raw, 300);

		if (sched_expect(g_evcount == 4, "pre, aggregate, button, post",
		                 g_evcount, 4, &bad)) {
			sched_expect(g_evlog[0].a1 == CORE_SCHED_SLOT_PRE,
			             "pre handler ran first", (long)g_evlog[0].a1,
			             CORE_SCHED_SLOT_PRE, &bad);
			sched_expect(g_evlog[1].a1 == CORE_SCHED_SLOT_BUTTONS,
			             "then the aggregate", (long)g_evlog[1].a1,
			             CORE_SCHED_SLOT_BUTTONS, &bad);
			sched_expect(g_evlog[2].a1 == 0, "then button slot 0",
			             (long)g_evlog[2].a1, 0, &bad);
			sched_expect(g_evlog[2].a2 == 1, "with state pressed",
			             (long)g_evlog[2].a2, 1, &bad);
			sched_expect(g_evlog[3].a1 == CORE_SCHED_SLOT_POST,
			             "post handler ran last", (long)g_evlog[3].a1,
			             CORE_SCHED_SLOT_POST, &bad);
			/*
			 * SIGNED. The original reads the button word with MOVSX,
			 * so bit 15 makes the argument negative and a script
			 * comparing it against 0x8000 would never match.
			 */
			sched_expect((s32)g_evlog[1].a2 == -32768,
			             "aggregate word is sign-extended",
			             (long)(s32)g_evlog[1].a2, -32768, &bad);
		}

		/* releasing it fires the same slot with state 0 */
		sched_reset_log();
		bind_raw(raw, 0, 0, 0);
		core_sched_on_input(&s, raw, 400);
		if (sched_expect(g_evcount == 4, "release fires the same four",
		                 g_evcount, 4, &bad)) {
			sched_expect(g_evlog[2].a1 == 0 && g_evlog[2].a2 == 0,
			             "button slot 0 released", (long)g_evlog[2].a2,
			             0, &bad);
		}
		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	/* ---- 3. the stick alone does not wake the button handlers ------ */
	{
		g_sched_live = 0;
		sched_test_open(&s);
		core_sched_load(&s, g_bind_code, BIND_WORDS, BIND_SLOTS, 100);
		bind_all(&s, -1);

		bind_raw(raw, 0, 0, 0);
		core_sched_on_input(&s, raw, 200);
		sched_reset_log();

		bind_raw(raw, -40, 25, 0);
		core_sched_on_input(&s, raw, 300);

		if (sched_expect(g_evcount == 3, "pre, stick, post", g_evcount, 3,
		                 &bad)) {
			sched_expect(g_evlog[0].a1 == CORE_SCHED_SLOT_PRE, "pre first",
			             (long)g_evlog[0].a1, CORE_SCHED_SLOT_PRE, &bad);
			sched_expect(g_evlog[1].a1 == CORE_SCHED_SLOT_STICK, "then stick",
			             (long)g_evlog[1].a1, CORE_SCHED_SLOT_STICK, &bad);
			sched_expect((s32)g_evlog[1].a2 == -40,
			             "stick handler got signed X",
			             (long)(s32)g_evlog[1].a2, -40, &bad);
			sched_expect(g_evlog[2].a1 == CORE_SCHED_SLOT_POST, "then post",
			             (long)g_evlog[2].a1, CORE_SCHED_SLOT_POST, &bad);
		}
		core_sched_unload(&s);
		groups++;
	}

	/* ---- 4. an unbound slot is simply skipped ---------------------- */
	{
		g_sched_live = 0;
		sched_test_open(&s);
		core_sched_load(&s, g_bind_code, BIND_WORDS, BIND_SLOTS, 100);
		bind_all(&s, CORE_SCHED_SLOT_PRE);        /* unbind the pre handler */

		bind_raw(raw, 0, 0, 0);
		core_sched_on_input(&s, raw, 200);
		sched_reset_log();

		bind_raw(raw, 0, 0, 0x8000u);
		core_sched_on_input(&s, raw, 300);

		if (sched_expect(g_evcount == 3, "the unbound slot did not fire",
		                 g_evcount, 3, &bad)) {
			sched_expect(g_evlog[0].a1 == CORE_SCHED_SLOT_BUTTONS,
			             "aggregate is now first", (long)g_evlog[0].a1,
			             CORE_SCHED_SLOT_BUTTONS, &bad);
		}
		core_sched_unload(&s);
		groups++;
	}

	/* ---- 5. slot n IS raw button index n --------------------------- */
	/*
	 * Cross-checked against core_decode, which is already verified against
	 * drv_BuildJoystickReport by 26 vectors: pressing the bit that the
	 * dispatcher reports as slot n must make core light HID button
	 * button_map[n]. That ties the two readings of the same word together
	 * rather than asserting the mapping twice from the same assumption.
	 */
	{
		core_state cs;
		int n;
		int checked = 0;

		g_sched_live = 0;
		sched_test_open(&s);
		core_sched_load(&s, g_bind_code, BIND_WORDS, BIND_SLOTS, 100);
		bind_all(&s, -1);
		core_init(&cs, 0, 0);

		for (n = 0; n < CORE_RAW_BUTTON_BITS; n++) {
			u32 word = 1u << (15 - n);
			u8  dest = cs.button_map[n];

			bind_raw(raw, 0, 0, 0);
			core_sched_on_input(&s, raw, 1000 + (u64)n * 10);
			sched_reset_log();

			bind_raw(raw, 0, 0, word);
			core_sched_on_input(&s, raw, 1005 + (u64)n * 10);

			/* pre, aggregate, the button, post */
			if (g_evcount != 4 || g_evlog[2].a1 != (u32)n) {
				hlog("  FAIL bind raw bit %2d -> slot %ld, want %d "
				     "(%d events)\n", n,
				     g_evcount >= 3 ? (long)g_evlog[2].a1 : -1L, n,
				     g_evcount);
				bad++;
				continue;
			}
			if (dest == CORE_BUTTON_NONE) {
				checked++;
				continue;       /* filler bit, no HID button */
			}
			core_decode(&cs, raw);
			if ((cs.buttons & (u16)(1u << dest)) == 0) {
				hlog("  FAIL bind slot %d fired but core did not light "
				     "HID button %d (buttons=%04x)\n", n, dest,
				     cs.buttons);
				bad++;
				continue;
			}
			checked++;
		}
		sched_expect(checked == CORE_RAW_BUTTON_BITS,
		             "all 16 bits agree with core_decode", checked,
		             CORE_RAW_BUTTON_BITS, &bad);
		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	/* ---- 6. several buttons at once, in slot order ----------------- */
	{
		g_sched_live = 0;
		sched_test_open(&s);
		core_sched_load(&s, g_bind_code, BIND_WORDS, BIND_SLOTS, 100);
		bind_all(&s, -1);

		bind_raw(raw, 0, 0, 0);
		core_sched_on_input(&s, raw, 200);
		sched_reset_log();

		/* bits 15, 12 and 0 -> slots 0, 3 and 15 */
		bind_raw(raw, 7, -7, 0x9001u);
		core_sched_on_input(&s, raw, 300);

		/* pre, stick, aggregate, three buttons, post */
		if (sched_expect(g_evcount == 7, "seven handlers ran", g_evcount,
		                 7, &bad)) {
			sched_expect(g_evlog[0].a1 == CORE_SCHED_SLOT_PRE, "pre",
			             (long)g_evlog[0].a1, CORE_SCHED_SLOT_PRE, &bad);
			sched_expect(g_evlog[1].a1 == CORE_SCHED_SLOT_STICK, "stick",
			             (long)g_evlog[1].a1, CORE_SCHED_SLOT_STICK, &bad);
			sched_expect(g_evlog[2].a1 == CORE_SCHED_SLOT_BUTTONS, "aggregate",
			             (long)g_evlog[2].a1, CORE_SCHED_SLOT_BUTTONS, &bad);
			sched_expect(g_evlog[3].a1 == 0, "slot 0", (long)g_evlog[3].a1,
			             0, &bad);
			sched_expect(g_evlog[4].a1 == 3, "slot 3", (long)g_evlog[4].a1,
			             3, &bad);
			sched_expect(g_evlog[5].a1 == 15, "slot 15",
			             (long)g_evlog[5].a1, 15, &bad);
			sched_expect(g_evlog[6].a1 == CORE_SCHED_SLOT_POST, "post",
			             (long)g_evlog[6].a1, CORE_SCHED_SLOT_POST, &bad);
		}
		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	hlog("Script input binding   : %s (%d groups)\n", bad ? "FAIL" : "ok",
	     groups);
	return bad;
}

/* ------------------------------------------------------------------ */
/* the native builtin library                                          */
/* ------------------------------------------------------------------ */

#define NAT_VARS   4

/*
 * Assemble "store the result of one builtin call into global 0".
 *
 *     LOAD &global0 ; PUSH          the destination, for the store at the end
 *     [ LOC k ; PUSH ; LOAD v ; STORA ]   once per argument
 *     CALL id
 *     STORA                         global0 = the accumulator
 *     RET
 *
 * The argument blocks return the stack to the depth they found it, so at the
 * call the depth is 1 and LOC k names slot 1+k - exactly where the builtin
 * reads argument k. That is the convention being exercised, not an accident
 * of the encoding.
 */
static s32 nat_prog(u32 *c, u32 id, int argc, u32 a1, u32 a2)
{
	s32 n = 0;

	c[n++] = 0x110;  c[n++] = CORE_TAG_GLOBAL | 0u;
	c[n++] = 0x093;
	if (argc >= 1) {
		c[n++] = 0x192; c[n++] = 1;
		c[n++] = 0x093;
		c[n++] = 0x110; c[n++] = a1;
		c[n++] = 0x231;
	}
	if (argc >= 2) {
		c[n++] = 0x192; c[n++] = 2;
		c[n++] = 0x093;
		c[n++] = 0x110; c[n++] = a2;
		c[n++] = 0x231;
	}
	c[n++] = 0x181; c[n++] = id;
	c[n++] = 0x231;
	c[n++] = 0x082;
	return n;
}

/* Load that program as thread 0 and run one pass. */
static void nat_run(core_sched *s, core_state *cs, u32 id, int argc,
                    u32 a1, u32 a2, u64 now)
{
	u32 code[40];
	s32 n = nat_prog(code, id, argc, a1, a2);

	sched_test_open(s);
	core_sched_set_core(s, cs);
	core_sched_set_native(s, core_sched_native, s);
	sched_reset_log();
	core_sched_load(s, code, n, NAT_VARS, now);
}

static int test_natives(void)
{
	int bad    = 0;
	int groups = 0;
	core_sched s;
	core_state cs;

	/* ---- 1. the builtins that post events -------------------------- */
	{
		static const struct {
			u32 id;
			u32 a1, a2;
			u32 type, e1, e2;
			const char *what;
		} EV[] = {
		  {CORE_FN_KEY,          0x04, 1, CORE_EVENT_KEY,          0x04, 1,
		   "_key(A, down)"},
		  {CORE_FN_KEY,          0xE1, 0, CORE_EVENT_KEY,          0xE1, 0,
		   "_key(LShift, up)"},
		  {CORE_FN_MOUSE_BUTTON, 2,    1, CORE_EVENT_MOUSE_BUTTON, 2,    1,
		   "_mouse_button(2, down)"},
		  {CORE_FN_MOUSE_REL,    0xFFFFFFF6u, 9, CORE_EVENT_MOUSE_REL,
		   0xFFFFFFF6u, 9, "_mouse_relative(-10, 9)"},
		  {CORE_FN_DEBUG,        111, 222, CORE_EVENT_DEBUG,       111, 222,
		   "_debug(111, 222)"},
		  /* clamped to 0..0xFFFF, both ends */
		  {CORE_FN_MOUSE_ABS, 0xFFFFFFFBu, 70000, CORE_EVENT_MOUSE_ABS,
		   0, 0xFFFF, "_mouse_absolute clamps"},
		  {CORE_FN_MOUSE_ABS, 0x8000, 0x4000, CORE_EVENT_MOUSE_ABS,
		   0x8000, 0x4000, "_mouse_absolute passes"}
		};
		int k;

		for (k = 0; k < (int)(sizeof(EV) / sizeof(EV[0])); k++) {
			core_init(&cs, 0, 0);
			g_sched_live = 0;
			nat_run(&s, &cs, EV[k].id, 2, EV[k].a1, EV[k].a2, 1000);

			if (g_evcount != 1) {
				hlog("  FAIL native %-24s posted %d events, want 1\n",
				     EV[k].what, g_evcount);
				bad++;
			} else if (g_evlog[0].type != EV[k].type ||
			           g_evlog[0].a1 != EV[k].e1 ||
			           g_evlog[0].a2 != EV[k].e2) {
				hlog("  FAIL native %-24s got (%u,%u,%u), want (%u,%u,%u)\n",
				     EV[k].what, g_evlog[0].type, g_evlog[0].a1,
				     g_evlog[0].a2, EV[k].type, EV[k].e1, EV[k].e2);
				bad++;
			}
			htrace("native %-26s -> event %u (%u, %u)\n", EV[k].what,
			       g_evlog[0].type, g_evlog[0].a1, g_evlog[0].a2);
			core_sched_unload(&s);
			sched_expect(g_sched_live == 0, "all memory returned",
			             g_sched_live, 0, &bad);
		}
		groups++;
	}

	/* ---- 2. _button sets and clears HID button bits ----------------- */
	{
		core_init(&cs, 0, 0);
		cs.buttons = 0;
		g_sched_live = 0;
		nat_run(&s, &cs, CORE_FN_BUTTON, 2, 1, 1, 1000);
		sched_expect(cs.buttons == 0x0001, "_button(1, down)",
		             cs.buttons, 0x0001, &bad);
		core_sched_unload(&s);

		nat_run(&s, &cs, CORE_FN_BUTTON, 2, 16, 1, 1000);
		sched_expect(cs.buttons == 0x8001, "_button(16, down)",
		             cs.buttons, 0x8001, &bad);
		core_sched_unload(&s);

		nat_run(&s, &cs, CORE_FN_BUTTON, 2, 1, 0, 1000);
		sched_expect(cs.buttons == 0x8000, "_button(1, up)",
		             cs.buttons, 0x8000, &bad);
		core_sched_unload(&s);

		/* out of range is ignored, not a fault */
		nat_run(&s, &cs, CORE_FN_BUTTON, 2, 17, 1, 1000);
		sched_expect(cs.buttons == 0x8000, "_button(17) ignored",
		             cs.buttons, 0x8000, &bad);
		sched_expect(s.fault_thread == 0, "and does not fault",
		             s.fault_thread != 0, 0, &bad);
		core_sched_unload(&s);

		nat_run(&s, &cs, CORE_FN_BUTTON, 2, 0, 1, 1000);
		sched_expect(cs.buttons == 0x8000, "_button(0) ignored",
		             cs.buttons, 0x8000, &bad);
		core_sched_unload(&s);
		groups++;
	}

	/* ---- 3. _stick and _stick_relative, with the clamp -------------- */
	{
		core_init(&cs, 0, 0);
		g_sched_live = 0;

		nat_run(&s, &cs, CORE_FN_STICK, 2, 300, 0xFFFFFED4u, 1000);
		sched_expect(cs.stick_x == 300 && cs.stick_y == -300,
		             "_stick(300, -300)", cs.stick_x, 300, &bad);
		core_sched_unload(&s);

		nat_run(&s, &cs, CORE_FN_STICK, 2, 9000, 0xFFFFDCD8u, 1000);
		sched_expect(cs.stick_x == CORE_STICK_LIMIT, "_stick clamps high",
		             cs.stick_x, CORE_STICK_LIMIT, &bad);
		sched_expect(cs.stick_y == -CORE_STICK_LIMIT, "_stick clamps low",
		             cs.stick_y, -CORE_STICK_LIMIT, &bad);
		core_sched_unload(&s);

		cs.stick_x = 100;
		cs.stick_y = 100;
		nat_run(&s, &cs, CORE_FN_STICK_REL, 2, 50, 0xFFFFFFCEu, 1000);
		sched_expect(cs.stick_x == 150, "_stick_relative adds X",
		             cs.stick_x, 150, &bad);
		sched_expect(cs.stick_y == 50, "_stick_relative adds Y",
		             cs.stick_y, 50, &bad);
		core_sched_unload(&s);

		cs.stick_x = 1190;
		nat_run(&s, &cs, CORE_FN_STICK_REL, 2, 100, 0, 1000);
		sched_expect(cs.stick_x == CORE_STICK_LIMIT,
		             "_stick_relative clamps", cs.stick_x,
		             CORE_STICK_LIMIT, &bad);
		core_sched_unload(&s);
		groups++;
	}

	/* ---- 4. _getpid, _time, and the ignored pair -------------------- */
	{
		core_init(&cs, 0, 0);
		g_sched_live = 0;

		nat_run(&s, &cs, CORE_FN_GETPID, 0, 0, 0, 1000);
		sched_expect(s.vm.vars[0] == 1, "_getpid of thread 0",
		             (long)s.vm.vars[0], 1, &bad);
		core_sched_unload(&s);

		/* thread 0 wakes at the load time, so _time is 0 there */
		nat_run(&s, &cs, CORE_FN_TIME, 0, 0, 0, 1000);
		sched_expect(s.vm.vars[0] == 0, "_time at load", (long)s.vm.vars[0],
		             0, &bad);
		core_sched_unload(&s);

		/* accepted, ignored, and NOT a fault - unlike an unknown id */
		nat_run(&s, &cs, CORE_FN_STICK_SWAP, 2, 1, 2, 1000);
		sched_expect(s.fault_thread == 0, "_stick_swap does not fault",
		             s.fault_thread != 0, 0, &bad);
		sched_expect(g_evcount == 0, "and posts nothing", g_evcount, 0,
		             &bad);
		core_sched_unload(&s);

		nat_run(&s, &cs, CORE_FN_SET_RUMBLE, 2, 1, 2, 1000);
		sched_expect(s.fault_thread == 0, "_set_rumble does not fault",
		             s.fault_thread != 0, 0, &bad);
		core_sched_unload(&s);

		/* an unknown id IS a fault, status 8 */
		nat_run(&s, &cs, 0x40000099u, 0, 0, 0, 1000);
		sched_expect(s.fault_status == CORE_SCRIPT_BAD_NATIVE,
		             "unknown builtin faults", s.fault_status,
		             CORE_SCRIPT_BAD_NATIVE, &bad);
		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	/* ---- 5. _exit terminates without faulting ----------------------- */
	{
		core_init(&cs, 0, 0);
		g_sched_live = 0;
		nat_run(&s, &cs, CORE_FN_EXIT, 0, 0, 0, 1000);

		sched_expect(s.fault_thread == 0, "_exit is not a fault",
		             s.fault_thread != 0, 0, &bad);
		sched_expect(s.thread_count == 0, "and the thread is gone",
		             s.thread_count, 0, &bad);
		sched_expect(s.freepool.flink != &s.freepool,
		             "its node went on the free pool", 1, 1, &bad);
		core_sched_unload(&s);
		groups++;
	}

	/* ---- 6. _sleep yields and resumes with 1 in the accumulator ----- */
	{
		core_init(&cs, 0, 0);
		g_sched_live = 0;
		nat_run(&s, &cs, CORE_FN_SLEEP, 1, 250, 0, 1000);

		sched_expect(s.thread_count == 1, "_sleep leaves it queued",
		             s.thread_count, 1, &bad);
		sched_expect(g_arm_time == 1000 + 250 * 10000,
		             "armed 250ms later", (long)g_arm_time,
		             1000 + 250 * 10000, &bad);
		sched_expect(s.vm.vars[0] == 0, "nothing stored yet",
		             (long)s.vm.vars[0], 0, &bad);

		core_sched_run(&s, 1000 + 250 * 10000);
		sched_expect(s.vm.vars[0] == 1, "_sleep returns 1 when it expires",
		             (long)s.vm.vars[0], 1, &bad);
		sched_expect(s.thread_count == 0, "and the thread finished",
		             s.thread_count, 0, &bad);
		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	/* ---- 7. _wake cuts a sleep short and zeroes the accumulator ----- */
	{
		/*
		 * Thread 0 sleeps 250ms and stores what _sleep returned. A second
		 * thread, queued at pc `waker`, calls _wake(1). A sleep that runs
		 * its course leaves 1; one that is cut short leaves 0, and that is
		 * how a script tells the two apart.
		 */
		u32 code[48];
		s32 n = nat_prog(code, CORE_FN_SLEEP, 1, 250, 0);
		s32 waker = n;
		core_sched_thread *t;

		code[n++] = 0x192; code[n++] = 1;
		code[n++] = 0x093;
		code[n++] = 0x110; code[n++] = 1;      /* thread id 1 */
		code[n++] = 0x231;
		code[n++] = 0x181; code[n++] = CORE_FN_WAKE;
		code[n++] = 0x082;

		core_init(&cs, 0, 0);
		g_sched_live = 0;
		sched_test_open(&s);
		core_sched_set_core(&s, &cs);
		core_sched_set_native(&s, core_sched_native, &s);
		sched_reset_log();
		core_sched_load(&s, code, n, NAT_VARS, 1000);

		sched_expect(s.thread_count == 1, "the sleeper is queued",
		             s.thread_count, 1, &bad);

		t = core_sched_thread_alloc(&s, 0);
		t->pc        = waker;
		t->wake_time = 2000;
		core_sched_queue(&s, t);
		core_sched_run(&s, 2000);

		sched_expect(s.vm.vars[0] == 0,
		             "_sleep returns 0 when woken early",
		             (long)s.vm.vars[0], 0, &bad);
		sched_expect(s.thread_count == 0, "both threads finished",
		             s.thread_count, 0, &bad);
		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	/* ---- 8. _kill removes a thread and keeps the count honest ------- */
	{
		u32 code[48];
		s32 n = nat_prog(code, CORE_FN_SLEEP, 1, 5000, 0);
		s32 killer = n;
		core_sched_thread *t;

		code[n++] = 0x192; code[n++] = 1;
		code[n++] = 0x093;
		code[n++] = 0x110; code[n++] = 1;
		code[n++] = 0x231;
		code[n++] = 0x181; code[n++] = CORE_FN_KILL;
		code[n++] = 0x082;

		core_init(&cs, 0, 0);
		g_sched_live = 0;
		sched_test_open(&s);
		core_sched_set_core(&s, &cs);
		core_sched_set_native(&s, core_sched_native, &s);
		sched_reset_log();
		core_sched_load(&s, code, n, NAT_VARS, 1000);
		sched_expect(s.thread_count == 1, "the victim is asleep",
		             s.thread_count, 1, &bad);

		t = core_sched_thread_alloc(&s, 0);
		t->pc        = killer;
		t->wake_time = 2000;
		core_sched_queue(&s, t);
		core_sched_run(&s, 2000);

		sched_expect(s.thread_count == 0,
		             "_kill decremented the count too", s.thread_count, 0,
		             &bad);
		sched_expect(s.vm.vars[0] == 0, "the victim never resumed",
		             (long)s.vm.vars[0], 0, &bad);
		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	/* ---- 9. _fork: parent sees the child id, child sees zero -------- */
	{
		/*
		 * Both halves resume at the instruction after the call, so the
		 * only thing that tells them apart is the accumulator. The parent
		 * stores the child id in global 0; the child takes the zero branch
		 * and stores a marker in global 1, because storing the zero it
		 * received would be indistinguishable from never having run.
		 */
		static const u32 FORK[] = {
			0x181, CORE_FN_FORK,          /*  0 acc = child id, or 0     */
			0x093,                        /*  2 push it                  */
			0x172, 6,                     /*  3 if zero, go to 11        */
			0x110, CORE_TAG_GLOBAL | 0u,  /*  5 parent: acc = &global0   */
			0x230,                        /*  7 global0 = pop() = the id */
			0x082,                        /*  8 ret                      */
			0x082, 0x082,                 /*  9 never reached            */
			0x294,                        /* 11 child: discard the zero  */
			0x110, CORE_TAG_GLOBAL | 1u,  /* 12 acc = &global1           */
			0x093,                        /* 14 push                     */
			0x110, 77,                    /* 15 acc = 77                 */
			0x231,                        /* 17 global1 = 77             */
			0x082                         /* 18 ret                      */
		};

		core_init(&cs, 0, 0);
		g_sched_live = 0;
		sched_test_open(&s);
		core_sched_set_core(&s, &cs);
		core_sched_set_native(&s, core_sched_native, &s);
		sched_reset_log();
		core_sched_load(&s, FORK, (s32)(sizeof(FORK) / 4), NAT_VARS, 1000);

		sched_expect(s.vm.vars[0] == 2, "parent got the child id",
		             (long)s.vm.vars[0], 2, &bad);
		sched_expect(s.vm.vars[1] == 77, "child took the zero branch",
		             (long)s.vm.vars[1], 77, &bad);
		sched_expect(s.thread_count == 0, "both threads finished",
		             s.thread_count, 0, &bad);
		sched_expect(s.fault_thread == 0, "neither faulted",
		             s.fault_thread != 0, 0, &bad);
		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	/* ---- 10. _fork refuses past the thread ceiling ------------------ */
	{
		/*
		 * Word 0 is a bare ret so thread 0 retires immediately; the fork
		 * program sits at word 1 and is reached by a thread queued after
		 * the ready list has been filled to the ceiling.
		 */
		static const u32 CAP[] = {
			0x082,                        /* 0 thread 0: ret             */
			0x110, CORE_TAG_GLOBAL | 0u,  /* 1 acc = &global0            */
			0x093,                        /* 3 push                      */
			0x181, CORE_FN_FORK,          /* 4 acc = id, or -1           */
			0x231,                        /* 6 global0 = acc             */
			0x082                         /* 7 ret                       */
		};
		core_sched_thread *t;
		int k;

		core_init(&cs, 0, 0);
		g_sched_live = 0;
		sched_test_open(&s);
		core_sched_set_core(&s, &cs);
		core_sched_set_native(&s, core_sched_native, &s);
		sched_reset_log();
		core_sched_load(&s, CAP, (s32)(sizeof(CAP) / 4), NAT_VARS, 1000);
		sched_expect(s.thread_count == 0, "thread 0 retired",
		             s.thread_count, 0, &bad);

		for (k = 0; k < CORE_SCHED_MAX_THREADS; k++) {
			t = core_sched_thread_alloc(&s, 0);
			t->pc        = 0;
			t->wake_time = 999999;      /* parked, never due here */
			core_sched_queue(&s, t);
		}
		t = core_sched_thread_alloc(&s, 0);
		t->pc        = 1;
		t->wake_time = 2000;
		core_sched_queue(&s, t);
		core_sched_run(&s, 2000);

		sched_expect((s32)s.vm.vars[0] == -1,
		             "_fork refuses at the ceiling",
		             (long)(s32)s.vm.vars[0], -1, &bad);
		sched_expect(s.thread_count == CORE_SCHED_MAX_THREADS,
		             "and queued nothing", s.thread_count,
		             CORE_SCHED_MAX_THREADS, &bad);
		core_sched_unload(&s);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	hlog("Native builtins        : %s (%d groups)\n", bad ? "FAIL" : "ok",
	     groups);
	return bad;
}

/* ------------------------------------------------------------------ */
/* the raw N64 controller-bus transaction                              */
/* ------------------------------------------------------------------ */

#define N64LOG 8

static struct {
	core_vendor_req req;
	u32 len;
	u8  out[72];        /* what an OUT transfer carried */
} g_n64log[N64LOG];
static int g_n64count;

/* What the fake device answers with, and how much of it. */
static u8  g_n64reply[72];
static u32 g_n64reply_len;
static int g_n64fail_on;      /* 1-based transfer to refuse; 0 = never */

static int n64_sync(void *ctx, const core_vendor_req *req, u8 *data, u32 len)
{
	int k = g_n64count;
	u32 i;

	(void)ctx;
	if (k < N64LOG) {
		g_n64log[k].req = *req;
		g_n64log[k].len = len;
		for (i = 0; i < len && i < sizeof(g_n64log[k].out); i++) {
			g_n64log[k].out[i] = (req->bmRequestType == CORE_VENDOR_OUT
			                      && data != 0) ? data[i] : 0;
		}
	}
	g_n64count++;
	if (g_n64fail_on == g_n64count) {
		return 0;
	}
	if (req->bmRequestType == CORE_VENDOR_IN && data != 0) {
		for (i = 0; i < len; i++) {
			data[i] = (i < g_n64reply_len) ? g_n64reply[i] : 0;
		}
	}
	return 1;
}

static void n64_reset_log(void)
{
	g_n64count     = 0;
	g_n64fail_on   = 0;
	g_n64reply_len = 0;
}

static void n64_set_reply(const u8 *b, u32 n)
{
	u32 i;

	for (i = 0; i < n && i < sizeof(g_n64reply); i++) {
		g_n64reply[i] = b[i];
	}
	g_n64reply_len = n;
}

static int test_n64_transaction(void)
{
	int bad    = 0;
	int groups = 0;
	core_state cs;
	u8  rx[80];
	s32 actual;

	/* ---- 1. the short form: request info ---------------------------- */
	{
		/* command 0x00, three bytes back. The device answers
		 * length-then-payload, and the payload arrives reversed. */
		static const u8 TX[]    = {CORE_N64_CMD_INFO};
		static const u8 REPLY[] = {0x03, 0x02, 0x00, 0x05};

		core_init(&cs, 0, 0);
		core_set_vendor_sync(&cs, n64_sync);
		n64_reset_log();
		n64_set_reply(REPLY, 4);

		sched_expect(core_n64_transaction(&cs, TX, 1, rx, 3, &actual) == 1,
		             "info transaction ran", 1, 1, &bad);
		sched_expect(g_n64count == 1, "one transfer", g_n64count, 1, &bad);
		sched_expect(g_n64log[0].req.bmRequestType == CORE_VENDOR_IN,
		             "vendor IN", g_n64log[0].req.bmRequestType,
		             CORE_VENDOR_IN, &bad);
		sched_expect(g_n64log[0].req.bRequest == 0x21,
		             "bRequest 0x20 + tx_len", g_n64log[0].req.bRequest,
		             0x21, &bad);
		sched_expect(g_n64log[0].req.wValue == 0x0000,
		             "wValue is the command, zero-filled",
		             g_n64log[0].req.wValue, 0, &bad);
		sched_expect(g_n64log[0].req.wIndex == 0x0000, "wIndex zero-filled",
		             g_n64log[0].req.wIndex, 0, &bad);
		sched_expect(g_n64log[0].req.wLength == 4, "asks for rx_len + 1",
		             g_n64log[0].req.wLength, 4, &bad);
		sched_expect(actual == 3, "three bytes back", actual, 3, &bad);
		/* payload 02 00 05 reversed -> 05 00 02 */
		sched_expect(rx[0] == 0x05 && rx[1] == 0x00 && rx[2] == 0x02,
		             "reply is byte-reversed", rx[0], 0x05, &bad);
		groups++;
	}

	/* ---- 2. all four short-form lengths pack the setup packet ------- */
	{
		static const u8 TX[] = {0xA1, 0xB2, 0xC3, 0xD4};
		static const u8 REPLY[] = {0x01, 0x77};
		static const struct {
			s32 tx_len;
			u8  request;
			u16 value, index;
		} W[] = {
		  {1, 0x21, 0x00A1, 0x0000},
		  {2, 0x22, 0xB2A1, 0x0000},
		  {3, 0x23, 0xB2A1, 0x00C3},
		  {4, 0x24, 0xB2A1, 0xD4C3}
		};
		int k;

		for (k = 0; k < 4; k++) {
			core_init(&cs, 0, 0);
			core_set_vendor_sync(&cs, n64_sync);
			n64_reset_log();
			n64_set_reply(REPLY, 2);
			core_n64_transaction(&cs, TX, W[k].tx_len, rx, 1, &actual);

			if (g_n64log[0].req.bRequest != W[k].request ||
			    g_n64log[0].req.wValue != W[k].value ||
			    g_n64log[0].req.wIndex != W[k].index) {
				hlog("  FAIL n64 tx_len %d -> %02x %04x %04x, "
				     "want %02x %04x %04x\n", W[k].tx_len,
				     g_n64log[0].req.bRequest, g_n64log[0].req.wValue,
				     g_n64log[0].req.wIndex, W[k].request, W[k].value,
				     W[k].index);
				bad++;
			}
			htrace("n64 short tx_len %d -> %02x %04x %04x\n", W[k].tx_len,
			       g_n64log[0].req.bRequest, g_n64log[0].req.wValue,
			       g_n64log[0].req.wIndex);
		}
		groups++;
	}

	/* ---- 3. a zero first byte means the device did not answer ------- */
	{
		static const u8 TX[]    = {CORE_N64_CMD_INFO};
		static const u8 REPLY[] = {0x00, 0xAA, 0xBB, 0xCC};

		core_init(&cs, 0, 0);
		core_set_vendor_sync(&cs, n64_sync);
		n64_reset_log();
		n64_set_reply(REPLY, 4);
		rx[0] = 0xEE;

		sched_expect(core_n64_transaction(&cs, TX, 1, rx, 3, &actual) == 1,
		             "it still ran", 1, 1, &bad);
		sched_expect(actual == 0, "but reported nothing back", actual, 0,
		             &bad);
		sched_expect(rx[0] == 0xEE, "and did not touch the buffer", rx[0],
		             0xEE, &bad);
		groups++;
	}

	/* ---- 4. the long form: write accessory -------------------------- */
	{
		/* 35 bytes out, one back. tx[0..2] ride in the setup packet and
		 * the remaining 32 are the data stage. */
		u8 tx[35];
		static const u8 REPLY[] = {0x81, 0x5A};
		int k;

		/* tx[0] is deliberately NOT equal to rx_len, so that swapping
		 * the two halves of wValue is detectable. */
		for (k = 0; k < 35; k++) {
			tx[k] = (u8)(k + 3);
		}
		core_init(&cs, 0, 0);
		core_set_vendor_sync(&cs, n64_sync);
		n64_reset_log();
		n64_set_reply(REPLY, 2);

		sched_expect(core_n64_transaction(&cs, tx, 35, rx, 1, &actual) == 1,
		             "write transaction ran", 1, 1, &bad);
		sched_expect(g_n64count == 2, "two transfers", g_n64count, 2, &bad);

		sched_expect(g_n64log[0].req.bmRequestType == CORE_VENDOR_OUT,
		             "first is vendor OUT", g_n64log[0].req.bmRequestType,
		             CORE_VENDOR_OUT, &bad);
		sched_expect(g_n64log[0].req.bRequest == 0x20, "bRequest 0x20",
		             g_n64log[0].req.bRequest, 0x20, &bad);
		sched_expect(g_n64log[0].req.wValue == 0x0301,
		             "wValue is rx_len then tx[0]", g_n64log[0].req.wValue,
		             0x0301, &bad);
		sched_expect(g_n64log[0].req.wIndex == 0x0504,
		             "wIndex is tx[1], tx[2]", g_n64log[0].req.wIndex,
		             0x0504, &bad);
		sched_expect(g_n64log[0].len == 32, "32 bytes of data stage",
		             (long)g_n64log[0].len, 32, &bad);
		sched_expect(g_n64log[0].out[0] == 6,
		             "the data stage starts at tx[3]", g_n64log[0].out[0],
		             6, &bad);

		sched_expect(g_n64log[1].req.bmRequestType == CORE_VENDOR_IN,
		             "second is vendor IN", g_n64log[1].req.bmRequestType,
		             CORE_VENDOR_IN, &bad);
		sched_expect(g_n64log[1].req.bRequest == 0x71, "bRequest 0x71",
		             g_n64log[1].req.bRequest, 0x71, &bad);
		sched_expect(g_n64log[1].req.wValue == 0x0030, "wValue 0x0030",
		             g_n64log[1].req.wValue, 0x0030, &bad);
		sched_expect(actual == 1, "one byte back", actual, 1, &bad);
		sched_expect(rx[0] == 0x5A, "the CRC byte", rx[0], 0x5A, &bad);
		groups++;
	}

	/* ---- 5. the long form validates its status byte ----------------- */
	{
		u8 tx[35];
		int k;
		static const struct {
			u8 status;
			s32 want;
			const char *what;
		} ST[] = {
		  {0x81, 1, "bit 7 set, count matches"},
		  {0x01, 0, "bit 7 clear"},
		  {0x82, 0, "count disagrees"},
		  {0xC1, 1, "spare bits ignored"}
		};

		for (k = 0; k < 35; k++) {
			tx[k] = (u8)k;
		}
		for (k = 0; k < 4; k++) {
			u8 reply[2];

			reply[0] = ST[k].status;
			reply[1] = 0x99;
			core_init(&cs, 0, 0);
			core_set_vendor_sync(&cs, n64_sync);
			n64_reset_log();
			n64_set_reply(reply, 2);
			core_n64_transaction(&cs, tx, 35, rx, 1, &actual);
			sched_expect(actual == ST[k].want, ST[k].what, actual,
			             ST[k].want, &bad);
		}
		groups++;
	}

	/* ---- 6. the guard, and the bound the original does not have ----- */
	{
		static const u8 TX[] = {1, 2, 3, 4, 5, 6};

		core_init(&cs, 0, 0);
		core_set_vendor_sync(&cs, n64_sync);

		/* tx_len >= 5 AND rx_len >= 4 is refused outright */
		n64_reset_log();
		actual = 0x5555;
		sched_expect(core_n64_transaction(&cs, TX, 6, rx, 4, &actual) == 0,
		             "long form with a big reply is refused", 0, 0, &bad);
		sched_expect(g_n64count == 0, "and sends nothing", g_n64count, 0,
		             &bad);
		sched_expect(actual == 0, "actual is zeroed even when refused",
		             actual, 0, &bad);

		/* the receive bound this port adds */
		n64_reset_log();
		sched_expect(core_n64_transaction(&cs, TX, 1, rx,
		                                  CORE_N64_RX_MAX, &actual) == 1,
		             "63 bytes is allowed", 1, 1, &bad);
		n64_reset_log();
		sched_expect(core_n64_transaction(&cs, TX, 1, rx,
		                                  CORE_N64_RX_MAX + 1, &actual) == 0,
		             "64 is refused", 0, 0, &bad);
		sched_expect(g_n64count == 0, "with no transfer attempted",
		             g_n64count, 0, &bad);
		n64_reset_log();
		sched_expect(core_n64_transaction(&cs, TX, 1, rx, 4096, &actual) == 0,
		             "and so is 4096", 0, 0, &bad);
		groups++;
	}

	/* ---- 7. a failed transfer is reported, not ignored -------------- */
	{
		static const u8 TX[] = {CORE_N64_CMD_INFO};
		u8 tx35[35];
		int k;

		for (k = 0; k < 35; k++) {
			tx35[k] = (u8)k;
		}
		core_init(&cs, 0, 0);
		core_set_vendor_sync(&cs, n64_sync);

		n64_reset_log();
		g_n64fail_on = 1;
		actual = 0x5555;
		sched_expect(core_n64_transaction(&cs, TX, 1, rx, 3, &actual) == 1,
		             "short form, transfer refused", 1, 1, &bad);
		sched_expect(actual == 0, "reports nothing back", actual, 0, &bad);

		n64_reset_log();
		g_n64fail_on = 2;       /* the OUT succeeds, the fetch does not */
		actual = 0x5555;
		core_n64_transaction(&cs, tx35, 35, rx, 1, &actual);
		sched_expect(actual == 0, "long form, fetch refused", actual, 0,
		             &bad);
		groups++;
	}

	/* ---- 8. the controller reset --------------------------------- */
	{
		core_init(&cs, 0, 0);
		core_set_vendor(&cs, harness_vendor, 0);
		bus_reset(0);

		sched_expect(core_controller_reset(&cs) == 1, "reset was issued", 1,
		             1, &bad);
		sched_expect(g_bus_count == 1, "one transfer", g_bus_count, 1,
		             &bad);
		sched_expect(g_bus_log[0].bmRequestType == CORE_VENDOR_OUT,
		             "vendor OUT", g_bus_log[0].bmRequestType,
		             CORE_VENDOR_OUT, &bad);
		sched_expect(g_bus_log[0].bRequest == 0x72, "bRequest 0x72",
		             g_bus_log[0].bRequest, 0x72, &bad);
		sched_expect(g_bus_log[0].wValue == 0x0F20, "wValue 0x0F20",
		             g_bus_log[0].wValue, 0x0F20, &bad);
		sched_expect(g_bus_log[0].wIndex == 0, "wIndex 0",
		             g_bus_log[0].wIndex, 0, &bad);
		groups++;
	}

	hlog("N64 transaction        : %s (%d groups)\n", bad ? "FAIL" : "ok",
	     groups);
	return bad;
}

/* ------------------------------------------------------------------ */
/* the keyboard and mouse report state machines                        */
/* ------------------------------------------------------------------ */

#define HIDLOG 64

static struct {
	u8  id;
	u8  data[CORE_REPORT_MAX_BYTES];
	u32 len;
} g_hidlog[HIDLOG];
static int g_hidcount;

static void hid_sink(void *ctx, u8 report_id, const u8 *data, u32 len)
{
	u32 i;

	(void)ctx;
	if (g_hidcount < HIDLOG) {
		g_hidlog[g_hidcount].id  = report_id;
		g_hidlog[g_hidcount].len = len;
		for (i = 0; i < len && i < CORE_REPORT_MAX_BYTES; i++) {
			g_hidlog[g_hidcount].data[i] = data[i];
		}
	}
	g_hidcount++;
}

/* The keycodes in the last keyboard report, as a printable digest. */
static int hid_keys_match(int at, const u8 *want, int n)
{
	int i;

	for (i = 0; i < CORE_HID_KEYS_REPORTED; i++) {
		u8 got = g_hidlog[at].data[2 + i];
		u8 exp = (i < n) ? want[i] : 0;

		if (got != exp) {
			return 0;
		}
	}
	return 1;
}

static int test_hid_reports(void)
{
	int bad    = 0;
	int groups = 0;
	core_state cs;

	/* ---- 1. modifiers are a bitmask, and only changes report ------- */
	{
		core_init(&cs, hid_sink, 0);
		g_hidcount = 0;

		core_hid_key_event(&cs, 0xE0, 1);          /* LeftControl */
		sched_expect(g_hidcount == 1, "press emits one report", g_hidcount,
		             1, &bad);
		sched_expect(g_hidlog[0].id == CORE_REPORT_KEYBOARD,
		             "report ID 2", g_hidlog[0].id, CORE_REPORT_KEYBOARD,
		             &bad);
		sched_expect(g_hidlog[0].len == 12, "twelve payload bytes",
		             (long)g_hidlog[0].len, 12, &bad);
		sched_expect(g_hidlog[0].data[0] == 0x01, "bit 0 for 0xE0",
		             g_hidlog[0].data[0], 0x01, &bad);
		sched_expect(g_hidlog[0].data[1] == 0, "reserved byte is zero",
		             g_hidlog[0].data[1], 0, &bad);

		core_hid_key_event(&cs, 0xE0, 1);          /* held */
		sched_expect(g_hidcount == 1, "a held modifier does not repeat",
		             g_hidcount, 1, &bad);

		core_hid_key_event(&cs, 0xE7, 1);          /* RightGUI */
		sched_expect(g_hidlog[1].data[0] == 0x81, "bit 7 for 0xE7",
		             g_hidlog[1].data[0], 0x81, &bad);

		core_hid_key_event(&cs, 0xE0, 0);
		sched_expect(g_hidlog[2].data[0] == 0x80, "release clears its bit",
		             g_hidlog[2].data[0], 0x80, &bad);

		core_hid_key_event(&cs, 0xE0, 0);          /* already up */
		sched_expect(g_hidcount == 3, "releasing an unheld key is silent",
		             g_hidcount, 3, &bad);
		groups++;
	}

	/* ---- 2. ordinary keys stay dense and in press order ------------ */
	{
		/*
		 * FOUR keys, not three. Removing the middle of three gives the
		 * same answer whether the tail is shifted down or the last entry
		 * is swapped into the hole; with four, only shifting preserves
		 * press order.
		 */
		static const u8 WANT4[] = {0x04, 0x05, 0x06, 0x07};
		static const u8 WANT3[] = {0x04, 0x06, 0x07};

		core_init(&cs, hid_sink, 0);
		g_hidcount = 0;

		core_hid_key_event(&cs, 0x04, 1);
		core_hid_key_event(&cs, 0x05, 1);
		core_hid_key_event(&cs, 0x06, 1);
		core_hid_key_event(&cs, 0x07, 1);
		sched_expect(g_hidcount == 4, "four presses, four reports",
		             g_hidcount, 4, &bad);
		sched_expect(hid_keys_match(3, WANT4, 4), "in press order",
		             g_hidlog[3].data[2], 0x04, &bad);

		core_hid_key_event(&cs, 0x05, 0);          /* the second one */
		sched_expect(hid_keys_match(4, WANT3, 3),
		             "release closes the gap, keeping order",
		             g_hidlog[4].data[3], 0x06, &bad);
		sched_expect(g_hidlog[4].data[5] == 0, "and zero-pads the tail",
		             g_hidlog[4].data[5], 0, &bad);

		core_hid_key_event(&cs, 0x04, 1);          /* already down */
		sched_expect(g_hidcount == 5, "a held key does not repeat",
		             g_hidcount, 5, &bad);
		groups++;
	}

	/* ---- 3. ten-key rollover, and recovery from it ------------------ */
	{
		int k;
		int roll_ok = 1;

		core_init(&cs, hid_sink, 0);
		g_hidcount = 0;

		for (k = 0; k < 10; k++) {
			core_hid_key_event(&cs, (u32)(0x04 + k), 1);
		}
		sched_expect(g_hidlog[9].data[2 + 9] == 0x0D,
		             "ten keys all fit", g_hidlog[9].data[11], 0x0D, &bad);

		core_hid_key_event(&cs, 0x0E, 1);          /* the eleventh */
		for (k = 0; k < CORE_HID_KEYS_REPORTED; k++) {
			if (g_hidlog[10].data[2 + k] != CORE_HID_ROLLOVER) {
				roll_ok = 0;
			}
		}
		sched_expect(roll_ok, "the eleventh fills all ten with 0x01",
		             g_hidlog[10].data[2], CORE_HID_ROLLOVER, &bad);

		/* releasing back to ten restores the real list */
		core_hid_key_event(&cs, 0x0E, 0);
		sched_expect(g_hidlog[11].data[2] == 0x04,
		             "dropping back to ten restores them",
		             g_hidlog[11].data[2], 0x04, &bad);
		sched_expect(g_hidlog[11].data[11] == 0x0D, "all ten of them",
		             g_hidlog[11].data[11], 0x0D, &bad);
		groups++;
	}

	/* ---- 4. usage 0 releases everything ----------------------------- */
	{
		core_init(&cs, hid_sink, 0);
		g_hidcount = 0;

		core_hid_key_event(&cs, 0xE1, 1);
		core_hid_key_event(&cs, 0x07, 1);
		g_hidcount = 0;

		core_hid_key_event(&cs, 0, 0);
		sched_expect(g_hidcount == 1, "one report", g_hidcount, 1, &bad);
		sched_expect(g_hidlog[0].data[0] == 0, "modifiers cleared",
		             g_hidlog[0].data[0], 0, &bad);
		sched_expect(g_hidlog[0].data[2] == 0, "and the key list",
		             g_hidlog[0].data[2], 0, &bad);

		core_hid_key_event(&cs, 0, 0);
		sched_expect(g_hidcount == 1, "clearing nothing is silent",
		             g_hidcount, 1, &bad);
		groups++;
	}

	/* ---- 5. keycodes outside the accepted ranges are ignored -------- */
	{
		/* NOT named OUT: the DDK headers define that as an annotation
		 * macro, and kstub.h mirrors them. */
		static const u32 REJECT[] = {0x01, 0x02, 0x03, 0xA5,
			                         0xDF, 0xE8, 0xFF};
		int k;

		core_init(&cs, hid_sink, 0);
		g_hidcount = 0;
		for (k = 0; k < (int)(sizeof(REJECT) / sizeof(REJECT[0])); k++) {
			core_hid_key_event(&cs, REJECT[k], 1);
		}
		sched_expect(g_hidcount == 0, "seven out-of-range codes ignored",
		             g_hidcount, 0, &bad);

		/* the boundaries themselves ARE accepted */
		core_hid_key_event(&cs, 0x04, 1);
		core_hid_key_event(&cs, 0xA4, 1);
		core_hid_key_event(&cs, 0xE0, 1);
		core_hid_key_event(&cs, 0xE7, 1);
		sched_expect(g_hidcount == 4, "all four boundaries accepted",
		             g_hidcount, 4, &bad);
		groups++;
	}

	/* ---- 6. mouse buttons ------------------------------------------- */
	{
		core_init(&cs, hid_sink, 0);
		g_hidcount = 0;

		core_hid_mouse_button(&cs, 1, 1);
		sched_expect(g_hidlog[0].id == CORE_REPORT_MOUSE, "report ID 3",
		             g_hidlog[0].id, CORE_REPORT_MOUSE, &bad);
		sched_expect(g_hidlog[0].len == 4, "four payload bytes",
		             (long)g_hidlog[0].len, 4, &bad);
		sched_expect(g_hidlog[0].data[0] == 0x01, "button 1 is bit 0",
		             g_hidlog[0].data[0], 0x01, &bad);
		sched_expect(g_hidlog[0].data[1] == 0 && g_hidlog[0].data[2] == 0 &&
		             g_hidlog[0].data[3] == 0, "with no movement",
		             g_hidlog[0].data[1], 0, &bad);

		core_hid_mouse_button(&cs, 3, 1);
		sched_expect(g_hidlog[1].data[0] == 0x05, "button 3 is bit 2",
		             g_hidlog[1].data[0], 0x05, &bad);

		core_hid_mouse_button(&cs, 1, 1);
		sched_expect(g_hidcount == 2, "a held button does not repeat",
		             g_hidcount, 2, &bad);

		core_hid_mouse_button(&cs, 4, 1);
		core_hid_mouse_button(&cs, 0, 1);
		sched_expect(g_hidcount == 2, "buttons 0 and 4 are ignored",
		             g_hidcount, 2, &bad);

		core_hid_mouse_button(&cs, 3, 0);
		sched_expect(g_hidlog[2].data[0] == 0x01, "release clears its bit",
		             g_hidlog[2].data[0], 0x01, &bad);
		groups++;
	}

	/* ---- 7. mouse movement carries deltas and accumulates totals ---- */
	{
		core_init(&cs, hid_sink, 0);
		g_hidcount = 0;
		core_hid_mouse_button(&cs, 2, 1);
		g_hidcount = 0;

		core_hid_mouse_move(&cs, 10, -20, 0);
		sched_expect(g_hidlog[0].data[0] == 0x02,
		             "movement carries the held button",
		             g_hidlog[0].data[0], 0x02, &bad);
		sched_expect((s8)g_hidlog[0].data[1] == 10, "dx",
		             (s8)g_hidlog[0].data[1], 10, &bad);
		sched_expect((s8)g_hidlog[0].data[2] == -20, "dy",
		             (s8)g_hidlog[0].data[2], -20, &bad);

		core_hid_mouse_move(&cs, 5, 5, 0);
		sched_expect(cs.mouse_total_x == 15, "totals accumulate",
		             cs.mouse_total_x, 15, &bad);
		sched_expect(cs.mouse_total_y == -15, "on both axes",
		             cs.mouse_total_y, -15, &bad);
		sched_expect((s8)g_hidlog[1].data[1] == 5,
		             "but the report carries the delta, not the total",
		             (s8)g_hidlog[1].data[1], 5, &bad);

		core_hid_mouse_move(&cs, 0, 0, 0);
		sched_expect(g_hidcount == 3, "a zero move still reports",
		             g_hidcount, 3, &bad);
		groups++;
	}

	/* ---- 8. the joystick submit, replay and virtual mode ------------ */
	{
		static const u8 R1[] = {0x11, 0x22, 0x33, 0x44, 0x55};
		int k;
		int same = 1;

		core_init(&cs, hid_sink, 0);
		g_hidcount = 0;

		core_submit_joystick(&cs, R1);
		sched_expect(g_hidlog[0].id == CORE_REPORT_JOYSTICK, "report ID 1",
		             g_hidlog[0].id, CORE_REPORT_JOYSTICK, &bad);
		sched_expect(g_hidlog[0].len == CORE_JOY_REPORT_BYTES,
		             "five payload bytes", (long)g_hidlog[0].len,
		             CORE_JOY_REPORT_BYTES, &bad);
		for (k = 0; k < CORE_JOY_REPORT_BYTES; k++) {
			if (g_hidlog[0].data[k] != R1[k]) {
				same = 0;
			}
		}
		sched_expect(same, "passed through unchanged", g_hidlog[0].data[0],
		             0x11, &bad);

		/* NULL replays the last one */
		core_submit_joystick(&cs, 0);
		same = 1;
		for (k = 0; k < CORE_JOY_REPORT_BYTES; k++) {
			if (g_hidlog[1].data[k] != R1[k]) {
				same = 0;
			}
		}
		sched_expect(same, "NULL replays the last report",
		             g_hidlog[1].data[0], 0x11, &bad);

		/* turning the switch on does not take effect until a NULL submit */
		cs.reports_enabled = 1;
		cs.virtual_stick   = 0x123;
		core_submit_joystick(&cs, R1);
		sched_expect(g_hidlog[2].data[0] == 0x11,
		             "the switch is not read on a real submit",
		             g_hidlog[2].data[0], 0x11, &bad);

		core_submit_joystick(&cs, 0);
		sched_expect(cs.virtual_mode == 3, "a NULL submit selects it",
		             cs.virtual_mode, 3, &bad);
		sched_expect(g_hidlog[3].data[0] == 1, "X low byte is 1",
		             g_hidlog[3].data[0], 1, &bad);
		sched_expect(g_hidlog[3].data[1] == 0x30,
		             "Y low nibble in the high half",
		             g_hidlog[3].data[1], 0x30, &bad);
		sched_expect(g_hidlog[3].data[2] == 0x12, "Y high bits",
		             g_hidlog[3].data[2], 0x12, &bad);
		sched_expect(g_hidlog[3].data[3] == 0 && g_hidlog[3].data[4] == 0,
		             "and no buttons", g_hidlog[3].data[3], 0, &bad);

		/* in virtual mode a real report is dropped entirely */
		g_hidcount = 0;
		core_submit_joystick(&cs, R1);
		sched_expect(g_hidcount == 0, "virtual mode drops real reports",
		             g_hidcount, 0, &bad);
		groups++;
	}

	/* ---- 9. the device mask gates each report independently -------- */
	{
		core_init(&cs, hid_sink, 0);
		cs.devices_mask = CORE_DEVICE_MOUSE;      /* keyboard off */
		g_hidcount = 0;

		core_hid_key_event(&cs, 0x04, 1);
		sched_expect(g_hidcount == 0, "keyboard report suppressed",
		             g_hidcount, 0, &bad);
		core_hid_mouse_button(&cs, 1, 1);
		sched_expect(g_hidcount == 1, "mouse report still goes out",
		             g_hidcount, 1, &bad);
		groups++;
	}

	hlog("HID report builders    : %s (%d groups)\n", bad ? "FAIL" : "ok",
	     groups);
	return bad;
}

/* ------------------------------------------------------------------ */
/* the private IOCTL surface                                           */
/* ------------------------------------------------------------------ */

/* Little-endian dword access, so the tests read the buffers the way the
 * configurator lays them out rather than by struct punning. */
static void wr32_test(u8 *p, u32 v)
{
	p[0] = (u8)(v & 0xFFu);
	p[1] = (u8)((v >> 8) & 0xFFu);
	p[2] = (u8)((v >> 16) & 0xFFu);
	p[3] = (u8)((v >> 24) & 0xFFu);
}

static u32 rd32_test(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

static int g_ioc_enable_calls;
static int g_ioc_enable_last;
static u32 g_ioc_vendor_ret;
static int g_ioc_vendor_calls;
static u32 g_ioc_vendor_len;

static void ioc_enable_stub(void *ctx, int on)
{
	(void)ctx;
	g_ioc_enable_calls++;
	g_ioc_enable_last = on;
}

static u32 ioc_vendor_stub(void *ctx, const u8 *setup, u8 *data, u32 len)
{
	(void)ctx;
	(void)setup;
	(void)data;
	g_ioc_vendor_calls++;
	g_ioc_vendor_len = len;
	return g_ioc_vendor_ret;
}

/* One request, with the buffers the harness owns. */
static u8 g_ioc_in[256];
static u8 g_ioc_out[256];

static u32 ioc_call(core_ioctl_env *env, u32 fn, u32 in_len, u32 out_len,
                    u32 *info)
{
	core_ioctl r;

	r.code    = CORE_IOCTL_CODE(fn);
	r.in      = g_ioc_in;
	r.in_len  = in_len;
	r.out     = g_ioc_out;
	r.out_len = out_len;
	return core_ioctl_dispatch(env, &r, info);
}

static int test_ioctl(void)
{
	int bad    = 0;
	int groups = 0;
	core_state cs;
	core_sched sch;
	core_ioctl_env env;
	u32 st, info;
	int i;

	/* Every group starts from a clean device. */
#define IOC_OPEN()                                                        \
	do {                                                                  \
		core_init(&cs, 0, 0);                                             \
		core_set_vendor_sync(&cs, n64_sync);                              \
		core_sched_init(&sch, sched_test_alloc, sched_test_free, 0);      \
		env.cs         = &cs;                                             \
		env.sched      = &sch;                                            \
		env.vendor     = ioc_vendor_stub;                                 \
		env.vendor_ctx = 0;                                               \
		env.enable     = ioc_enable_stub;                                 \
		env.enable_ctx = 0;                                               \
		env.now_100ns  = 0;                                               \
		n64_reset_log();                                                  \
		for (i = 0; i < 256; i++) { g_ioc_in[i] = 0; g_ioc_out[i] = 0; }  \
	} while (0)

	/* ---- 1. unknown codes versus wrong lengths --------------------- */
	{
		IOC_OPEN();

		/* function 0x831 has an index-table entry but shares the default
		 * target, so it is NOT handled */
		sched_expect(ioc_call(&env, 0x831, 0, 0, &info) ==
		             CORE_ST_NOT_SUPPORTED, "fn 0x831 is not handled",
		             1, 1, &bad);
		sched_expect(ioc_call(&env, 0x838, 0, 0, &info) ==
		             CORE_ST_NOT_SUPPORTED, "fn 0x838 gap", 1, 1, &bad);
		sched_expect(ioc_call(&env, 0x83B, 0, 0, &info) ==
		             CORE_ST_NOT_SUPPORTED, "fn 0x83b gap", 1, 1, &bad);
		sched_expect(ioc_call(&env, 0x999, 0, 0, &info) ==
		             CORE_ST_NOT_SUPPORTED, "an unrelated code", 1, 1,
		             &bad);
		/* a HANDLED code with wrong lengths is a different answer */
		sched_expect(ioc_call(&env, CORE_IOC_STATUS_SNAP, 0, 5, &info) ==
		             CORE_ST_INVALID_PARAM, "wrong length is not the same",
		             1, 1, &bad);
		/* and a foreign device type is not ours at all */
		{
			core_ioctl r;

			r.code = 0x12340000u | (CORE_IOC_STATUS_SNAP << 2);
			r.in = g_ioc_in; r.in_len = 0;
			r.out = g_ioc_out; r.out_len = 6;
			sched_expect(core_ioctl_dispatch(&env, &r, &info) ==
			             CORE_ST_NOT_SUPPORTED, "foreign device type",
			             1, 1, &bad);
		}
		core_sched_unload(&sch);
		groups++;
	}

	/* ---- 2. the status snapshot clears its flag -------------------- */
	{
		static const u8 RAW[] = {0x11, 0x22, CORE_STATUS_VALID, 0x44, 0x55};

		IOC_OPEN();
		cs.accessory_state = CORE_ACC_FOUND_1;
		core_on_raw_packet(&cs, RAW);

		st = ioc_call(&env, CORE_IOC_STATUS_SNAP, 0, 6, &info);
		sched_expect(st == CORE_ST_SUCCESS, "snapshot succeeds", (long)st,
		             0, &bad);
		sched_expect(info == 6, "six bytes", (long)info, 6, &bad);
		sched_expect(g_ioc_out[0] == 1, "pending flag was set",
		             g_ioc_out[0], 1, &bad);
		sched_expect(g_ioc_out[1] == 0x11 && g_ioc_out[5] == 0x55,
		             "carrying the raw packet", g_ioc_out[1], 0x11, &bad);

		ioc_call(&env, CORE_IOC_STATUS_SNAP, 0, 6, &info);
		sched_expect(g_ioc_out[0] == 0, "reading it cleared the flag",
		             g_ioc_out[0], 0, &bad);
		core_sched_unload(&sch);
		groups++;
	}

	/* ---- 3. the device name, and a buffer too small ---------------- */
	{
		IOC_OPEN();
		cs.device_name[0] = 'A';
		cs.device_name[1] = 'd';
		cs.device_name[2] = 'a';
		cs.device_name[3] = 0;

		st = ioc_call(&env, CORE_IOC_DEVICE_NAME, 0, 16, &info);
		sched_expect(st == CORE_ST_SUCCESS, "name read succeeds", (long)st,
		             0, &bad);
		sched_expect(info == 4, "three characters and the NUL", (long)info,
		             4, &bad);
		sched_expect(g_ioc_out[0] == 'A' && g_ioc_out[3] == 0, "contents",
		             g_ioc_out[0], 'A', &bad);

		/* a short buffer fills what it can and reports that much */
		ioc_call(&env, CORE_IOC_DEVICE_NAME, 0, 2, &info);
		sched_expect(info == 2, "a short buffer is filled, not overrun",
		             (long)info, 2, &bad);

		sched_expect(ioc_call(&env, CORE_IOC_DEVICE_NAME, 0, 0, &info) ==
		             CORE_ST_INVALID_PARAM, "a zero buffer is rejected",
		             1, 1, &bad);
		core_sched_unload(&sch);
		groups++;
	}

	/* ---- 4. the four counters, read and zero ----------------------- */
	{
		IOC_OPEN();
		cs.bcd_device      = 0x0123;
		cs.counter_two     = 4444;
		cs.reports_emitted = 77;
		cs.accessory_state = CORE_ACC_FOUND_2;

		wr32_test(g_ioc_in, CORE_COUNTER_FIRMWARE);
		ioc_call(&env, CORE_IOC_READ_COUNTER, 4, 4, &info);
		sched_expect(rd32_test(g_ioc_out) == 0x0123, "selector 1, bcdDevice",
		             (long)rd32_test(g_ioc_out), 0x0123, &bad);

		wr32_test(g_ioc_in, CORE_COUNTER_TWO);
		ioc_call(&env, CORE_IOC_READ_COUNTER, 4, 4, &info);
		sched_expect(rd32_test(g_ioc_out) == 4444, "selector 2",
		             (long)rd32_test(g_ioc_out), 4444, &bad);

		wr32_test(g_ioc_in, CORE_COUNTER_REPORTS);
		ioc_call(&env, CORE_IOC_READ_COUNTER, 4, 4, &info);
		sched_expect(rd32_test(g_ioc_out) == 77, "selector 3",
		             (long)rd32_test(g_ioc_out), 77, &bad);

		wr32_test(g_ioc_in, CORE_COUNTER_PROBE);
		ioc_call(&env, CORE_IOC_READ_COUNTER, 4, 4, &info);
		sched_expect(rd32_test(g_ioc_out) == CORE_ACC_FOUND_2, "selector 4",
		             (long)rd32_test(g_ioc_out), CORE_ACC_FOUND_2, &bad);

		wr32_test(g_ioc_in, 0);
		sched_expect(ioc_call(&env, CORE_IOC_READ_COUNTER, 4, 4, &info) ==
		             CORE_ST_INVALID_PARAM, "selector 0 rejected", 1, 1,
		             &bad);
		wr32_test(g_ioc_in, 5);
		sched_expect(ioc_call(&env, CORE_IOC_READ_COUNTER, 4, 4, &info) ==
		             CORE_ST_INVALID_PARAM, "selector 5 rejected", 1, 1,
		             &bad);

		/* only 2 and 3 are writable */
		wr32_test(g_ioc_in, CORE_COUNTER_TWO);
		ioc_call(&env, CORE_IOC_ZERO_COUNTER, 4, 0, &info);
		sched_expect(cs.counter_two == 0, "selector 2 zeroed",
		             (long)cs.counter_two, 0, &bad);
		wr32_test(g_ioc_in, CORE_COUNTER_REPORTS);
		ioc_call(&env, CORE_IOC_ZERO_COUNTER, 4, 0, &info);
		sched_expect(cs.reports_emitted == 0, "selector 3 zeroed",
		             (long)cs.reports_emitted, 0, &bad);
		wr32_test(g_ioc_in, CORE_COUNTER_FIRMWARE);
		st = ioc_call(&env, CORE_IOC_ZERO_COUNTER, 4, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS && cs.bcd_device == 0x0123,
		             "selector 1 accepted but does nothing",
		             cs.bcd_device, 0x0123, &bad);
		core_sched_unload(&sch);
		groups++;
	}

	/* ---- 5. the two get/set tunables ------------------------------- */
	{
		IOC_OPEN();

		/* read only */
		st = ioc_call(&env, CORE_IOC_STICK_CLIP, 0, 4, &info);
		sched_expect(st == CORE_ST_SUCCESS &&
		             rd32_test(g_ioc_out) == CORE_STICK_CLIP_DEFAULT,
		             "clip reads its default",
		             (long)rd32_test(g_ioc_out), CORE_STICK_CLIP_DEFAULT,
		             &bad);

		/* set and read in one call: the OLD value comes back */
		wr32_test(g_ioc_in, 100);
		ioc_call(&env, CORE_IOC_STICK_CLIP, 4, 4, &info);
		sched_expect(rd32_test(g_ioc_out) == CORE_STICK_CLIP_DEFAULT,
		             "set-and-get returns the old value",
		             (long)rd32_test(g_ioc_out), CORE_STICK_CLIP_DEFAULT,
		             &bad);
		sched_expect(cs.stick_clip == 100, "and stores the new one",
		             cs.stick_clip, 100, &bad);

		/* write only */
		wr32_test(g_ioc_in, 33);
		st = ioc_call(&env, CORE_IOC_STICK_STRETCH, 4, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS && cs.stick_stretch == 33,
		             "stretch write only", cs.stick_stretch, 33, &bad);
		sched_expect(info == 0, "and reports nothing back", (long)info, 0,
		             &bad);

		/* a value that would not survive a byte, proving the widening */
		wr32_test(g_ioc_in, 0x1234);
		ioc_call(&env, CORE_IOC_STICK_CLIP, 4, 0, &info);
		sched_expect(cs.stick_clip == 0x1234, "values above 255 survive",
		             cs.stick_clip, 0x1234, &bad);

		sched_expect(ioc_call(&env, CORE_IOC_STICK_CLIP, 2, 0, &info) ==
		             CORE_ST_INVALID_PARAM, "a 2-byte input is rejected",
		             1, 1, &bad);
		core_sched_unload(&sch);
		groups++;
	}

	/* ---- 6. the enable toggle -------------------------------------- */
	{
		IOC_OPEN();
		g_ioc_enable_calls = 0;

		wr32_test(g_ioc_in, 0);
		ioc_call(&env, CORE_IOC_SET_ENABLE, 4, 0, &info);
		sched_expect(g_ioc_enable_last == 0, "zero turns it off",
		             g_ioc_enable_last, 0, &bad);
		wr32_test(g_ioc_in, 7);
		ioc_call(&env, CORE_IOC_SET_ENABLE, 4, 0, &info);
		sched_expect(g_ioc_enable_last == 1, "any non-zero turns it on",
		             g_ioc_enable_last, 1, &bad);
		sched_expect(g_ioc_enable_calls == 2, "twice", g_ioc_enable_calls,
		             2, &bad);
		core_sched_unload(&sch);
		groups++;
	}

	/* ---- 7. DEFECT 11: the N64 passthrough receive bound ------------ */
	{
		static const u8 REPLY[] = {0x03, 0x02, 0x00, 0x05};

		IOC_OPEN();
		n64_set_reply(REPLY, 4);
		g_ioc_in[0] = CORE_N64_CMD_INFO;

		st = ioc_call(&env, CORE_IOC_N64_PASSTHRU, 1, 3, &info);
		sched_expect(st == CORE_ST_SUCCESS, "a normal transaction runs",
		             (long)st, 0, &bad);
		sched_expect(info == 3, "three bytes back", (long)info, 3, &bad);

		/* the bound: 63 is the largest reply the original's buffer held */
		n64_reset_log();
		st = ioc_call(&env, CORE_IOC_N64_PASSTHRU, 1, CORE_N64_RX_MAX,
		              &info);
		sched_expect(st == CORE_ST_SUCCESS, "63 is allowed", (long)st, 0,
		             &bad);
		n64_reset_log();
		st = ioc_call(&env, CORE_IOC_N64_PASSTHRU, 1, CORE_N64_RX_MAX + 1,
		              &info);
		sched_expect(st == CORE_ST_INVALID_PARAM, "64 is refused HERE",
		             (long)st, CORE_ST_INVALID_PARAM, &bad);
		sched_expect(g_n64count == 0, "before any transfer is attempted",
		             g_n64count, 0, &bad);
		n64_reset_log();
		st = ioc_call(&env, CORE_IOC_N64_PASSTHRU, 1, 200, &info);
		sched_expect(st == CORE_ST_INVALID_PARAM, "and so is 200",
		             (long)st, CORE_ST_INVALID_PARAM, &bad);

		sched_expect(ioc_call(&env, CORE_IOC_N64_PASSTHRU, 0, 3, &info) ==
		             CORE_ST_INVALID_PARAM, "a zero input is rejected", 1,
		             1, &bad);
		core_sched_unload(&sch);
		groups++;
	}

	/* ---- 8. DEFECT 14: the signed effect slot index ----------------- */
	{
		IOC_OPEN();
		cs.effect[0].block_length = 8;

		/* the valid range works */
		g_ioc_in[0] = CORE_EFFECT_CMD_START;
		g_ioc_in[1] = 0;
		st = ioc_call(&env, CORE_IOC_EFFECT_CTRL, 2, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS && cs.effect[0].running == 1,
		             "slot 0 starts", cs.effect[0].running, 1, &bad);

		g_ioc_in[0] = CORE_EFFECT_CMD_STOP;
		g_ioc_in[1] = 31;
		sched_expect(ioc_call(&env, CORE_IOC_EFFECT_CTRL, 2, 0, &info) ==
		             CORE_ST_SUCCESS, "slot 31 is in range", 1, 1, &bad);

		/* 32 and above were already rejected by the original */
		g_ioc_in[1] = 32;
		sched_expect(ioc_call(&env, CORE_IOC_EFFECT_CTRL, 2, 0, &info) ==
		             CORE_ST_INVALID_PARAM, "slot 32 is rejected", 1, 1,
		             &bad);

		/*
		 * These are the bytes the original accepts: read as a signed
		 * char, every one of them is negative and passes its
		 * upper-bound-only test.
		 */
		{
			static const u8 NEG[] = {0x80, 0xC0, 0xFF, 0xF0, 0xE0};
			int k;
			int rejected = 0;

			for (k = 0; k < 5; k++) {
				g_ioc_in[0] = CORE_EFFECT_CMD_STOP;
				g_ioc_in[1] = NEG[k];
				if (ioc_call(&env, CORE_IOC_EFFECT_CTRL, 2, 0, &info) ==
				    CORE_ST_INVALID_PARAM) {
					rejected++;
				}
				g_ioc_in[0] = CORE_EFFECT_CMD_START;
				if (ioc_call(&env, CORE_IOC_EFFECT_CTRL, 2, 0, &info) ==
				    CORE_ST_INVALID_PARAM) {
					rejected++;
				}
			}
			sched_expect(rejected == 10,
			             "all ten negative slot calls refused", rejected,
			             10, &bad);
		}

		/* the query path takes the same index and must refuse it too */
		g_ioc_in[0] = 0xFF;
		sched_expect(ioc_call(&env, CORE_IOC_EFFECT_QUERY, 1, 1, &info) ==
		             CORE_ST_INVALID_PARAM, "and the query path too", 1, 1,
		             &bad);
		g_ioc_in[0] = 0;
		sched_expect(ioc_call(&env, CORE_IOC_EFFECT_QUERY, 1, 1, &info) ==
		             CORE_ST_SUCCESS, "while slot 0 is answered", 1, 1,
		             &bad);
		core_sched_unload(&sch);
		groups++;
	}

	/* ---- 9. stop-all, and programming a slot ----------------------- */
	{
		IOC_OPEN();

		for (i = 0; i < CORE_EFFECT_SLOTS; i++) {
			cs.effect[i].running = 1;
		}
		g_ioc_in[0] = CORE_EFFECT_CMD_STOP_ALL;
		g_ioc_in[1] = 0;
		ioc_call(&env, CORE_IOC_EFFECT_CTRL, 2, 0, &info);
		{
			int any = 0;

			for (i = 0; i < CORE_EFFECT_SLOTS; i++) {
				if (cs.effect[i].running) {
					any = 1;
				}
			}
			sched_expect(!any, "stop-all clears all 32", any, 0, &bad);
		}

		/* program slot 3: header, then one 8-byte axis stream */
		wr32_test(g_ioc_in + 0x00, 3u | (0x01u << 16) | (0x55u << 24));
		wr32_test(g_ioc_in + 0x04, 1000);   /* duration     */
		wr32_test(g_ioc_in + 0x08, 11);     /* attack level */
		wr32_test(g_ioc_in + 0x0C, 22);     /* attack time  */
		wr32_test(g_ioc_in + 0x10, 33);     /* fade level   */
		wr32_test(g_ioc_in + 0x14, 44);     /* fade time    */
		wr32_test(g_ioc_in + 0x18, 8);      /* block length */
		for (i = 0; i < 8; i++) {
			g_ioc_in[0x1C + i] = (u8)(0x60 + i);
		}
		st = ioc_call(&env, CORE_IOC_EFFECT_PROG, 0x1C + 8, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS, "programming succeeds",
		             (long)st, 0, &bad);
		sched_expect(cs.effect[3].type == 0x55, "type from the top byte",
		             (long)cs.effect[3].type, 0x55, &bad);
		sched_expect(cs.effect[3].duration == 1000, "duration at +4",
		             cs.effect[3].duration, 1000, &bad);
		sched_expect(cs.effect[3].attack_level == 11, "attack level at +8",
		             cs.effect[3].attack_level, 11, &bad);
		sched_expect(cs.effect[3].attack_time == 22, "attack time at +0C",
		             cs.effect[3].attack_time, 22, &bad);
		sched_expect(cs.effect[3].fade_level == 33, "fade level at +10",
		             cs.effect[3].fade_level, 33, &bad);
		sched_expect(cs.effect[3].fade_time == 44, "fade time at +14",
		             cs.effect[3].fade_time, 44, &bad);
		sched_expect(cs.effect[3].block_length == 8, "block length at +18",
		             cs.effect[3].block_length, 8, &bad);
		sched_expect(cs.effect[3].axis[0].periodic.magnitude == 0x60,
		             "the stream landed on axis 0",
		             cs.effect[3].axis[0].periodic.magnitude, 0x60, &bad);
		sched_expect(cs.effect[3].running == 1, "and the slot started",
		             cs.effect[3].running, 1, &bad);

		/* a truncated payload is rejected on length */
		sched_expect(ioc_call(&env, CORE_IOC_EFFECT_PROG, 0x1C + 4, 0,
		                      &info) == CORE_ST_INVALID_PARAM,
		             "a short payload is rejected", 1, 1, &bad);
		/* and so is an out-of-range slot */
		wr32_test(g_ioc_in + 0x00, 32u);
		wr32_test(g_ioc_in + 0x18, 0);
		sched_expect(ioc_call(&env, CORE_IOC_EFFECT_PROG, 0x1C, 0, &info) ==
		             CORE_ST_INVALID_PARAM, "slot 32 is rejected", 1, 1,
		             &bad);
		core_sched_unload(&sch);
		groups++;
	}

	/* ---- 10. the Controller Pak paths, and their CRC answers -------- */
	{
		u8 reply[CORE_PAK_BLOCK_BYTES + 2];

		IOC_OPEN();
		/* a good read: 32 data bytes then the CRC the device computed */
		reply[0] = 0x21;                        /* the length byte */
		for (i = 0; i < CORE_PAK_BLOCK_BYTES; i++) {
			reply[1 + i] = (u8)(0xA0 + i);
		}
		{
			/* the transaction reverses, so lay the payload out backwards */
			u8 fwd[CORE_PAK_BLOCK_BYTES + 1];
			u8 crc;

			for (i = 0; i < CORE_PAK_BLOCK_BYTES; i++) {
				fwd[i] = (u8)(0xA0 + i);
			}
			crc = core_pak_data_crc8(fwd, CORE_PAK_BLOCK_BYTES);
			fwd[CORE_PAK_BLOCK_BYTES] = crc;
			reply[0] = 0x21;
			for (i = 0; i < CORE_PAK_BLOCK_BYTES + 1; i++) {
				reply[1 + i] = fwd[CORE_PAK_BLOCK_BYTES - i];
			}
			n64_set_reply(reply, CORE_PAK_BLOCK_BYTES + 2);
		}
		wr32_test(g_ioc_in, 0x40);
		st = ioc_call(&env, CORE_IOC_PAK_READ, 4, 32, &info);
		sched_expect(st == CORE_ST_SUCCESS, "a good block read succeeds",
		             (long)st, 0, &bad);
		sched_expect(info == 32, "32 bytes", (long)info, 32, &bad);
		sched_expect(g_ioc_out[0] == 0xA0 && g_ioc_out[31] == 0xBF,
		             "with the right contents", g_ioc_out[0], 0xA0, &bad);

		/* corrupt the CRC byte -> CRC error */
		n64_reset_log();
		reply[1] = (u8)(reply[1] ^ 0x01);
		n64_set_reply(reply, CORE_PAK_BLOCK_BYTES + 2);
		st = ioc_call(&env, CORE_IOC_PAK_READ, 4, 32, &info);
		sched_expect(st == CORE_ST_CRC_ERROR, "a bad CRC is reported",
		             (long)st, CORE_ST_CRC_ERROR, &bad);

		sched_expect(ioc_call(&env, CORE_IOC_PAK_READ, 4, 31, &info) ==
		             CORE_ST_INVALID_PARAM, "a 31-byte buffer is rejected",
		             1, 1, &bad);
		sched_expect(ioc_call(&env, CORE_IOC_PAK_WRITE, 0x23, 0, &info) ==
		             CORE_ST_INVALID_PARAM, "a 0x23 write is rejected", 1,
		             1, &bad);
		core_sched_unload(&sch);
		groups++;
	}

	/* ---- 11. script load, and the fault drain ---------------------- */
	{
		IOC_OPEN();

		/* in_len must equal code_count * 4 + 8 exactly */
		wr32_test(g_ioc_in + 0, 1);           /* code_count */
		wr32_test(g_ioc_in + 4, 4);           /* var_count  */
		wr32_test(g_ioc_in + 8, 0x082);       /* ret        */
		st = ioc_call(&env, CORE_IOC_SCRIPT_LOAD, 12, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS, "a one-word script loads",
		             (long)st, 0, &bad);
		sched_expect(sch.vm.code_count == 1, "and is installed",
		             sch.vm.code_count, 1, &bad);

		sched_expect(ioc_call(&env, CORE_IOC_SCRIPT_LOAD, 13, 0, &info) ==
		             CORE_ST_INVALID_PARAM, "a length that disagrees is "
		             "rejected", 1, 1, &bad);
		sched_expect(ioc_call(&env, CORE_IOC_SCRIPT_LOAD, 4, 0, &info) ==
		             CORE_ST_INVALID_PARAM, "and one too short to hold a "
		             "header", 1, 1, &bad);

		/* a count that would overflow the multiply must not match */
		wr32_test(g_ioc_in + 0, 0x40000001u);
		sched_expect(ioc_call(&env, CORE_IOC_SCRIPT_LOAD, 12, 0, &info) ==
		             CORE_ST_INVALID_PARAM, "an overflowing count is "
		             "rejected", 1, 1, &bad);

		/* code_count 0 is the unload */
		wr32_test(g_ioc_in + 0, 0);
		wr32_test(g_ioc_in + 4, 4);
		st = ioc_call(&env, CORE_IOC_SCRIPT_LOAD, 8, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS && sch.vm.code == 0,
		             "count 0 unloads", sch.vm.code != 0, 0, &bad);

		/* the fault drain with nothing to drain */
		st = ioc_call(&env, CORE_IOC_SCRIPT_FAULT, 0, 64, &info);
		sched_expect(st == CORE_ST_SUCCESS && info == 0,
		             "an empty fault slot reports nothing", (long)info, 0,
		             &bad);
		core_sched_unload(&sch);
		groups++;
	}

	/* ---- 12. the fault drain releases both blocks ------------------- */
	{
		static const u32 LOOP[] = {0x170, 0xFFFFFFFEu};
		int live_before;

		IOC_OPEN();
		g_sched_live = 0;
		core_sched_set_native(&sch, core_sched_native, &sch);
		core_sched_load(&sch, LOOP, 2, 4, 0);   /* runs out of budget */

		sched_expect(sch.fault_thread != 0, "a thread faulted",
		             sch.fault_thread != 0, 1, &bad);
		live_before = g_sched_live;

		/*
		 * An output buffer exactly the size of the thread node leaves no
		 * room for the globals. The original frees the snapshot only
		 * inside the branch that copies it, so this is the shape that
		 * leaks; both must come back here.
		 */
		st = ioc_call(&env, CORE_IOC_SCRIPT_FAULT, 0,
		              (u32)(sizeof(core_sched_thread) +
		                    (u32)sch.fault_thread->stack_size * 4u), &info);
		sched_expect(st == CORE_ST_SUCCESS, "the drain succeeds", (long)st,
		             0, &bad);
		sched_expect(sch.fault_thread == 0, "the slot is emptied",
		             sch.fault_thread != 0, 0, &bad);
		sched_expect(g_sched_live == live_before - 2,
		             "BOTH blocks were released",
		             live_before - g_sched_live, 2, &bad);
		core_sched_unload(&sch);
		sched_expect(g_sched_live == 0, "all memory returned",
		             g_sched_live, 0, &bad);
		groups++;
	}

	/* ---- 13. the raw vendor passthrough buffer split ---------------- */
	{
		IOC_OPEN();
		g_ioc_vendor_ret   = CORE_ST_SUCCESS;
		g_ioc_vendor_calls = 0;

		/* IN: six-byte setup with the direction bit, data to the output */
		g_ioc_in[0] = 0xC0;
		st = ioc_call(&env, CORE_IOC_RAW_VENDOR, 6, 8, &info);
		sched_expect(st == CORE_ST_SUCCESS, "an IN transfer is accepted",
		             (long)st, 0, &bad);
		sched_expect(g_ioc_vendor_len == 8, "data length is the output",
		             (long)g_ioc_vendor_len, 8, &bad);

		/* OUT: data follows the setup packet in the input buffer */
		g_ioc_in[0] = 0x40;
		st = ioc_call(&env, CORE_IOC_RAW_VENDOR, 6 + 5, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS, "an OUT transfer is accepted",
		             (long)st, 0, &bad);
		sched_expect(g_ioc_vendor_len == 5, "data length is the tail",
		             (long)g_ioc_vendor_len, 5, &bad);

		/* no data stage */
		st = ioc_call(&env, CORE_IOC_RAW_VENDOR, 6, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS && g_ioc_vendor_len == 0,
		             "a setup-only transfer is accepted",
		             (long)g_ioc_vendor_len, 0, &bad);

		/* mismatched direction and buffers are rejected */
		g_ioc_in[0] = 0xC0;
		sched_expect(ioc_call(&env, CORE_IOC_RAW_VENDOR, 6 + 5, 0, &info) ==
		             CORE_ST_INVALID_PARAM, "IN with input data is "
		             "rejected", 1, 1, &bad);
		g_ioc_in[0] = 0x40;
		sched_expect(ioc_call(&env, CORE_IOC_RAW_VENDOR, 6, 8, &info) ==
		             CORE_ST_INVALID_PARAM, "OUT with an output buffer is "
		             "rejected", 1, 1, &bad);
		sched_expect(ioc_call(&env, CORE_IOC_RAW_VENDOR, 5, 0, &info) ==
		             CORE_ST_INVALID_PARAM, "a short setup is rejected",
		             1, 1, &bad);

		/* the seam can answer PENDING, and only this case can */
		g_ioc_vendor_ret = CORE_ST_PENDING;
		g_ioc_in[0] = 0xC0;
		sched_expect(ioc_call(&env, CORE_IOC_RAW_VENDOR, 6, 8, &info) ==
		             CORE_ST_PENDING, "and PENDING is passed through", 1,
		             1, &bad);
		core_sched_unload(&sch);
		groups++;
	}

	/* ---- 14. the accept-and-ignore case ---------------------------- */
	{
		IOC_OPEN();
		sched_expect(ioc_call(&env, CORE_IOC_ACCEPT_NOP, 0, 0, &info) ==
		             CORE_ST_SUCCESS, "an empty request is accepted", 1, 1,
		             &bad);
		sched_expect(ioc_call(&env, CORE_IOC_ACCEPT_NOP, 0x200, 0, &info) ==
		             CORE_ST_SUCCESS, "0x200 bytes are accepted", 1, 1,
		             &bad);
		sched_expect(ioc_call(&env, CORE_IOC_ACCEPT_NOP, 0x201, 0, &info) ==
		             CORE_ST_INVALID_PARAM, "0x201 is not", 1, 1, &bad);
		sched_expect(ioc_call(&env, CORE_IOC_ACCEPT_NOP, 0, 4, &info) ==
		             CORE_ST_INVALID_PARAM, "nor is an output buffer", 1,
		             1, &bad);
		core_sched_unload(&sch);
		groups++;
	}

#undef IOC_OPEN

	hlog("Private IOCTL surface  : %s (%d groups)\n", bad ? "FAIL" : "ok",
	     groups);
	return bad;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
	printf("usage: %s [-v] [--log FILE] [--trace FILE]\n", argv0);
	printf("  -v            print the whole run, not just results\n");
	printf("  --log FILE    tee console output to FILE\n");
	printf("  --trace FILE  write the numeric trace to FILE\n");
}

int main(int argc, char **argv)
{
	DRIVER_OBJECT        driver;
	UNICODE_STRING       regpath;
	DEVICE_OBJECT        fdo, pdo, lower;
	HID_DEVICE_EXTENSION hidext;
	PADAPTOID_DEVEXT     devext;
	NTSTATUS             status;
	int                  i;
	int                  count;
	int                  bad = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0) {
			g_verbose = 1;
		} else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
			g_log = fopen(argv[++i], "w");
			if (g_log == NULL) {
				printf("cannot open log file %s\n", argv[i]);
				return 2;
			}
		} else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
			g_trace = fopen(argv[++i], "w");
			if (g_trace == NULL) {
				printf("cannot open trace file %s\n", argv[i]);
				return 2;
			}
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	/* X, Y, status, button bits 8..15, button bits 0..7. */
	static const u8 packets[][CORE_RAW_PACKET_BYTES] = {
		{ 0x00, 0x00, 0x80, 0x00, 0x00 },   /* centre, nothing held  */
		{ 0x00, 0x00, 0x80, 0x00, 0x80 },   /* A                     */
		{ 0x41, 0x41, 0x80, 0x00, 0x00 },   /* stick into a corner   */
		{ 0xB0, 0x00, 0x80, 0x20, 0x00 }    /* full left, L held     */
	};

	count = (int)(sizeof(packets) / sizeof(packets[0]));

	hverbose("wishk300 test harness\n");
	hverbose("=====================\n\n");

	memset(&driver,  0, sizeof(driver));
	memset(&regpath, 0, sizeof(regpath));
	memset(&fdo,     0, sizeof(fdo));
	memset(&pdo,     0, sizeof(pdo));
	memset(&lower,   0, sizeof(lower));
	memset(&hidext,  0, sizeof(hidext));

	/* 0. Pure functions first - they need no device. */
	bad += test_pak_crcs();
	bad += test_joystick_report();
	bad += test_accessory_probe();
	bad += test_effect_engine();
	bad += test_effect_chain();
	bad += test_pak_change();
	bad += test_tune_mode();
	bad += test_script();
	bad += test_sched();
	bad += test_input_bind();
	bad += test_natives();
	bad += test_n64_transaction();
	bad += test_hid_reports();
	bad += test_ioctl();

	/* 1. Load. */
	status = DriverEntry(&driver, &regpath);
	hverbose("DriverEntry            : 0x%08lX  (registered=%d, "
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
	hverbose("AddDevice              : ok\n");

	/*
	 * 3. Redirect the core output at the harness. In the driver this stays
	 *    AdaptoidReportSink, which completes a pending HID read IRP. Swapping
	 *    it here is the whole point of the seam.
	 */
	core_init(&devext->Core, harness_sink, devext);
	core_set_vendor(&devext->Core, harness_vendor, 0);
	hverbose("sink                   : harness (devices_mask=0x%X)\n",
	       (unsigned)devext->Core.devices_mask);

	/*
	 * The probe gates input, so run it before feeding packets - which is
	 * exactly what the driver does on its first poll.
	 */
	{
		u8 ident[CORE_PROBE_REPLY_BYTES] = { 0x03, 0x01, 0x00, 0x05 };

		bus_reset(ident);
		core_probe_start(&devext->Core);
		harness_pump(&devext->Core);
	}
	hverbose("accessory probe        : state %u after %d transfers, "
	       "status 0x%02X\n\n",
	       devext->Core.accessory_state, g_bus_count,
	       devext->Core.accessory_status);

	/* 4. Drive it. */
	hverbose("feeding %d raw packets:\n", count);
	for (i = 0; i < count; i++) {
		clock_advance_ms(16);              /* ~1/64 s, the script time base */
		core_tick(&devext->Core, KeQueryInterruptTime());
		core_on_raw_packet(&devext->Core, packets[i]);
	}

	/* 5. Report. */
	hverbose("\nsummary\n");
	hverbose("  reports emitted      : %lu\n",
	       (unsigned long)devext->Core.reports_emitted);
	hverbose("  reports seen by sink : %lu\n", g_reports_seen);
	hverbose("  pool allocs / frees  : %lu / %lu\n",
	         g_pool_allocs, g_pool_frees);
	hverbose("  final clock          : %llu ms\n",
	       (unsigned long long)(g_interrupt_time_100ns / 10000ULL));

	if (g_reports_seen != devext->Core.reports_emitted) {
		hlog("\nFAIL: sink count does not match emitted count\n");
		bad++;
	}

	AdaptoidUnload(&driver);
	free(hidext.MiniDeviceExtension);

	if (bad) {
		hlog("\n%d failure(s)\n", bad);
	} else {
		hlog("\nok\n");
	}

	if (g_log) {
		fclose(g_log);
	}
	if (g_trace) {
		fclose(g_trace);
	}
	return bad ? 1 : 0;
}
