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
