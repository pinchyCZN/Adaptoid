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

/*
 * The pointer-width twin. It exists because the 32-bit one silently
 * truncates a 64-bit pointer, which on x64 leaves half of a cancel routine
 * in the IRP and trips Verifier's 0xC9/7.
 */
PVOID InterlockedExchangePointer(PVOID volatile *Target, PVOID Value)
{
	PVOID old = *Target;
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

/*
 * THE SLOT HAS TO BE CLAIMED BEFORE IT CAN BE USED, and this stub enforces
 * it because the driver does. AdaptoidVendorSend answers STATUS_DEVICE_BUSY
 * to any send whose slot is not already CLAIMED, so a transport here that
 * accepted unclaimed sends would be MORE PERMISSIVE THAN THE REAL ONE - and
 * every test would pass while the driver deadlocked its own input path.
 * That is exactly what happened: core_probe_issue sent without claiming,
 * nothing here objected, and on hardware the probe was refused forever.
 */
static int g_bus_claimed;

static int harness_vendor_claim(void *ctx)
{
	(void)ctx;
	if (g_bus_busy || g_bus_claimed) {
		return 0;
	}
	g_bus_claimed = 1;
	return 1;
}

/*
 * The strict transport: refuses anything that has not claimed the slot,
 * which is what AdaptoidVendorSend does. Used only by the scenario that
 * exists to check the claim, because turning arbitration on globally would
 * change the meaning of every effect test - core_effect_claim treats an
 * absent claim seam as "no arbitration installed" and proceeds.
 */
static int harness_vendor(void *ctx, const core_vendor_req *req);

static int harness_vendor_strict(void *ctx, const core_vendor_req *req)
{
	if (!g_bus_claimed) {
		return 0;
	}
	g_bus_claimed = 0;
	return harness_vendor(ctx, req);
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
		/*
		 * THE SLOT STAYS CLAIMED ACROSS THE CALLBACK, as the driver's
		 * completion path does - it parks the slot at CLAIMED before
		 * calling back "while the chain below decides", so a multi-step
		 * sequence can issue its next transfer without re-claiming.
		 * Model it, or the strict transport refuses every step after
		 * the first and the failure looks like a stalled probe rather
		 * than a mis-modelled slot.
		 */
		g_bus_claimed = 1;
		if (g_bus_fail_next) {
			g_bus_fail_next = 0;
			core_probe_complete(cs, 0, 0, 0);
		} else if (is_in) {
			core_probe_complete(cs, 1, g_bus_reply,
			                    CORE_PROBE_REPLY_BYTES);
		} else {
			core_probe_complete(cs, 1, 0, 0);
		}
		/* Whatever the chain did not take, the transport reclaims. */
		g_bus_claimed = 0;
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

	/*
	 * 7. THE PROBE MUST CLAIM THE SLOT BEFORE IT SENDS.
	 *
	 * harness_vendor is deliberately permissive - it accepts any send
	 * while the bus is free - because most tests want to drive the
	 * transport without modelling arbitration. THE DRIVER IS NOT
	 * PERMISSIVE: AdaptoidVendorSend answers STATUS_DEVICE_BUSY to a send
	 * whose slot is not already CLAIMED. A core path that sends without
	 * claiming therefore passes every test here and cannot work on
	 * hardware.
	 *
	 * That is not hypothetical. core_probe_issue shipped without the
	 * claim, and because the probe gates every report the result was a
	 * driver that enumerated, served all three descriptors, bound
	 * correctly - and delivered no input at all, with no error reported
	 * anywhere. The loop was: packet arrives, probe starts, unclaimed
	 * send refused, probe abandoned back to NEEDED, repeat forever.
	 *
	 * harness_vendor_strict enforces what the driver enforces, so this
	 * scenario fails the moment the claim is dropped again.
	 */
	core_init(&cs, 0, 0);
	core_set_vendor(&cs, harness_vendor_strict, 0);
	core_set_vendor_claim(&cs, harness_vendor_claim);
	bus_reset(good);
	core_probe_start(&cs);
	harness_pump(&cs);
	if (g_bus_count == 0) {
		hlog("  FAIL probe claim: the strict transport refused every "
		     "send - the probe did not claim the slot\n");
		bad++;
	}
	if (cs.accessory_state != CORE_ACC_FOUND_1) {
		hlog("  FAIL probe claim: state %u, want %u\n",
		       cs.accessory_state, CORE_ACC_FOUND_1);
		bad++;
	}

	/*
	 * 8. A REFUSED SEND MUST NOT STEAL THE OWNER.
	 *
	 * vendor_owner says who the NEXT completion belongs to. The effect
	 * engine runs from core_tick on the very packet that starts the
	 * probe, so it tries to send while the probe's transfer is still in
	 * flight and is refused - the single slot is taken. If it has already
	 * written EFFECT over PROBE by then, the probe's completion is
	 * delivered to core_effect_complete instead, and the probe waits for
	 * an answer that was handed to somebody else.
	 *
	 * The symptom is the whole driver going quiet: accessory_state stuck
	 * at PROBING, probe_step stuck at 0, every report gated, and not one
	 * error reported anywhere.
	 */
	core_init(&cs, 0, 0);
	core_set_vendor(&cs, harness_vendor, 0);
	bus_reset(good);

	core_probe_start(&cs);              /* claims nothing here, but issues */
	if (cs.vendor_owner != CORE_VENDOR_OWNER_PROBE) {
		hlog("  FAIL probe owner: after issue owner is %u, want %u\n",
		       cs.vendor_owner, CORE_VENDOR_OWNER_PROBE);
		bad++;
	}

	/* The slot is busy, so this send is refused. */
	if (core_effect_send_idle(&cs)) {
		hlog("  FAIL probe owner: the busy slot accepted a send\n");
		bad++;
	}
	if (cs.vendor_owner != CORE_VENDOR_OWNER_PROBE) {
		hlog("  FAIL probe owner: a refused send took the owner - "
		     "got %u, want %u\n", cs.vendor_owner,
		       CORE_VENDOR_OWNER_PROBE);
		bad++;
	}

	/* And the probe still finishes, because its completion still routes
	 * back to it. */
	harness_pump(&cs);
	if (cs.accessory_state != CORE_ACC_FOUND_1) {
		hlog("  FAIL probe owner: state %u, want %u\n",
		       cs.accessory_state, CORE_ACC_FOUND_1);
		bad++;
	}

	hlog("Accessory probe        : %s (8 scenarios)\n", bad ? "FAIL" : "ok");
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
static const u32 P06[] = {PROLOGUE, 0x110,0xFFFFFFF7u,0x093,0x110,2,0x255,
	                      EPILOGUE};
static const u32 P07[] = {PROLOGUE, 0x110,0xFFFFFFF7u,0x093,0x110,2,0x256,
	                      EPILOGUE};
static const u32 P08[] = {PROLOGUE, 0x110,1,0x093,0x110,4,0x250, EPILOGUE};
static const u32 P09[] = {PROLOGUE, 0x110,0xFFFFFF00u,0x093,0x110,4,0x251,
	                      EPILOGUE};
static const u32 P10[] = {PROLOGUE, 0x110,5,0x093,0x110,3,0x262, EPILOGUE};
static const u32 P11[] = {PROLOGUE, 0x110,3,0x093,0x110,5,0x262, EPILOGUE};
static const u32 P12[] = {PROLOGUE, 0x110,0xFFFFFFFBu,0x093,0x110,3,0x263,
	                      EPILOGUE};
static const u32 P13[] = {PROLOGUE, 0x110,5,0x093,0x110,5,0x264, EPILOGUE};
static const u32 P14[] = {PROLOGUE, 0x110,0,0x093,0x110,7,0x266, EPILOGUE};
static const u32 P15[] = {PROLOGUE, 0x110,0,0x093,0x110,7,0x267, EPILOGUE};
static const u32 P16[] = {PROLOGUE, 0x110,5,0x040, EPILOGUE};
static const u32 P17[] = {PROLOGUE, 0x110,5,0x041, EPILOGUE};
static const u32 P18[] = {PROLOGUE, 0x110,5,0x042, EPILOGUE};
static const u32 P19[] = {PROLOGUE, 0x110,1,0x172,2,0x110,111, EPILOGUE};
static const u32 P20[] = {PROLOGUE, 0x110,0,0x172,2,0x110,111,0x110,222,
	                      EPILOGUE};
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
		cs.instance_id     = 0x123;
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

static int g_wait_would_block;
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
/* the control device and the notification queue                       */
/* ------------------------------------------------------------------ */

#define CTLLOG 256

static struct {
	void *req;
	u32   type, a1, a2;
} g_ctl_delivered[CTLLOG];
static int g_ctl_delcount;
static int g_ctl_abortcount;

/* Which waiters the owner will refuse to hand over, standing in for a
 * request that cancellation already claimed. */
static void *g_ctl_unclaimable;

static int ctl_claim(void *ctx, core_notify_waiter *w)
{
	(void)ctx;
	return w->request != g_ctl_unclaimable;
}

static void ctl_deliver(void *ctx, core_notify_waiter *w, u32 t, u32 a1,
                        u32 a2)
{
	(void)ctx;
	if (g_ctl_delcount < CTLLOG) {
		g_ctl_delivered[g_ctl_delcount].req  = w->request;
		g_ctl_delivered[g_ctl_delcount].type = t;
		g_ctl_delivered[g_ctl_delcount].a1   = a1;
		g_ctl_delivered[g_ctl_delcount].a2   = a2;
	}
	g_ctl_delcount++;
}

static void ctl_abort(void *ctx, core_notify_waiter *w)
{
	(void)ctx;
	(void)w;
	g_ctl_abortcount++;
}

static void ctl_reset_log(void)
{
	g_ctl_delcount    = 0;
	g_ctl_abortcount  = 0;
	g_ctl_unclaimable = 0;
}

static u32 ctl_call(core_registry *reg, u32 fn, u32 in_len, u32 out_len,
                    core_notify_waiter *w, u32 *info)
{
	core_ioctl r;

	r.code    = CORE_IOCTL_CODE(fn);
	r.in      = g_ioc_in;
	r.in_len  = in_len;
	r.out     = g_ioc_out;
	r.out_len = out_len;
	return core_ctl_dispatch(reg, &r, w, 0, info);
}

static int test_control_device(void)
{
	int bad    = 0;
	int groups = 0;
	core_registry reg;
	core_notify_waiter w[8];
	u32 st, info;
	int i;

	/* ---- 1. the queue delivers FIFO, one event per waiter ---------- */
	{
		core_registry_init(&reg);
		core_notify_init(&reg.notify, ctl_claim, ctl_deliver, ctl_abort, 0);
		ctl_reset_log();

		/* events with no waiter simply queue */
		for (i = 0; i < 3; i++) {
			core_notify_post(&reg.notify, CORE_EVENT_DEBUG, (u32)i, 0);
		}
		sched_expect(reg.notify.count == 3, "three events queued",
		             reg.notify.count, 3, &bad);
		sched_expect(g_ctl_delcount == 0, "and nothing delivered",
		             g_ctl_delcount, 0, &bad);

		/* each waiter takes exactly one, oldest first */
		for (i = 0; i < 2; i++) {
			w[i].request = &w[i];
			core_notify_wait(&reg.notify, &w[i]);
		}
		sched_expect(g_ctl_delcount == 2, "two waiters, two deliveries",
		             g_ctl_delcount, 2, &bad);
		sched_expect(g_ctl_delivered[0].a1 == 0 &&
		             g_ctl_delivered[1].a1 == 1, "in arrival order",
		             (long)g_ctl_delivered[0].a1, 0, &bad);
		sched_expect(reg.notify.count == 1, "one event still queued",
		             reg.notify.count, 1, &bad);

		/* a waiter parked before an event is served when it arrives */
		w[2].request = &w[2];
		core_notify_wait(&reg.notify, &w[2]);
		sched_expect(g_ctl_delcount == 3, "the third event went out",
		             g_ctl_delcount, 3, &bad);
		w[3].request = &w[3];
		sched_expect(core_notify_wait(&reg.notify, &w[3]) == 0,
		             "a waiter with no event parks", 0, 0, &bad);
		core_notify_post(&reg.notify, CORE_EVENT_DEBUG, 99, 0);
		sched_expect(g_ctl_delcount == 4 &&
		             g_ctl_delivered[3].a1 == 99,
		             "and is served on the next post",
		             (long)g_ctl_delivered[3].a1, 99, &bad);
		groups++;
	}

	/* ---- 2. the hundred-event cap drops the oldest ----------------- */
	{
		core_registry_init(&reg);
		core_notify_init(&reg.notify, ctl_claim, ctl_deliver, ctl_abort, 0);
		ctl_reset_log();

		for (i = 0; i < 150; i++) {
			core_notify_post(&reg.notify, CORE_EVENT_DEBUG, (u32)i, 0);
		}
		sched_expect(reg.notify.count == CORE_NOTIFY_MAX, "the ring is full",
		             reg.notify.count, CORE_NOTIFY_MAX, &bad);
		sched_expect(reg.notify.dropped == 50, "fifty were dropped",
		             (long)reg.notify.dropped, 50, &bad);

		w[0].request = &w[0];
		core_notify_wait(&reg.notify, &w[0]);
		sched_expect(g_ctl_delivered[0].a1 == 50,
		             "the oldest survivor is 50",
		             (long)g_ctl_delivered[0].a1, 50, &bad);
		groups++;
	}

	/* ---- 3. a waiter the owner will not claim keeps its event ------ */
	{
		core_registry_init(&reg);
		core_notify_init(&reg.notify, ctl_claim, ctl_deliver, ctl_abort, 0);
		ctl_reset_log();

		w[0].request = &w[0];
		w[1].request = &w[1];
		core_notify_wait(&reg.notify, &w[0]);
		core_notify_wait(&reg.notify, &w[1]);

		/* cancellation got to the first one */
		g_ctl_unclaimable = &w[0];
		core_notify_post(&reg.notify, CORE_EVENT_DEBUG, 7, 0);

		sched_expect(g_ctl_delcount == 1,
		             "the unclaimable waiter was skipped", g_ctl_delcount,
		             1, &bad);
		sched_expect(g_ctl_delivered[0].req == &w[1],
		             "and the event went to the next one",
		             g_ctl_delivered[0].req == &w[1], 1, &bad);
		sched_expect(reg.notify.count == 0, "the event was NOT wasted",
		             reg.notify.count, 0, &bad);
		groups++;
	}

	/* ---- 4. cancel, and the teardown flush ------------------------- */
	{
		core_registry_init(&reg);
		core_notify_init(&reg.notify, ctl_claim, ctl_deliver, ctl_abort, 0);
		ctl_reset_log();

		for (i = 0; i < 3; i++) {
			w[i].request = &w[i];
			core_notify_wait(&reg.notify, &w[i]);
		}
		sched_expect(reg.notify.waiter_count == 3, "three parked",
		             reg.notify.waiter_count, 3, &bad);
		sched_expect(core_notify_cancel(&reg.notify, &w[1]) == 1,
		             "the middle one is withdrawn", 1, 1, &bad);
		sched_expect(reg.notify.waiter_count == 2, "two left",
		             reg.notify.waiter_count, 2, &bad);
		sched_expect(core_notify_cancel(&reg.notify, &w[1]) == 0,
		             "withdrawing it twice is refused", 0, 0, &bad);

		core_notify_flush(&reg.notify);
		sched_expect(g_ctl_abortcount == 2, "the flush aborted both",
		             g_ctl_abortcount, 2, &bad);
		sched_expect(reg.notify.waiter_count == 0, "and emptied the list",
		             reg.notify.waiter_count, 0, &bad);
		groups++;
	}

	/* ---- 5. the registry, and the interface event ------------------ */
	{
		core_device_entry d1, d2;
		core_state cs1, cs2;

		core_registry_init(&reg);
		core_notify_init(&reg.notify, ctl_claim, ctl_deliver, ctl_abort, 0);
		ctl_reset_log();
		core_init(&cs1, 0, 0);
		core_init(&cs2, 0, 0);

		d1.handle = 0x1111; d1.cs = &cs1; d1.sched = 0; d1.live = 0;
		d2.handle = 0x2222; d2.cs = &cs2; d2.sched = 0; d2.live = 0;
		core_registry_add(&reg, &d1);
		core_registry_add(&reg, &d2);
		sched_expect(reg.count == 2, "two registered", reg.count, 2, &bad);
		sched_expect(reg.live_count == 0, "neither live yet",
		             reg.live_count, 0, &bad);

		core_registry_set_live(&reg, &d1, 1);
		sched_expect(reg.live_count == 1, "one live", reg.live_count, 1,
		             &bad);
		sched_expect(reg.generation == 1, "the generation moved",
		             (long)reg.generation, 1, &bad);
		sched_expect(reg.notify.count == 1, "an event was posted",
		             reg.notify.count, 1, &bad);
		sched_expect(reg.notify.events[0].type == CORE_EVENT_INTERFACE,
		             "of type 99", (long)reg.notify.events[0].type,
		             CORE_EVENT_INTERFACE, &bad);

		/* the SAME event on the way out; a listener must re-enumerate */
		core_registry_set_live(&reg, &d1, 0);
		sched_expect(reg.notify.events[1].type == CORE_EVENT_INTERFACE,
		             "departure posts the same type",
		             (long)reg.notify.events[1].type, CORE_EVENT_INTERFACE,
		             &bad);
		sched_expect(reg.generation == 2, "and moves it again",
		             (long)reg.generation, 2, &bad);

		/* setting the same state twice changes nothing */
		core_registry_set_live(&reg, &d1, 0);
		sched_expect(reg.generation == 2, "a repeat is ignored",
		             (long)reg.generation, 2, &bad);

		core_registry_remove(&reg, &d2);
		sched_expect(reg.count == 1, "one removed", reg.count, 1, &bad);
		groups++;
	}

	/* ---- 6. the last adapter leaving releases every waiter --------- */
	{
		core_device_entry d1;
		core_state cs1;

		core_registry_init(&reg);
		core_notify_init(&reg.notify, ctl_claim, ctl_deliver, ctl_abort, 0);
		ctl_reset_log();
		core_init(&cs1, 0, 0);
		d1.handle = 1; d1.cs = &cs1; d1.sched = 0; d1.live = 0;
		core_registry_add(&reg, &d1);
		core_registry_set_live(&reg, &d1, 1);

		/* drain the arrival event so the waiters actually park */
		reg.notify.count = 0;
		for (i = 0; i < 3; i++) {
			w[i].request = &w[i];
			core_notify_wait(&reg.notify, &w[i]);
		}
		sched_expect(reg.notify.waiter_count == 3, "three parked",
		             reg.notify.waiter_count, 3, &bad);

		g_ctl_abortcount = 0;
		core_registry_set_live(&reg, &d1, 0);
		sched_expect(reg.notify.waiter_count == 0,
		             "the last adapter leaving frees them all",
		             reg.notify.waiter_count, 0, &bad);
		groups++;
	}

	/* ---- 7. the simple control codes ------------------------------- */
	{
		core_device_entry d1;
		core_state cs1;

		core_registry_init(&reg);
		core_notify_init(&reg.notify, ctl_claim, ctl_deliver, ctl_abort, 0);
		ctl_reset_log();
		core_init(&cs1, 0, 0);
		d1.handle = 0xABCD; d1.cs = &cs1; d1.sched = 0; d1.live = 0;
		core_registry_add(&reg, &d1);
		core_registry_set_live(&reg, &d1, 1);

		st = ctl_call(&reg, CORE_CTL_DEVICE_COUNT, 0, 1, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS && g_ioc_out[0] == 1,
		             "the live count is one byte", g_ioc_out[0], 1, &bad);

		st = ctl_call(&reg, CORE_CTL_VERSION, 0, 8, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS && info == 4,
		             "the version reports four bytes", (long)info, 4, &bad);
		sched_expect(g_ioc_out[0] == 2 && g_ioc_out[1] == 1,
		             "version 2.1", g_ioc_out[0], 2, &bad);
		sched_expect(ctl_call(&reg, CORE_CTL_VERSION, 0, 3, 0, &info) ==
		             CORE_ST_INVALID_PARAM, "and needs four bytes of room",
		             1, 1, &bad);

		st = ctl_call(&reg, CORE_CTL_GENERATION, 0, 4, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS && rd32_test(g_ioc_out) == 1,
		             "the generation counter", (long)rd32_test(g_ioc_out),
		             1, &bad);

		sched_expect(ctl_call(&reg, CORE_CTL_RESERVED_802, 0x40, 0x40, 0,
		                      &info) == CORE_ST_SUCCESS,
		             "fn 0x802 is accepted", 1, 1, &bad);
		sched_expect(info == 0x40, "and reports 0x40 bytes", (long)info,
		             0x40, &bad);
		sched_expect(ctl_call(&reg, CORE_CTL_RESERVED_803, 0x40, 0x40, 0,
		                      &info) == CORE_ST_SUCCESS,
		             "so is fn 0x803", 1, 1, &bad);
		sched_expect(ctl_call(&reg, CORE_CTL_RESERVED_802, 0x20, 0x40, 0,
		                      &info) == CORE_ST_INVALID_PARAM,
		             "but only at exactly 0x40", 1, 1, &bad);

		/* validated, then refused anyway */
		sched_expect(ctl_call(&reg, CORE_CTL_UNIMPLEMENTED, 4, 4, 0,
		                      &info) == CORE_ST_NOT_SUPPORTED,
		             "fn 0x814 validates then refuses", 1, 1, &bad);
		sched_expect(ctl_call(&reg, CORE_CTL_UNIMPLEMENTED, 2, 4, 0,
		                      &info) == CORE_ST_INVALID_PARAM,
		             "and rejects bad lengths first", 1, 1, &bad);

		sched_expect(ctl_call(&reg, 0x7FF, 0, 0, 0, &info) ==
		             CORE_ST_INVALID_PARAM,
		             "an unknown low code falls into the forward path", 1,
		             1, &bad);
		groups++;
	}

	/* ---- 8. the global button map, get and set --------------------- */
	{
		core_device_entry d1;
		core_state cs1;

		core_registry_init(&reg);
		core_notify_init(&reg.notify, ctl_claim, ctl_deliver, ctl_abort, 0);
		ctl_reset_log();
		core_init(&cs1, 0, 0);
		d1.handle = 1; d1.cs = &cs1; d1.sched = 0; d1.live = 0;
		core_registry_add(&reg, &d1);

		/* read it back with no input */
		st = ctl_call(&reg, CORE_CTL_BUTTON_MAP, 0, 16, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS && info == 16,
		             "sixteen entries come back", (long)info, 16, &bad);
		sched_expect(g_ioc_out[0] == 0 && g_ioc_out[1] == 3,
		             "the shipped mapping", g_ioc_out[1], 3, &bad);

		/* set entry 0 to 5, and try an out-of-range 14 on entry 1 */
		for (i = 0; i < 16; i++) {
			g_ioc_in[i] = (u8)(reg.button_map[i]);
		}
		g_ioc_in[0] = 5;
		g_ioc_in[1] = 14;           /* rejected: must be below 14 */
		st = ctl_call(&reg, CORE_CTL_BUTTON_MAP, 16, 16, 0, &info);
		sched_expect(reg.button_map[0] == 5, "entry 0 was set",
		             reg.button_map[0], 5, &bad);
		sched_expect(reg.button_map[1] == 3, "entry 1 was left alone",
		             reg.button_map[1], 3, &bad);
		sched_expect(g_ioc_out[0] == 0,
		             "and the OLD value came back", g_ioc_out[0], 0, &bad);
		sched_expect(cs1.button_map[0] == 5,
		             "the map was pushed to the adapter",
		             cs1.button_map[0], 5, &bad);

		/* no length validation at all: a zero-length call is legal */
		sched_expect(ctl_call(&reg, CORE_CTL_BUTTON_MAP, 0, 0, 0, &info) ==
		             CORE_ST_SUCCESS, "a zero-length call is accepted", 1,
		             1, &bad);
		sched_expect(info == 0, "and reports nothing", (long)info, 0, &bad);
		/* a short buffer stops at the shorter of the two */
		ctl_call(&reg, CORE_CTL_BUTTON_MAP, 0, 4, 0, &info);
		sched_expect(info == 4, "a four-byte read gives four", (long)info,
		             4, &bad);
		groups++;
	}

	/* ---- 9. the reports switch sweeps every adapter ---------------- */
	{
		core_device_entry d1, d2;
		core_state cs1, cs2;

		core_registry_init(&reg);
		core_notify_init(&reg.notify, ctl_claim, ctl_deliver, ctl_abort, 0);
		ctl_reset_log();
		core_init(&cs1, hid_sink, 0);
		core_init(&cs2, hid_sink, 0);
		cs1.instance_id = 100;
		cs2.instance_id = 200;
		d1.handle = 1; d1.cs = &cs1; d1.sched = 0; d1.live = 0;
		d2.handle = 2; d2.cs = &cs2; d2.sched = 0; d2.live = 0;
		core_registry_add(&reg, &d1);
		core_registry_add(&reg, &d2);

		g_hidcount = 0;
		wr32_test(g_ioc_in, 1);
		st = ctl_call(&reg, CORE_CTL_SET_REPORTS, 4, 0, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS, "the switch is accepted",
		             (long)st, 0, &bad);
		sched_expect(reg.reports_enabled == 1, "and recorded",
		             reg.reports_enabled, 1, &bad);
		sched_expect(g_hidcount == 2, "both adapters resubmitted",
		             g_hidcount, 2, &bad);
		sched_expect(cs1.virtual_mode == 3 && cs2.virtual_mode == 3,
		             "and both entered virtual mode", cs1.virtual_mode, 3,
		             &bad);
		/* each parks its stick at its own instance number */
		sched_expect(g_hidlog[0].data[2] == (100 >> 4),
		             "the first reports its own identity",
		             g_hidlog[0].data[2], 100 >> 4, &bad);
		sched_expect(g_hidlog[1].data[2] == (200 >> 4),
		             "and the second reports a different one",
		             g_hidlog[1].data[2], 200 >> 4, &bad);

		sched_expect(ctl_call(&reg, CORE_CTL_SET_REPORTS, 2, 0, 0, &info) ==
		             CORE_ST_INVALID_PARAM, "a short input is rejected", 1,
		             1, &bad);
		groups++;
	}

	/* ---- 10. enumerate, look up, and forward ----------------------- */
	{
		core_device_entry d1, d2;
		core_state cs1, cs2;
		core_sched sch1;

		core_registry_init(&reg);
		core_notify_init(&reg.notify, ctl_claim, ctl_deliver, ctl_abort, 0);
		ctl_reset_log();
		core_init(&cs1, 0, 0);
		core_init(&cs2, 0, 0);
		core_sched_init(&sch1, sched_test_alloc, sched_test_free, 0);
		cs1.instance_id = 137;
		cs2.instance_id = 642;
		d1.handle = 0xAAAA; d1.cs = &cs1; d1.sched = &sch1; d1.live = 0;
		d2.handle = 0xBBBB; d2.cs = &cs2; d2.sched = 0;     d2.live = 0;
		core_registry_add(&reg, &d1);
		core_registry_add(&reg, &d2);
		core_registry_set_live(&reg, &d1, 1);
		core_registry_set_live(&reg, &d2, 1);

		/* walk from the start */
		wr32_test(g_ioc_in, 0);
		st = ctl_call(&reg, CORE_CTL_ENUM_DEVICES, 4, 8, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS &&
		             rd32_test(g_ioc_out) == 0xAAAA, "the first adapter",
		             (long)rd32_test(g_ioc_out), 0xAAAA, &bad);
		sched_expect(rd32_test(g_ioc_out + 4) == 2, "and the live count",
		             (long)rd32_test(g_ioc_out + 4), 2, &bad);

		wr32_test(g_ioc_in, 0xAAAA);
		ctl_call(&reg, CORE_CTL_ENUM_DEVICES, 4, 8, 0, &info);
		sched_expect(rd32_test(g_ioc_out) == 0xBBBB, "then the second",
		             (long)rd32_test(g_ioc_out), 0xBBBB, &bad);

		wr32_test(g_ioc_in, 0xBBBB);
		ctl_call(&reg, CORE_CTL_ENUM_DEVICES, 4, 8, 0, &info);
		sched_expect(rd32_test(g_ioc_out) == 0, "then the end",
		             (long)rd32_test(g_ioc_out), 0, &bad);

		wr32_test(g_ioc_in, 0x9999);
		sched_expect(ctl_call(&reg, CORE_CTL_ENUM_DEVICES, 4, 8, 0,
		                      &info) == CORE_ST_NO_SUCH_DEVICE,
		             "a stale handle is reported", 1, 1, &bad);

		/* look up by instance number */
		wr32_test(g_ioc_in, 642);
		st = ctl_call(&reg, CORE_CTL_LOOKUP_DEVICE, 4, 4, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS &&
		             rd32_test(g_ioc_out) == 0xBBBB,
		             "an instance number resolves to its handle",
		             (long)rd32_test(g_ioc_out), 0xBBBB, &bad);
		wr32_test(g_ioc_in, 999);
		sched_expect(ctl_call(&reg, CORE_CTL_LOOKUP_DEVICE, 4, 4, 0,
		                      &info) == CORE_ST_NO_SUCH_DEVICE,
		             "an unknown one does not", 1, 1, &bad);

		/* forward a per-device request: handle, then that surface's input */
		cs1.stick_clip = 88;
		wr32_test(g_ioc_in, 0xAAAA);
		st = ctl_call(&reg, CORE_IOC_STICK_CLIP, 4, 4, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS &&
		             rd32_test(g_ioc_out) == 88,
		             "a forwarded read reaches the right adapter",
		             (long)rd32_test(g_ioc_out), 88, &bad);

		wr32_test(g_ioc_in, 0xBBBB);
		wr32_test(g_ioc_in + 4, 55);
		st = ctl_call(&reg, CORE_IOC_STICK_CLIP, 8, 0, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS && cs2.stick_clip == 55,
		             "and a forwarded write does too", cs2.stick_clip, 55,
		             &bad);
		sched_expect(cs1.stick_clip == 88, "without touching the other",
		             cs1.stick_clip, 88, &bad);

		wr32_test(g_ioc_in, 0xDEAD);
		sched_expect(ctl_call(&reg, CORE_IOC_STICK_CLIP, 8, 0, 0, &info) ==
		             CORE_ST_NO_SUCH_DEVICE,
		             "forwarding to a stranger is refused", 1, 1, &bad);
		sched_expect(ctl_call(&reg, CORE_IOC_STICK_CLIP, 3, 0, 0, &info) ==
		             CORE_ST_INVALID_PARAM,
		             "and so is one with no room for a handle", 1, 1,
		             &bad);
		core_sched_unload(&sch1);
		groups++;
	}

	/* ---- 11. waiting through the dispatcher ------------------------ */
	{
		core_device_entry d1;
		core_state cs1;

		core_registry_init(&reg);
		core_notify_init(&reg.notify, ctl_claim, ctl_deliver, ctl_abort, 0);
		ctl_reset_log();
		core_init(&cs1, 0, 0);
		d1.handle = 1; d1.cs = &cs1; d1.sched = 0; d1.live = 0;

		/* with no adapter present there will never be an event */
		sched_expect(ctl_call(&reg, CORE_CTL_WAIT_NOTIFY, 0,
		                      CORE_NOTIFY_BYTES, &w[0], &info) ==
		             CORE_ST_DELETE_PENDING,
		             "waiting with no adapter is refused", 1, 1, &bad);

		core_registry_add(&reg, &d1);
		core_registry_set_live(&reg, &d1, 1);
		reg.notify.count = 0;           /* drop the arrival event */

		w[0].request = &w[0];
		sched_expect(ctl_call(&reg, CORE_CTL_WAIT_NOTIFY, 0,
		                      CORE_NOTIFY_BYTES, &w[0], &info) ==
		             CORE_ST_PENDING, "with nothing queued it parks", 1, 1,
		             &bad);

		core_notify_post(&reg.notify, CORE_EVENT_FAULT, 10, 0);
		sched_expect(g_ctl_delcount == 1, "and is served on a post",
		             g_ctl_delcount, 1, &bad);

		/* an event already waiting is served without parking */
		core_notify_post(&reg.notify, CORE_EVENT_DEBUG, 1, 2);
		w[1].request = &w[1];
		st = ctl_call(&reg, CORE_CTL_WAIT_NOTIFY, 0, CORE_NOTIFY_BYTES,
		              &w[1], &info);
		sched_expect(st == CORE_ST_SUCCESS && info == CORE_NOTIFY_BYTES,
		             "a queued event is served at once", (long)info,
		             CORE_NOTIFY_BYTES, &bad);

		sched_expect(ctl_call(&reg, CORE_CTL_WAIT_NOTIFY, 0, 8, &w[2],
		                      &info) == CORE_ST_INVALID_PARAM,
		             "the output must be twelve bytes", 1, 1, &bad);
		groups++;
	}

	hlog("Control device surface : %s (%d groups)\n", bad ? "FAIL" : "ok",
	     groups);
	return bad;
}

/* ------------------------------------------------------------------ */
/* the fake USB bus and the two kernel calls the transport reaches      */
/* ------------------------------------------------------------------ */

/* What the last submitted transfer looked like, and what to answer with. */
static ADAPTOID_SETUP g_urb_setup;
static ULONG          g_urb_len;
static int            g_urb_count;
static NTSTATUS       g_urb_ret = STATUS_PENDING;
static int            g_urb_autocomplete;

/* Completed IRPs, so a test can see what a failure path answered. */
static PIRP     g_irp_last;
static NTSTATUS g_irp_status;
static int      g_irp_count;

void IoCompleteRequest(PIRP Irp, CHAR PriorityBoost)
{
	(void)PriorityBoost;
	g_irp_last   = Irp;
	g_irp_status = Irp->IoStatus.Status;
	g_irp_count++;
}

NTSTATUS KeWaitForSingleObject(PVOID Object, ULONG WaitReason,
                               ULONG WaitMode, BOOLEAN Alertable,
                               PVOID Timeout)
{
	PKEVENT e = (PKEVENT)Object;

	(void)WaitReason; (void)WaitMode; (void)Alertable; (void)Timeout;
	/*
	 * There is no other thread here to signal it. An unsignalled event
	 * means the driver would have blocked forever, so say so loudly rather
	 * than returning and letting the test pass.
	 */
	if (!e->Signalled) {
		hlog("  FAIL kernel KeWaitForSingleObject would block forever\n");
		g_wait_would_block++;
	}
	return STATUS_SUCCESS;
}

/* The OS edge of the vendor transport, replacing the URB build. */
NTSTATUS AdaptoidVendorSubmitUrb(PADAPTOID_DEVEXT DevExt,
                                 const ADAPTOID_SETUP *Setup,
                                 ULONG TransferLength, PVOID TransferBuffer)
{
	(void)TransferBuffer;
	g_urb_setup = *Setup;
	g_urb_len   = TransferLength;
	g_urb_count++;
	/*
	 * AUTO-COMPLETION IS OPT-IN. The transport tests drive
	 * AdaptoidVendorComplete by hand so they can watch the slot change
	 * state; the PnP tests cannot, because AdaptoidStartDevice WAITS for
	 * a reply inside the dispatcher. Without this the harness reports
	 * "KeWaitForSingleObject would block forever", which is exactly what
	 * it should say and exactly how this was found.
	 */
	if (g_urb_autocomplete) {
		AdaptoidVendorComplete(DevExt, STATUS_SUCCESS, TransferLength);
		return STATUS_SUCCESS;
	}
	(void)DevExt;
	return g_urb_ret;
}

/* ------------------------------------------------------------------ */
/* the remove lock and the vendor transport                            */
/* ------------------------------------------------------------------ */

static int g_cb_calls;
static int g_cb_return;

static int wdm_test_callback(PADAPTOID_DEVEXT DevExt)
{
	(void)DevExt;
	g_cb_calls++;
	return g_cb_return;
}

static void wdm_reset(PADAPTOID_DEVEXT dx)
{
	int i;
	u8 *p = (u8 *)dx;

	for (i = 0; i < (int)sizeof(*dx); i++) {
		p[i] = 0;
	}
	core_init(&dx->Core, 0, 0);
	AdaptoidDevExtInit(dx);
	/* The transport tests drive the slot directly, so start it free. */
	dx->PollStopMask = 0;
	g_urb_count        = 0;
	g_urb_len          = 0;
	g_urb_ret          = STATUS_PENDING;
	g_urb_autocomplete = 0;
	g_irp_count        = 0;
	g_irp_status       = 0;
	g_cb_calls         = 0;
	g_cb_return        = 0;
	g_wait_would_block = 0;
}

static int test_wdm_transport(void)
{
	int bad    = 0;
	int groups = 0;
	static ADAPTOID_DEVEXT dx;
	ADAPTOID_SETUP setup;
	IRP irp;
	NTSTATUS st;

	/* ---- 1. the remove lock counts, and the gate ------------------- */
	{
		wdm_reset(&dx);

		sched_expect(dx.RemoveLockA.IoCount == 1,
		             "a fresh lock starts at one",
		             dx.RemoveLockA.IoCount, 1, &bad);
		sched_expect(AdaptoidLockAcquire(&dx.RemoveLockA) == STATUS_SUCCESS,
		             "acquire succeeds", 1, 1, &bad);
		sched_expect(dx.RemoveLockA.IoCount == 2, "and counts up",
		             dx.RemoveLockA.IoCount, 2, &bad);
		AdaptoidLockRelease(&dx.RemoveLockA);
		sched_expect(dx.RemoveLockA.IoCount == 1, "release counts down",
		             dx.RemoveLockA.IoCount, 1, &bad);
		sched_expect(dx.RemoveLockA.RemoveEvent.Signalled == 0,
		             "without signalling, because of the initial one",
		             dx.RemoveLockA.RemoveEvent.Signalled, 0, &bad);

		/* once Removed, every further acquire is refused and leaves the
		 * count where it found it */
		dx.RemoveLockA.Removed = 1;
		sched_expect(AdaptoidLockAcquire(&dx.RemoveLockA) ==
		             STATUS_DELETE_PENDING,
		             "a removing device refuses new references", 1, 1,
		             &bad);
		sched_expect(dx.RemoveLockA.IoCount == 1,
		             "and the refusal does not leak a count",
		             dx.RemoveLockA.IoCount, 1, &bad);
		groups++;
	}

	/* ---- 2. release-and-wait drains to zero ------------------------ */
	{
		wdm_reset(&dx);

		/*
		 * The caller of ReleaseAndWait must still HOLD its own reference:
		 * the call drops that one and the initial one from Init, so the
		 * count goes 2 -> 1 -> 0. That is how the remove IRP's own
		 * acquire is accounted for.
		 */
		AdaptoidLockAcquire(&dx.RemoveLockA);
		sched_expect(dx.RemoveLockA.IoCount == 2, "two references held",
		             dx.RemoveLockA.IoCount, 2, &bad);
		AdaptoidLockReleaseAndWait(&dx.RemoveLockA);

		sched_expect(dx.RemoveLockA.IoCount == 0, "the count reached zero",
		             dx.RemoveLockA.IoCount, 0, &bad);
		sched_expect(dx.RemoveLockA.RemoveEvent.Signalled != 0,
		             "and the event was signalled",
		             dx.RemoveLockA.RemoveEvent.Signalled != 0, 1, &bad);
		sched_expect(g_wait_would_block == 0, "so the wait did not block",
		             g_wait_would_block, 0, &bad);
		groups++;
	}

	/* ---- 3. the slot admits one claimant at a time ----------------- */
	{
		wdm_reset(&dx);

		sched_expect(AdaptoidVendorTryClaim(&dx) == 1, "the slot is free",
		             1, 1, &bad);
		sched_expect(dx.Vendor.State == ADAPTOID_SLOT_CLAIMED, "and claimed",
		             dx.Vendor.State, ADAPTOID_SLOT_CLAIMED, &bad);
		sched_expect(AdaptoidVendorTryClaim(&dx) == 0,
		             "a second claimant is refused", 0, 0, &bad);
		sched_expect(AdaptoidVendorClaimForIrp(&dx, &irp) == 0,
		             "including one bringing an IRP", 0, 0, &bad);
		groups++;
	}

	/* ---- 4. a transfer goes out, and the lock is held across it ---- */
	{
		wdm_reset(&dx);
		AdaptoidVendorTryClaim(&dx);

		setup.bmRequestType = ADAPTOID_VENDOR_IN;
		setup.bRequest      = 0x21;
		setup.wValue        = 0x1234;
		setup.wIndex        = 0x5678;
		st = AdaptoidVendorSend(&dx, &setup, 4, 0, wdm_test_callback);

		sched_expect(st == STATUS_PENDING, "the transfer was submitted",
		             (long)st, STATUS_PENDING, &bad);
		sched_expect(g_urb_count == 1, "once", g_urb_count, 1, &bad);
		sched_expect(g_urb_setup.bRequest == 0x21, "carrying the request",
		             g_urb_setup.bRequest, 0x21, &bad);
		sched_expect(g_urb_len == 4, "and the length", (long)g_urb_len, 4,
		             &bad);
		sched_expect(dx.Vendor.State == ADAPTOID_SLOT_IN_FLIGHT,
		             "the slot is in flight", dx.Vendor.State,
		             ADAPTOID_SLOT_IN_FLIGHT, &bad);
		sched_expect(dx.RemoveLockB.IoCount == 2,
		             "and the remove lock is held across it",
		             dx.RemoveLockB.IoCount, 2, &bad);

		/* completing it hands the slot back and drops the lock */
		AdaptoidVendorComplete(&dx, STATUS_SUCCESS, 4);
		sched_expect(g_cb_calls == 1, "the callback ran", g_cb_calls, 1,
		             &bad);
		sched_expect(dx.Vendor.State == ADAPTOID_SLOT_FREE,
		             "the slot went back to free", dx.Vendor.State,
		             ADAPTOID_SLOT_FREE, &bad);
		sched_expect(dx.RemoveLockB.IoCount == 1,
		             "and the lock came back", dx.RemoveLockB.IoCount, 1,
		             &bad);
		groups++;
	}

	/* ---- 5. a callback that takes the slot again keeps it ---------- */
	{
		wdm_reset(&dx);
		AdaptoidVendorTryClaim(&dx);
		setup.bmRequestType = ADAPTOID_VENDOR_OUT;
		AdaptoidVendorSend(&dx, &setup, 0, 0, wdm_test_callback);

		g_cb_return = 1;            /* "I resubmitted" */
		AdaptoidVendorComplete(&dx, STATUS_SUCCESS, 0);
		sched_expect(dx.Vendor.State == ADAPTOID_SLOT_CLAIMED,
		             "the slot stays claimed for the chain",
		             dx.Vendor.State, ADAPTOID_SLOT_CLAIMED, &bad);
		groups++;
	}

	/* ---- 6. a failed transfer suppresses the callback -------------- */
	{
		wdm_reset(&dx);
		AdaptoidVendorTryClaim(&dx);
		setup.bmRequestType = ADAPTOID_VENDOR_IN;
		AdaptoidVendorSend(&dx, &setup, 4, 0, wdm_test_callback);

		AdaptoidVendorComplete(&dx, STATUS_UNSUCCESSFUL, 0);
		sched_expect(g_cb_calls == 0,
		             "a failed transfer does not run the callback",
		             g_cb_calls, 0, &bad);
		sched_expect(dx.Vendor.State == ADAPTOID_SLOT_FREE,
		             "but the slot is still released", dx.Vendor.State,
		             ADAPTOID_SLOT_FREE, &bad);
		sched_expect(dx.RemoveLockB.IoCount == 1, "and the lock too",
		             dx.RemoveLockB.IoCount, 1, &bad);
		groups++;
	}

	/* ---- 7. a waiting IRP inherits the transfer's result ----------- */
	{
		wdm_reset(&dx);
		irp.IoStatus.Status = 0;
		irp.IoStatus.Information = 0;
		sched_expect(AdaptoidVendorClaimForIrp(&dx, &irp) == 1,
		             "claimed on behalf of a request", 1, 1, &bad);
		setup.bmRequestType = ADAPTOID_VENDOR_IN;
		AdaptoidVendorSend(&dx, &setup, 8, 0, 0);

		AdaptoidVendorComplete(&dx, STATUS_SUCCESS, 8);
		sched_expect(g_irp_count == 1, "the request was completed",
		             g_irp_count, 1, &bad);
		sched_expect(irp.IoStatus.Information == 8, "with the transfer length",
		             (long)irp.IoStatus.Information, 8, &bad);
		sched_expect(irp.IoStatus.Status == STATUS_SUCCESS, "and its status",
		             (long)irp.IoStatus.Status, 0, &bad);
		groups++;
	}

	/* ---- 8. DEFECT 15: a rejected request must not leak ------------ */
	{
		/*
		 * bmRequestType comes from the caller on the raw passthrough
		 * IOCTL, so anything other than 0x40 or 0xC0 is untrusted input
		 * that has to be refused - without leaking the reference taken
		 * on entry, which is what the original does.
		 */
		static const u8 BAD[] = {0x00, 0x21, 0x80, 0xC1, 0x41, 0xFF};
		int k;
		LONG before;

		wdm_reset(&dx);
		before = dx.RemoveLockB.IoCount;

		for (k = 0; k < (int)(sizeof(BAD) / sizeof(BAD[0])); k++) {
			irp.IoStatus.Status = 0;
			AdaptoidVendorClaimForIrp(&dx, &irp);
			setup.bmRequestType = BAD[k];
			st = AdaptoidVendorSend(&dx, &setup, 0, 0, 0);
			if (st != STATUS_INVALID_PARAMETER) {
				hlog("  FAIL wdm bmRequestType %02x gave %08lx, want "
				     "INVALID_PARAMETER\n", BAD[k], (unsigned long)st);
				bad++;
			}
		}
		sched_expect(dx.RemoveLockB.IoCount == before,
		             "six rejections leaked nothing",
		             dx.RemoveLockB.IoCount, before, &bad);
		sched_expect(dx.Vendor.State == ADAPTOID_SLOT_FREE,
		             "and the slot was given back", dx.Vendor.State,
		             ADAPTOID_SLOT_FREE, &bad);
		sched_expect(g_irp_count == 6, "each request was completed",
		             g_irp_count, 6, &bad);
		sched_expect(g_urb_count == 0, "and none reached the wire",
		             g_urb_count, 0, &bad);

		/* the two legal values still work */
		setup.bmRequestType = ADAPTOID_VENDOR_OUT;
		AdaptoidVendorTryClaim(&dx);
		sched_expect(AdaptoidVendorSend(&dx, &setup, 0, 0, 0) ==
		             STATUS_PENDING, "0x40 is accepted", 1, 1, &bad);
		AdaptoidVendorComplete(&dx, STATUS_SUCCESS, 0);
		setup.bmRequestType = ADAPTOID_VENDOR_IN;
		AdaptoidVendorTryClaim(&dx);
		sched_expect(AdaptoidVendorSend(&dx, &setup, 4, 0, 0) ==
		             STATUS_PENDING, "and so is 0xC0", 1, 1, &bad);
		AdaptoidVendorComplete(&dx, STATUS_SUCCESS, 4);
		sched_expect(dx.RemoveLockB.IoCount == before,
		             "with the lock still balanced",
		             dx.RemoveLockB.IoCount, before, &bad);
		groups++;
	}

	/* ---- 9. submitting without claiming is refused ----------------- */
	{
		wdm_reset(&dx);
		setup.bmRequestType = ADAPTOID_VENDOR_IN;
		st = AdaptoidVendorSend(&dx, &setup, 4, 0, 0);
		sched_expect(st == STATUS_DEVICE_BUSY,
		             "an unclaimed slot refuses the transfer", (long)st,
		             STATUS_DEVICE_BUSY, &bad);
		sched_expect(g_urb_count == 0, "nothing went out", g_urb_count, 0,
		             &bad);
		sched_expect(dx.RemoveLockB.IoCount == 1, "and nothing leaked",
		             dx.RemoveLockB.IoCount, 1, &bad);

		/* a completion for a transfer we never made is dropped */
		AdaptoidVendorComplete(&dx, STATUS_SUCCESS, 0);
		sched_expect(dx.Vendor.State == ADAPTOID_SLOT_FREE,
		             "a spurious completion does not corrupt the slot",
		             dx.Vendor.State, ADAPTOID_SLOT_FREE, &bad);
		groups++;
	}

	/* ---- a submit that fails must put everything back --------------
	 *
	 * THE BUG THIS EXISTS FOR, and it needed Driver Verifier's
	 * low-resources injection to find on hardware even though it was
	 * always reachable from here: g_urb_ret has been a knob the whole
	 * time and nothing ever turned it.
	 *
	 * AdaptoidVendorSend takes RemoveLockB on entry and moves the slot to
	 * IN_FLIGHT, then hands off to AdaptoidVendorSubmitUrb. That routine
	 * frees only what IT allocated, so when its allocation fails the
	 * caller's lock reference and the slot are both abandoned - no
	 * completion will ever run to undo them.
	 *
	 * The symptom is not a crash. RemoveLockB never reaches zero, so
	 * AdaptoidLockReleaseAndWait waits forever, the driver cannot unload,
	 * and the device sits half torn down: present to the driver, gone
	 * from Windows. Measured on the live driver as RemoveLockB.IoCount
	 * stuck at 1 with Removed already set.
	 */
	{
		wdm_reset(&dx);
		g_urb_count = 0;
		g_urb_ret   = STATUS_INSUFFICIENT_RESOURCES;

		sched_expect(AdaptoidVendorTryClaim(&dx) != 0,
		             "the slot is claimed", 1, 1, &bad);

		setup.bmRequestType = ADAPTOID_VENDOR_OUT;
		setup.bRequest      = 0x72;
		setup.wValue        = 0;
		setup.wIndex        = 0;
		st = AdaptoidVendorSend(&dx, &setup, 0, 0, 0);

		sched_expect(!NT_SUCCESS(st),
		             "a failed submit is reported", 1, 1, &bad);
		sched_expect(dx.RemoveLockB.IoCount == 1,
		             "the remove lock is given back",
		             dx.RemoveLockB.IoCount, 1, &bad);
		sched_expect(dx.Vendor.State == ADAPTOID_SLOT_FREE,
		             "and the slot is free again",
		             dx.Vendor.State, ADAPTOID_SLOT_FREE, &bad);

		/* A second transfer must still be possible. */
		g_urb_ret = STATUS_PENDING;
		sched_expect(AdaptoidVendorTryClaim(&dx) != 0,
		             "so the next transfer can claim it", 1, 1, &bad);
		groups++;
	}

	hlog("WDM lock and transport : %s (%d groups)\n", bad ? "FAIL" : "ok",
	     groups);
	return bad;
}

/* ------------------------------------------------------------------ */
/* the fake device stack                                               */
/* ------------------------------------------------------------------ */

/*
 * Every stage-three call the PnP dispatcher makes is recorded here in
 * order. That ordering IS the thing worth testing: the original tears down
 * before passing REMOVE and STOP down, and passes START and
 * QUERY_CAPABILITIES down before acting, and getting those backwards would
 * have the poll loop touching USB resources the bus driver has reclaimed.
 */
#define PNPLOG 32
static const char *g_pnp_log[PNPLOG];
static int         g_pnp_count;
static PADAPTOID_DEVEXT g_pnp_devext;

static void pnp_note(const char *what)
{
	if (g_pnp_count < PNPLOG) {
		g_pnp_log[g_pnp_count] = what;
	}
	g_pnp_count++;
}

/* Where in the log a call appears, or -1. */
static int pnp_at(const char *what)
{
	int i;

	/* By CONTENT, not by pointer: the literals live in two translation
	 * units and nothing guarantees the compiler pools them. */
	for (i = 0; i < g_pnp_count && i < PNPLOG; i++) {
		const char *a = g_pnp_log[i];
		const char *b = what;

		while (*a != 0 && *a == *b) {
			a++;
			b++;
		}
		if (*a == *b) {
			return i;
		}
	}
	return -1;
}

static NTSTATUS g_lower_status = STATUS_SUCCESS;
static int      g_lower_calls;

NTSTATUS IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	(void)DeviceObject;
	pnp_note("passdown");
	g_lower_calls++;
	Irp->IoStatus.Status = g_lower_status;
	return g_lower_status;
}

void IoCopyCurrentIrpStackLocationToNext(PIRP Irp) { (void)Irp; }
void IoSkipCurrentIrpStackLocation(PIRP Irp)       { (void)Irp; }

/*
 * The variable itself lives in wdm.c, shared by both builds. Only the
 * decision is OS-specific, so only the decision is stubbed.
 */
void AdaptoidInitPoolType(void)
{
	AdaptoidPoolType = NonPagedPoolNx;
}

PADAPTOID_DEVEXT AdaptoidDevExtOf(PDEVICE_OBJECT DeviceObject)
{
	(void)DeviceObject;
	return g_pnp_devext;
}

static NTSTATUS g_descriptor_status = STATUS_SUCCESS;
static NTSTATUS g_selectcfg_status  = STATUS_SUCCESS;

NTSTATUS AdaptoidFetchDeviceDescriptor(PADAPTOID_DEVEXT DevExt)
{ (void)DevExt; pnp_note("descriptor"); return g_descriptor_status; }

NTSTATUS AdaptoidSelectConfiguration(PADAPTOID_DEVEXT DevExt)
{ (void)DevExt; pnp_note("selectcfg"); return g_selectcfg_status; }

void AdaptoidQuiesceIo(PADAPTOID_DEVEXT DevExt)
{ (void)DevExt; pnp_note("quiesce"); }

void AdaptoidUnconfigureDevice(PADAPTOID_DEVEXT DevExt)
{ (void)DevExt; pnp_note("unconfigure"); }

void AdaptoidAbortPipes(PADAPTOID_DEVEXT DevExt)
{ (void)DevExt; pnp_note("abortpipes"); }

void AdaptoidFreeDeviceResources(PADAPTOID_DEVEXT DevExt)
{ (void)DevExt; pnp_note("freeres"); }

void AdaptoidEnableInterface(PADAPTOID_DEVEXT DevExt)
{ (void)DevExt; pnp_note("enableiface"); }

void AdaptoidRegistryRemove(PADAPTOID_DEVEXT DevExt)
{ (void)DevExt; pnp_note("unregister"); }

void AdaptoidSetCompletionRoutine(PIRP Irp, PVOID Event)
{
	/* Nothing below will signal it, so signal it here - the dispatcher
	 * only waits when the lower driver returned STATUS_PENDING, and the
	 * fake one never does. */
	(void)Irp;
	((PKEVENT)Event)->Signalled = 1;
}

/* ------------------------------------------------------------------ */
/* the kernel edges stage five reaches                                 */
/* ------------------------------------------------------------------ */

/*
 * Power. The three calls are recorded rather than made, because what matters
 * about this layer is the ORDER: PoStartNextPowerIrp must precede
 * PoCallDriver on every path, and the device IRP a system IRP asks for must
 * complete before the system IRP does.
 */
static char  g_power_log[512];
static ULONG g_power_log_len;

static void power_note(const char *what)
{
	ULONG n = 0;

	while (what[n] != 0) {
		n++;
	}
	if (g_power_log_len + n + 2 >= sizeof(g_power_log)) {
		return;
	}
	if (g_power_log_len != 0) {
		g_power_log[g_power_log_len++] = ' ';
	}
	memcpy(g_power_log + g_power_log_len, what, n);
	g_power_log_len += n;
	g_power_log[g_power_log_len] = 0;
}

static void power_reset(void)
{
	g_power_log[0]  = 0;
	g_power_log_len = 0;
}

/* The state the last PoRequestPowerIrp asked for, and its completion. */
static ULONG                   g_power_requested;
static int                     g_power_requests;
static PREQUEST_POWER_COMPLETE g_power_complete;
static PVOID                   g_power_context;
static PDEVICE_OBJECT          g_power_device;

/* Where a passed-down power IRP went, and what completion was attached. */
static PIO_COMPLETION_ROUTINE g_power_completion;
static PVOID                  g_power_completion_ctx;

void PoStartNextPowerIrp(PIRP Irp)
{
	(void)Irp;
	power_note("next");
	/* The triage tests watch the PnP log rather than this one, and both
	 * are recording the same call. */
	pnp_note("nextpower");
}

NTSTATUS PoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	(void)DeviceObject;
	power_note("down");
	/*
	 * The fake stack completes synchronously, so a completion routine is
	 * run here - the real one would be called by the IRP machinery.
	 */
	if (g_power_completion != NULL) {
		PIO_COMPLETION_ROUTINE r = g_power_completion;
		PVOID                  c = g_power_completion_ctx;

		g_power_completion     = NULL;
		g_power_completion_ctx = NULL;
		r(DeviceObject, Irp, c);
		power_note("completed");
	}
	return STATUS_SUCCESS;
}

NTSTATUS PoRequestPowerIrp(PDEVICE_OBJECT DeviceObject, UCHAR MinorFunction,
                           POWER_STATE PowerState,
                           PREQUEST_POWER_COMPLETE Complete,
                           PVOID Context, PIRP *Irp)
{
	(void)MinorFunction;
	(void)Irp;
	power_note("request");
	g_power_requests++;
	g_power_requested = (ULONG)PowerState.DeviceState;
	g_power_complete  = Complete;
	g_power_context   = Context;
	g_power_device    = DeviceObject;
	return STATUS_PENDING;
}

/* Run the completion the last request registered, as the bus would. */
static void power_run_completion(void)
{
	PREQUEST_POWER_COMPLETE c = g_power_complete;

	g_power_complete = NULL;
	if (c != NULL) {
		IO_STATUS_BLOCK io;
		POWER_STATE     ps;

		io.Status      = STATUS_SUCCESS;
		io.Information = 0;
		ps.DeviceState = (DEVICE_POWER_STATE)g_power_requested;
		c(g_power_device, 0, ps, g_power_context, &io);
	}
}

void IoSetCompletionRoutine(PIRP Irp, PIO_COMPLETION_ROUTINE Routine,
                            PVOID Context, BOOLEAN OnSuccess,
                            BOOLEAN OnError, BOOLEAN OnCancel)
{
	(void)Irp;
	(void)OnSuccess;
	(void)OnError;
	(void)OnCancel;
	g_power_completion     = Routine;
	g_power_completion_ctx = Context;
}

/* Cancellation. Single-threaded, so the lock is a counter to assert on. */
static LONG g_cancel_lock_depth;

void IoSetCancelRoutine(PIRP Irp, PVOID Routine)
{
	Irp->CancelRoutine = Routine;
}

void IoAcquireCancelSpinLock(KIRQL *Irql)
{
	*Irql = 0;
	g_cancel_lock_depth++;
}

void IoReleaseCancelSpinLock(KIRQL Irql)
{
	(void)Irql;
	g_cancel_lock_depth--;
}

/* Timers and DPCs. Recorded, never fired on their own. */
void KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE Routine, PVOID Context)
{
	Dpc->Routine = Routine;
	Dpc->Context = Context;
	Dpc->Queued  = 0;
}

static LONGLONG g_timer_due;
static int      g_timer_arms;

void KeInitializeTimer(PKTIMER Timer) { Timer->Due = 0; }

BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
{
	BOOLEAN was = (BOOLEAN)(Timer->Due != 0);

	Timer->Due = (ULONGLONG)DueTime.QuadPart;
	g_timer_due = DueTime.QuadPart;
	g_timer_arms++;
	if (Dpc != NULL) {
		Dpc->Queued = 1;
	}
	return was;
}

BOOLEAN KeCancelTimer(PKTIMER Timer)
{
	BOOLEAN was = (BOOLEAN)(Timer->Due != 0);

	Timer->Due = 0;
	return was;
}

/* The fast mutex guarding the control-device singleton. */
static LONG g_mutex_depth;
static LONG g_mutex_max;

void ExInitializeFastMutex(PFAST_MUTEX Mutex) { Mutex->Held = 0; }

/*
 * The control-device lock is a KMUTEX in the driver, waited on through
 * KeWaitForSingleObject. Model it the same way the fast mutex is modelled:
 * a depth counter, so a lock held across a call that takes it again is
 * still caught.
 */
void KeInitializeMutex(PKMUTEX Mutex, ULONG Level)
{
	(void)Level;
	Mutex->Signalled = 1;           /* unheld == available */
}

LONG KeReleaseMutex(PKMUTEX Mutex, BOOLEAN Wait)
{
	(void)Wait;
	Mutex->Signalled = 1;
	g_mutex_depth--;
	return 0;
}

void ExAcquireFastMutex(PFAST_MUTEX Mutex)
{
	Mutex->Held++;
	g_mutex_depth++;
	if (g_mutex_depth > g_mutex_max) {
		g_mutex_max = g_mutex_depth;
	}
}

void ExReleaseFastMutex(PFAST_MUTEX Mutex)
{
	Mutex->Held--;
	g_mutex_depth--;
}

/* The control device object itself. One static extension is enough; the
 * driver only ever makes one. */
static DEVICE_OBJECT g_made_device;
/* The control-device extension carries the registry and the hundred-event
 * notification queue, so it is far larger than the original's 0x78 bytes. */
static UCHAR         g_made_ext[262144];
static int           g_devices_created;
static int           g_devices_deleted;
static int           g_links_created;
static int           g_links_deleted;
static int           g_create_device_fails;
static int           g_create_link_fails;

void RtlInitUnicodeString(PUNICODE_STRING Target, PCWSTR Source)
{
	USHORT n = 0;

	while (Source[n] != 0) {
		n++;
	}
	Target->Buffer        = (PWSTR)Source;
	Target->Length        = (USHORT)(n * sizeof(WCHAR));
	Target->MaximumLength = (USHORT)(Target->Length + sizeof(WCHAR));
}

NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG ExtensionSize,
                        PUNICODE_STRING Name, ULONG DeviceType,
                        ULONG Characteristics, BOOLEAN Exclusive,
                        PDEVICE_OBJECT *DeviceObject)
{
	(void)DriverObject;
	(void)Name;
	(void)DeviceType;
	(void)Characteristics;
	(void)Exclusive;

	if (g_create_device_fails) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	if (ExtensionSize > sizeof(g_made_ext)) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memset(&g_made_device, 0, sizeof(g_made_device));
	memset(g_made_ext, 0, ExtensionSize);
	g_made_device.DeviceExtension = g_made_ext;
	g_made_device.Flags           = DO_DEVICE_INITIALIZING;
	*DeviceObject = &g_made_device;
	g_devices_created++;
	return STATUS_SUCCESS;
}

void IoDeleteDevice(PDEVICE_OBJECT DeviceObject)
{
	(void)DeviceObject;
	g_devices_deleted++;
}

NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING Link, PUNICODE_STRING Target)
{
	(void)Link;
	(void)Target;
	if (g_create_link_fails) {
		return STATUS_UNSUCCESSFUL;
	}
	g_links_created++;
	return STATUS_SUCCESS;
}

NTSTATUS IoDeleteSymbolicLink(PUNICODE_STRING Link)
{
	(void)Link;
	g_links_deleted++;
	return STATUS_SUCCESS;
}

/*
 * The OS edge of a power request. Kept here rather than in wdm.c for the
 * same reason every other edge is: what the driver decides is testable, what
 * the bus does with it is not.
 */
NTSTATUS AdaptoidRequestPowerIrp(PADAPTOID_DEVEXT DevExt, ULONG State,
                                 PREQUEST_POWER_COMPLETE Complete)
{
	POWER_STATE ps;

	ps.DeviceState = (DEVICE_POWER_STATE)State;
	return PoRequestPowerIrp(DevExt->PhysicalDeviceObject, IRP_MN_SET_POWER,
	                         ps, Complete, DevExt, NULL);
}

/* ---- the wiring's OS edges ---------------------------------------- */

/* Whether DriverEntry installed AddDevice, and what it installed. */
static ADAPTOID_ADD_DEVICE g_add_device;
static int                 g_workitems;

void AdaptoidSetAddDevice(PDRIVER_OBJECT DriverObject,
                          ADAPTOID_ADD_DEVICE AddDevice)
{
	(void)DriverObject;
	g_add_device = AddDevice;
}

PVOID AdaptoidAllocateWorkItem(PDEVICE_OBJECT DeviceObject)
{
	static int slot;

	(void)DeviceObject;
	g_workitems++;
	slot++;
	return &slot;           /* a non-null token; nothing dereferences it */
}

void AdaptoidFreeWorkItem(PVOID WorkItem)
{
	if (WorkItem != NULL) {
		g_workitems--;
	}
}

/* The registry value AddDevice reads. Absent means "use the default". */
static int  g_reg_present;
static ULONG g_reg_value;

ULONG AdaptoidRegQueryDword(PCWSTR Name, ULONG Default)
{
	(void)Name;
	return g_reg_present ? g_reg_value : Default;
}

static int g_interfaces_registered;

NTSTATUS AdaptoidRegisterDeviceInterface(PADAPTOID_DEVEXT DevExt)
{
	(void)DevExt;
	g_interfaces_registered++;
	return STATUS_SUCCESS;
}

void RtlZeroMemory(PVOID Destination, ULONG_PTR Length)
{
	memset(Destination, 0, (size_t)Length);
}

void RtlCopyMemory(PVOID Destination, const void *Source, ULONG_PTR Length)
{
	memcpy(Destination, Source, (size_t)Length);
}

/* Which handler a triaged request reached. */
static const char *g_route_hit;

static NTSTATUS NTAPI hidclass_stub(PDEVICE_OBJECT d, PIRP Irp)
{ (void)d; (void)Irp; g_route_hit = "hidclass"; return STATUS_SUCCESS; }

NTSTATUS NTAPI AdaptoidChannelCreate(PDEVICE_OBJECT d, PIRP Irp)
{ (void)d; (void)Irp; g_route_hit = "private"; return STATUS_SUCCESS; }
NTSTATUS NTAPI AdaptoidChannelClose(PDEVICE_OBJECT d, PIRP Irp)
{ (void)d; (void)Irp; g_route_hit = "private"; return STATUS_SUCCESS; }
NTSTATUS NTAPI AdaptoidChannelIoctl(PDEVICE_OBJECT d, PIRP Irp)
{ (void)d; (void)Irp; g_route_hit = "private"; return STATUS_SUCCESS; }

/* ------------------------------------------------------------------ */
/* the dispatch triage and PnP                                         */
/* ------------------------------------------------------------------ */

/* Both remove locks back where wdm_reset left them. */
static int dx_lock_balanced(PADAPTOID_DEVEXT dx)
{
	return dx->RemoveLockA.IoCount == 1 && dx->RemoveLockB.IoCount == 1;
}

static ADAPTOID_CDO_EXT g_cdo_ext;
static ADAPTOID_DEVEXT g_hid_ext;
static DEVICE_OBJECT  g_cdo_dev;
static DEVICE_OBJECT  g_hid_dev;
static IO_STACK_LOCATION g_sp;
static FILE_OBJECT    g_file;
static WCHAR          g_name[4];

static void triage_setup(void)
{
	memset(&g_cdo_ext, 0, sizeof(g_cdo_ext));
	g_cdo_ext.Magic[0] = ADAPTOID_CDO_MAGIC0;
	g_cdo_ext.Magic[1] = ADAPTOID_CDO_MAGIC1;
	g_cdo_ext.Magic[2] = ADAPTOID_CDO_MAGIC2;
	/* A REAL extension, because the control handlers are now real code
	 * rather than route-marking stubs and they dereference it. */
	core_registry_init(&g_cdo_ext.Registry);
	core_cmd_channel_init(&g_cdo_ext.Channel);
	AdaptoidNotifyInit(&g_cdo_ext);
	g_cdo_dev.DeviceExtension = &g_cdo_ext;
	g_hid_dev.DeviceExtension = &g_hid_ext;

	AdaptoidSavedDispatch.Create        = hidclass_stub;
	AdaptoidSavedDispatch.Cleanup       = hidclass_stub;
	AdaptoidSavedDispatch.Close         = hidclass_stub;
	AdaptoidSavedDispatch.Read          = hidclass_stub;
	AdaptoidSavedDispatch.Write         = hidclass_stub;
	AdaptoidSavedDispatch.DeviceControl = hidclass_stub;
	AdaptoidSavedDispatch.Pnp           = hidclass_stub;
	AdaptoidSavedDispatch.Power         = hidclass_stub;

}

/* Build an IRP whose FileName is the two WCHARs given, or none at all. */
static void triage_irp(PIRP irp, int with_file, WCHAR c0, WCHAR c1,
                       USHORT len)
{
	g_name[0] = c0;
	g_name[1] = c1;
	g_file.FileName.Buffer = g_name;
	g_file.FileName.Length = len;
	g_sp.FileObject = with_file ? &g_file : NULL;
	g_sp.MajorFunction = 0;
	irp->CurrentStackLocation = &g_sp;
	irp->NextStackLocation    = &g_sp;
	irp->IoStatus.Status      = 0;
	irp->IoStatus.Information = 0;
}

static int test_triage_pnp(void)
{
	int bad    = 0;
	int groups = 0;
	IRP irp;

	/* ---- 1. the three routes ---------------------------------------- */
	{
		triage_setup();

		/* the magic wins regardless of the FileName */
		triage_irp(&irp, 1, L'\\', L'q', 4);
		sched_expect(AdaptoidRouteOf(&g_cdo_dev, &irp) ==
		             ADAPTOID_ROUTE_CONTROL,
		             "the control device is recognised by its magic", 1, 1,
		             &bad);

		/* a four-byte name whose second WCHAR is 'q' */
		sched_expect(AdaptoidRouteOf(&g_hid_dev, &irp) ==
		             ADAPTOID_ROUTE_PRIVATE,
		             "and the private channel by its suffix", 1, 1, &bad);

		/* ONLY Buffer[1] is tested - the first character is not looked at */
		triage_irp(&irp, 1, L'Z', L'q', 4);
		sched_expect(AdaptoidRouteOf(&g_hid_dev, &irp) ==
		             ADAPTOID_ROUTE_PRIVATE,
		             "the first character is not examined", 1, 1, &bad);

		/* everything else is hidclass's */
		triage_irp(&irp, 1, L'\\', L'x', 4);
		sched_expect(AdaptoidRouteOf(&g_hid_dev, &irp) ==
		             ADAPTOID_ROUTE_HIDCLASS, "a different suffix is not ours",
		             1, 1, &bad);
		triage_irp(&irp, 1, L'\\', L'q', 6);
		sched_expect(AdaptoidRouteOf(&g_hid_dev, &irp) ==
		             ADAPTOID_ROUTE_HIDCLASS, "nor is a longer name", 1, 1,
		             &bad);
		triage_irp(&irp, 0, 0, 0, 0);
		sched_expect(AdaptoidRouteOf(&g_hid_dev, &irp) ==
		             ADAPTOID_ROUTE_HIDCLASS, "nor an open with no file",
		             1, 1, &bad);

		/* a near-miss on the magic is not the control device */
		g_cdo_ext.Magic[2] = 0;
		triage_irp(&irp, 0, 0, 0, 0);
		sched_expect(AdaptoidRouteOf(&g_cdo_dev, &irp) ==
		             ADAPTOID_ROUTE_HIDCLASS,
		             "two thirds of the magic is not enough", 1, 1, &bad);
		g_cdo_ext.Magic[2] = ADAPTOID_CDO_MAGIC2;
		groups++;
	}

	/* ---- 2. the four-way wrappers ----------------------------------- */
	{
		triage_setup();

		/*
		 * OBSERVED BY EFFECT, not by a marker: with no adapter
		 * registered the real control handler refuses the open with
		 * STATUS_DELETE_PENDING, which hidclass_stub cannot produce.
		 * That proves the control CODE ran, not merely that some
		 * handler did.
		 */
		triage_irp(&irp, 0, 0, 0, 0);
		g_route_hit = 0;
		sched_expect(AdaptoidCreate(&g_cdo_dev, &irp) ==
		             STATUS_DELETE_PENDING && g_route_hit == 0,
		             "create: magic goes to the control device", 1, 1,
		             &bad);

		triage_irp(&irp, 1, L'\\', L'q', 4);
		g_route_hit = 0;
		AdaptoidCreate(&g_hid_dev, &irp);
		sched_expect(g_route_hit != 0 && g_route_hit[0] == 'p',
		             "create: 'q' goes to the private channel", 1, 1, &bad);

		triage_irp(&irp, 0, 0, 0, 0);
		g_route_hit = 0;
		AdaptoidCreate(&g_hid_dev, &irp);
		sched_expect(g_route_hit != 0 && g_route_hit[0] == 'h',
		             "create: everything else chains to hidclass", 1, 1,
		             &bad);

		g_route_hit = 0;
		AdaptoidDeviceControl(&g_hid_dev, &irp);
		sched_expect(g_route_hit != 0 && g_route_hit[0] == 'h',
		             "and so does device control", 1, 1, &bad);
		groups++;
	}

	/* ---- 3. the three-way and two-way shapes ----------------------- */
	{
		triage_setup();

		/* READ and WRITE have no private path at all */
		triage_irp(&irp, 1, L'\\', L'q', 4);
		g_route_hit = 0;
		AdaptoidRead(&g_hid_dev, &irp);
		sched_expect(g_route_hit != 0 && g_route_hit[0] == 'h',
		             "read: the private channel is IOCTL-only", 1, 1, &bad);
		g_route_hit = 0;
		AdaptoidWrite(&g_hid_dev, &irp);
		sched_expect(g_route_hit != 0 && g_route_hit[0] == 'h',
		             "and so is write", 1, 1, &bad);
		g_route_hit = 0;
		g_sp.MajorFunction = IRP_MJ_READ;
		g_sp.Parameters.Read.Length = CORE_CMD_BLOCK_BYTES;
		sched_expect(AdaptoidRead(&g_cdo_dev, &irp) ==
		             (NTSTATUS)CORE_ST_NO_SUCH_DEVICE &&
		             g_route_hit == 0,
		             "but the control device still reads", 1, 1, &bad);
		g_sp.MajorFunction = 0;

		/* PNP and POWER refuse the control device outright */
		g_route_hit = 0;
		g_irp_count = 0;
		sched_expect(AdaptoidPnpTriage(&g_cdo_dev, &irp) ==
		             STATUS_NOT_SUPPORTED,
		             "pnp on the control device is refused", 1, 1, &bad);
		sched_expect(g_route_hit == 0, "without reaching hidclass", 1, 1,
		             &bad);
		g_route_hit = 0;
		AdaptoidPnpTriage(&g_hid_dev, &irp);
		sched_expect(g_route_hit != 0 && g_route_hit[0] == 'h',
		             "but a real device's pnp chains down", 1, 1, &bad);

		/* the power path starts the next power IRP, which the original
		 * omits */
		g_pnp_count = 0;
		AdaptoidPowerTriage(&g_cdo_dev, &irp);
		sched_expect(pnp_at("nextpower") >= 0,
		             "power on the control device starts the next one",
		             pnp_at("nextpower") >= 0, 1, &bad);
		groups++;
	}

	/* ---- 4. a null saved handler is not a crash -------------------- */
	{
		triage_setup();
		AdaptoidSavedDispatch.Create = NULL;
		triage_irp(&irp, 0, 0, 0, 0);
		g_irp_count = 0;
		sched_expect(AdaptoidCreate(&g_hid_dev, &irp) ==
		             STATUS_NOT_SUPPORTED,
		             "hidclass not claiming a major function is refused", 1,
		             1, &bad);
		sched_expect(g_irp_count == 1, "and the request is completed",
		             g_irp_count, 1, &bad);
		groups++;
	}

	/* ---- 5. START brings the device up, bottom-up ------------------ */
	{
		wdm_reset(&g_hid_ext);
		g_pnp_devext   = &g_hid_ext;
		g_lower_status = STATUS_SUCCESS;
		g_pnp_count    = 0;
		/*
		 * AdaptoidStartDevice asks the adapter for its bus address and
		 * WAITS for the answer, so the fake transport has to complete
		 * by itself here. Without this the harness reports that
		 * KeWaitForSingleObject would block forever, which is how the
		 * new call was noticed.
		 */
		g_urb_autocomplete = 1;
		triage_irp(&irp, 0, 0, 0, 0);
		g_sp.MinorFunction = IRP_MN_START_DEVICE;

		sched_expect(AdaptoidPnp(&g_hid_dev, &irp) == STATUS_SUCCESS,
		             "start succeeds", 1, 1, &bad);
		sched_expect(g_hid_ext.Started == 1, "and the device is started",
		             (long)g_hid_ext.Started, 1, &bad);

		/* BOTTOM-UP: down first, then act */
		sched_expect(pnp_at("passdown") == 0,
		             "the IRP went down before anything else",
		             pnp_at("passdown"), 0, &bad);
		sched_expect(pnp_at("descriptor") > pnp_at("passdown"),
		             "the descriptor came after",
		             pnp_at("descriptor") > 0, 1, &bad);
		sched_expect(pnp_at("selectcfg") > pnp_at("descriptor"),
		             "then the configuration",
		             pnp_at("selectcfg") > pnp_at("descriptor"), 1, &bad);
		/* polling starts BEFORE the interface is published, so a listener
		 * reacting to the event finds a device already producing reports */
		sched_expect(pnp_at("pollstart") < pnp_at("enableiface"),
		             "polling starts before the interface is published",
		             pnp_at("pollstart") < pnp_at("enableiface"), 1, &bad);
		sched_expect(dx_lock_balanced(&g_hid_ext), "and the locks balanced",
		             1, 1, &bad);
		groups++;
	}

	/* ---- 6. a lower driver that fails start stops the bring-up ----- */
	{
		wdm_reset(&g_hid_ext);
		g_pnp_devext   = &g_hid_ext;
		g_lower_status = STATUS_UNSUCCESSFUL;
		g_pnp_count    = 0;
		triage_irp(&irp, 0, 0, 0, 0);
		g_sp.MinorFunction = IRP_MN_START_DEVICE;

		sched_expect(AdaptoidPnp(&g_hid_dev, &irp) == STATUS_UNSUCCESSFUL,
		             "the failure is reported", 1, 1, &bad);
		sched_expect(pnp_at("descriptor") < 0,
		             "and nothing was brought up", pnp_at("descriptor"), -1,
		             &bad);
		sched_expect(g_hid_ext.Started == 0, "nor marked started",
		             (long)g_hid_ext.Started, 0, &bad);
		sched_expect(dx_lock_balanced(&g_hid_ext),
		             "the locks balanced even so", 1, 1, &bad);

		/* the same if the descriptor fetch is what fails */
		wdm_reset(&g_hid_ext);
		g_lower_status      = STATUS_SUCCESS;
		g_descriptor_status = STATUS_UNSUCCESSFUL;
		g_pnp_count = 0;
		AdaptoidPnp(&g_hid_dev, &irp);
		sched_expect(pnp_at("selectcfg") < 0,
		             "a failed descriptor stops before the configuration",
		             pnp_at("selectcfg"), -1, &bad);
		sched_expect(g_hid_ext.Started == 0, "and does not start",
		             (long)g_hid_ext.Started, 0, &bad);
		g_descriptor_status = STATUS_SUCCESS;
		groups++;
	}

	/* ---- 7. STOP tears down top-down ------------------------------- */
	{
		wdm_reset(&g_hid_ext);
		g_pnp_devext = &g_hid_ext;
		g_hid_ext.Started = 1;
		g_lower_status = STATUS_SUCCESS;
		g_pnp_count = 0;
		triage_irp(&irp, 0, 0, 0, 0);
		g_sp.MinorFunction = IRP_MN_STOP_DEVICE;

		g_hid_ext.PollStopMask = 0;
		AdaptoidPnp(&g_hid_dev, &irp);
		sched_expect((g_hid_ext.PollStopMask &
		              ADAPTOID_STOP_REASON_PNP) != 0,
		             "polling was stopped",
		             (long)g_hid_ext.PollStopMask,
		             ADAPTOID_STOP_REASON_PNP, &bad);
		sched_expect(pnp_at("quiesce") == 0,
		             "and quiesced before anything else", pnp_at("quiesce"),
		             0, &bad);
		sched_expect(pnp_at("unconfigure") < pnp_at("passdown"),
		             "the resources go BEFORE the IRP does",
		             pnp_at("unconfigure") < pnp_at("passdown"), 1, &bad);
		sched_expect(g_hid_ext.Started == 0, "the started flag is cleared",
		             (long)g_hid_ext.Started, 0, &bad);
		groups++;
	}

	/* ---- 8. the query and cancel pairs, and the veto --------------- */
	{
		wdm_reset(&g_hid_ext);
		g_pnp_devext = &g_hid_ext;
		g_lower_status = STATUS_SUCCESS;
		triage_irp(&irp, 0, 0, 0, 0);

		/* an unstarted device leaves the status alone */
		g_hid_ext.Started = 0;
		g_sp.MinorFunction = IRP_MN_QUERY_REMOVE_DEVICE;
		irp.IoStatus.Status = STATUS_NOT_SUPPORTED;
		AdaptoidPnp(&g_hid_dev, &irp);
		sched_expect(g_hid_ext.RemovePending == 0,
		             "an unstarted device does not answer query-remove",
		             (long)g_hid_ext.RemovePending, 0, &bad);

		g_hid_ext.Started = 1;
		AdaptoidPnp(&g_hid_dev, &irp);
		sched_expect(g_hid_ext.RemovePending == 1, "a started one does",
		             (long)g_hid_ext.RemovePending, 1, &bad);
		g_sp.MinorFunction = IRP_MN_CANCEL_REMOVE_DEVICE;
		AdaptoidPnp(&g_hid_dev, &irp);
		sched_expect(g_hid_ext.RemovePending == 0, "and cancel undoes it",
		             (long)g_hid_ext.RemovePending, 0, &bad);

		/* the veto */
		g_sp.MinorFunction = IRP_MN_QUERY_STOP_DEVICE;
		g_hid_ext.StopVeto = 0;
		AdaptoidPnp(&g_hid_dev, &irp);
		sched_expect(g_hid_ext.StopPending == 1, "query-stop is accepted",
		             (long)g_hid_ext.StopPending, 1, &bad);

		g_hid_ext.StopPending = 0;
		g_hid_ext.StopVeto    = 1;
		sched_expect(AdaptoidPnp(&g_hid_dev, &irp) == STATUS_UNSUCCESSFUL,
		             "but vetoed when the guard is set", 1, 1, &bad);
		sched_expect(g_hid_ext.StopPending == 0, "and not recorded",
		             (long)g_hid_ext.StopPending, 0, &bad);
		sched_expect(dx_lock_balanced(&g_hid_ext),
		             "a veto still balances the locks", 1, 1, &bad);
		groups++;
	}

	/* ---- 9. capabilities gain Removable and SurpriseRemovalOK ------ */
	{
		DEVICE_CAPABILITIES caps;

		wdm_reset(&g_hid_ext);
		g_pnp_devext = &g_hid_ext;
		g_lower_status = STATUS_SUCCESS;
		triage_irp(&irp, 0, 0, 0, 0);
		g_sp.MinorFunction = IRP_MN_QUERY_CAPABILITIES;
		caps.DeviceD1 = 1;                  /* whatever the bus set */
		g_sp.Parameters.DeviceCapabilities.Capabilities = &caps;
		g_pnp_count = 0;

		AdaptoidPnp(&g_hid_dev, &irp);
		sched_expect(pnp_at("passdown") == 0,
		             "the bus driver fills it in first", pnp_at("passdown"),
		             0, &bad);
		sched_expect(caps.Removable != 0, "Removable is added",
		             (long)caps.Removable, 1, &bad);
		sched_expect(caps.SurpriseRemovalOK != 0, "and SurpriseRemovalOK",
		             (long)caps.SurpriseRemovalOK, 1, &bad);
		sched_expect(caps.DeviceD1 != 0,
		             "without disturbing what was there",
		             (long)caps.DeviceD1, 1, &bad);

		/* a failure downstream must not touch them */
		caps.Removable = 0;
		caps.SurpriseRemovalOK = 0;
		g_lower_status = STATUS_UNSUCCESSFUL;
		AdaptoidPnp(&g_hid_dev, &irp);
		sched_expect(caps.Removable == 0,
		             "a failed query leaves the capabilities alone",
		             (long)caps.Removable, 0, &bad);
		groups++;
	}

	/* ---- 10. anything unhandled just goes down --------------------- */
	{
		wdm_reset(&g_hid_ext);
		g_pnp_devext = &g_hid_ext;
		g_lower_status = STATUS_SUCCESS;
		g_pnp_count = 0;
		triage_irp(&irp, 0, 0, 0, 0);
		g_sp.MinorFunction = 0x0C;          /* QUERY_RESOURCES, not ours */

		AdaptoidPnp(&g_hid_dev, &irp);
		sched_expect(g_pnp_count == 1 && pnp_at("passdown") == 0,
		             "an unhandled minor function is passed straight down",
		             g_pnp_count, 1, &bad);
		sched_expect(dx_lock_balanced(&g_hid_ext), "with balanced locks", 1,
		             1, &bad);
		groups++;
	}

	hlog("Dispatch triage and PnP: %s (%d groups)\n", bad ? "FAIL" : "ok",
	     groups);
	return bad;
}

/* ---- the USB port recovery edge ------------------------------------ */

static ULONG    g_port_status     = ADAPTOID_PORT_CONNECTED;
static NTSTATUS g_port_status_ret = STATUS_SUCCESS;
static NTSTATUS g_port_reset_ret  = STATUS_SUCCESS;
static int      g_port_resets;
static int      g_port_cycles;
static int      g_port_queries;

NTSTATUS AdaptoidUsbGetPortStatus(PADAPTOID_DEVEXT DevExt, ULONG *Status)
{
	(void)DevExt;
	g_port_queries++;
	*Status = g_port_status;
	return g_port_status_ret;
}

NTSTATUS AdaptoidUsbResetPort(PADAPTOID_DEVEXT DevExt)
{ (void)DevExt; g_port_resets++; return g_port_reset_ret; }

void AdaptoidUsbCyclePort(PADAPTOID_DEVEXT DevExt)
{ (void)DevExt; g_port_cycles++; }

/* ---- the OS edge of the poll loop and the read queue --------------- */

static int   g_poll_submits;
static ULONG g_poll_last_slot;
static NTSTATUS g_poll_submit_ret = STATUS_PENDING;
static int   g_poll_freed;
static int   g_poll_cancels;
static int   g_poll_restarts;
static int   g_claim_refuse;        /* refuse to claim the next N reads */

/* Stand-ins for the IRP and URB a real submit would allocate. */
static IRP   g_poll_irp[ADAPTOID_POLL_SLOTS];
static ULONG g_poll_urb[ADAPTOID_POLL_SLOTS];

NTSTATUS AdaptoidPollSubmit(PADAPTOID_DEVEXT DevExt, ULONG Slot)
{
	g_poll_submits++;
	g_poll_last_slot = Slot;
	pnp_note("pollsubmit");
	if (Slot < ADAPTOID_POLL_SLOTS) {
		DevExt->PollSlot[Slot].Irp = &g_poll_irp[Slot];
		DevExt->PollSlot[Slot].Urb = &g_poll_urb[Slot];
	}
	return g_poll_submit_ret;
}

void AdaptoidFreePollIrp(PIRP Irp, PVOID Urb)
{ (void)Irp; (void)Urb; g_poll_freed++; }

/*
 * Cancelling a real IRP makes it COMPLETE, and it completes underneath the
 * caller. Modelling that is what puts a completion between the stop's two
 * passes - the interleaving the CancelLatch exists for, and one no test can
 * reach if cancel is a no-op.
 */
static PADAPTOID_DEVEXT g_cancel_devext;
static int              g_cancel_completes;

void AdaptoidCancelIrp(PIRP Irp)
{
	ULONG i;

	g_poll_cancels++;
	if (!g_cancel_completes || g_cancel_devext == 0) {
		return;
	}
	for (i = 0; i < ADAPTOID_POLL_SLOTS; i++) {
		if (Irp == &g_poll_irp[i]) {
			AdaptoidPollComplete(g_cancel_devext, i, STATUS_CANCELLED, 0);
			return;
		}
	}
}

void AdaptoidQueuePollRestart(PADAPTOID_DEVEXT DevExt)
{ (void)DevExt; g_poll_restarts++; pnp_note("pollrestart"); }

/*
 * THE REAL HANDSHAKE, NOT A FLAG. This used to ignore the request and
 * answer 1, which made every claim succeed whether or not a cancel routine
 * had ever been installed - so a queue that parked requests WITHOUT one
 * passed every test here and then, on hardware, dropped every report and
 * deadlocked hidclass on removal. Modelling the exchange is what makes a
 * missing IoSetCancelRoutine visible to the suite.
 *
 * g_claim_refuse stays as a deliberate override for tests that want to
 * force the losing side of the race.
 */
int AdaptoidClaimIrp(PIRP Irp)
{
	PVOID prev;

	if (g_claim_refuse > 0) {
		g_claim_refuse--;
		return 0;
	}
	prev = Irp->CancelRoutine;
	Irp->CancelRoutine = NULL;
	return prev != NULL;
}

/* What the last completed read carried. */
static UCHAR g_read_data[CORE_REPORT_MAX_BYTES];
static UCHAR g_read_len;
static int   g_read_count;

NTSTATUS AdaptoidCompleteRead(PADAPTOID_DEVEXT DevExt, PIRP Irp,
                              const UCHAR *Data, UCHAR Length)
{
	ULONG i;

	(void)DevExt;
	for (i = 0; i < Length && i < CORE_REPORT_MAX_BYTES; i++) {
		g_read_data[i] = Data[i];
	}
	g_read_len = Length;
	g_read_count++;
	return AdaptoidCompleteIrp(Irp, STATUS_SUCCESS, Length), STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* the input path: polling, the report queue, pending reads            */
/* ------------------------------------------------------------------ */

/*
 * The requests the input tests park and complete.
 *
 * FILE SCOPE SO THE FIXTURE CAN ZERO THEM. As a local array they were
 * uninitialised stack, so Cancel and CancelRoutine carried whatever the
 * previous group left behind. A real IRP arrives zeroed from
 * IoAllocateIrp, and code that reads Irp->Cancel - which any cancel-safe
 * queue must - is entitled to assume that.
 */
static IRP g_reads[4];

static void input_reset(PADAPTOID_DEVEXT dx)
{
	int i;
	u8 *p = (u8 *)dx;

	for (i = 0; i < (int)sizeof(*dx); i++) {
		p[i] = 0;
	}
	for (i = 0; i < (int)sizeof(g_reads); i++) {
		((u8 *)g_reads)[i] = 0;
	}
	core_init(&dx->Core, AdaptoidReportSink, dx);
	AdaptoidDevExtInit(dx);
	dx->Core.accessory_state = CORE_ACC_FOUND_1;   /* past the probe gate */

	g_poll_submits     = 0;
	g_poll_freed       = 0;
	g_poll_cancels     = 0;
	g_poll_restarts    = 0;
	g_poll_submit_ret  = STATUS_PENDING;
	g_claim_refuse     = 0;
	g_cancel_completes = 0;
	g_cancel_devext    = dx;
	g_read_count       = 0;
	g_read_len         = 0;
	g_irp_count        = 0;
	g_pnp_count        = 0;
}

static int test_input_path(void)
{
	int bad    = 0;
	int groups = 0;
	static ADAPTOID_DEVEXT dx;
	PIRP reads = g_reads;
	int i;

	/* ---- 1. starting polling submits both slots -------------------- */
	{
		input_reset(&dx);

		sched_expect(dx.PollStopMask == ADAPTOID_STOP_REASON_PNP,
		             "polling starts stopped",
		             (long)dx.PollStopMask, ADAPTOID_STOP_REASON_PNP, &bad);
		AdaptoidPollStart(&dx, ADAPTOID_STOP_REASON_PNP);
		sched_expect(dx.PollStopMask == 0, "the reason is cleared",
		             (long)dx.PollStopMask, 0, &bad);
		sched_expect(g_poll_submits == 2, "both slots go in flight",
		             g_poll_submits, 2, &bad);

		/* a second start does not submit four */
		AdaptoidPollStart(&dx, ADAPTOID_STOP_REASON_PNP);
		sched_expect(g_poll_submits == 2,
		             "starting twice does not double up", g_poll_submits, 2,
		             &bad);
		groups++;
	}

	/* ---- 2. a reason still held keeps polling stopped --------------- */
	{
		input_reset(&dx);
		dx.PollStopMask = ADAPTOID_STOP_REASON_PNP |
		                  ADAPTOID_STOP_REASON_ERROR;

		AdaptoidPollStart(&dx, ADAPTOID_STOP_REASON_PNP);
		sched_expect(dx.PollStopMask == ADAPTOID_STOP_REASON_ERROR,
		             "one reason cleared, one remains",
		             (long)dx.PollStopMask, ADAPTOID_STOP_REASON_ERROR,
		             &bad);
		sched_expect(g_poll_submits == 0, "and nothing is submitted",
		             g_poll_submits, 0, &bad);

		AdaptoidPollStart(&dx, ADAPTOID_STOP_REASON_ERROR);
		sched_expect(g_poll_submits == 2, "clearing the last one starts it",
		             g_poll_submits, 2, &bad);
		groups++;
	}

	/* ---- 3. a good packet decodes and resubmits its slot ----------- */
	{
		input_reset(&dx);
		AdaptoidPollStart(&dx, ADAPTOID_STOP_REASON_PNP);
		g_poll_submits = 0;

		dx.PollSlot[0].Buffer[CORE_RAW_X]          = 40;
		dx.PollSlot[0].Buffer[CORE_RAW_Y]          = 0;
		dx.PollSlot[0].Buffer[CORE_RAW_STATUS]     = CORE_STATUS_VALID;
		dx.PollSlot[0].Buffer[CORE_RAW_BUTTONS_HI] = 0;
		dx.PollSlot[0].Buffer[CORE_RAW_BUTTONS_LO] = 0;

		AdaptoidPollComplete(&dx, 0, STATUS_SUCCESS, ADAPTOID_POLL_BYTES);
		sched_expect(g_poll_submits == 1, "the slot went straight back",
		             g_poll_submits, 1, &bad);
		sched_expect(g_poll_last_slot == 0, "the same slot",
		             (long)g_poll_last_slot, 0, &bad);
		sched_expect(dx.PollStopMask == 0, "and polling is still running",
		             (long)dx.PollStopMask, 0, &bad);
		/* the packet reached the core and produced a report */
		sched_expect(dx.ReportCount == 1, "a report was queued",
		             dx.ReportCount, 1, &bad);
		sched_expect(dx.ReportQueue[0].Data[0] == CORE_REPORT_JOYSTICK,
		             "the joystick report", dx.ReportQueue[0].Data[0],
		             CORE_REPORT_JOYSTICK, &bad);
		groups++;
	}

	/* ---- 4. a short read is an error, not a partial packet ---------- */
	{
		static const struct {
			NTSTATUS st;
			ULONG    len;
			const char *what;
		} BAD[] = {
		  {STATUS_SUCCESS,      4, "a four-byte read"},
		  {STATUS_SUCCESS,      6, "a six-byte read"},
		  {STATUS_SUCCESS,      0, "an empty read"},
		  {STATUS_UNSUCCESSFUL, 5, "a failed IRP"}
		};
		int k;

		for (k = 0; k < 4; k++) {
			input_reset(&dx);
			AdaptoidPollStart(&dx, ADAPTOID_STOP_REASON_PNP);
			g_poll_submits = 0;
			dx.PollSlot[0].Buffer[CORE_RAW_STATUS] = CORE_STATUS_VALID;

			AdaptoidPollComplete(&dx, 0, BAD[k].st, BAD[k].len);
			if (g_poll_submits != 0 || dx.ReportCount != 0) {
				hlog("  FAIL input %s was accepted\n", BAD[k].what);
				bad++;
			}
			if ((dx.PollStopMask & ADAPTOID_STOP_REASON_ERROR) == 0) {
				hlog("  FAIL input %s did not stop polling\n", BAD[k].what);
				bad++;
			}
		}
		sched_expect(1, "four malformed reads all rejected", 1, 1, &bad);
		groups++;
	}

	/* ---- 5. the restart work is queued exactly once ---------------- */
	{
		input_reset(&dx);
		AdaptoidPollStart(&dx, ADAPTOID_STOP_REASON_PNP);

		/* the first failure is not the last slot outstanding */
		AdaptoidPollComplete(&dx, 0, STATUS_UNSUCCESSFUL, 0);
		sched_expect(g_poll_restarts == 0,
		             "the first failure does not queue the restart",
		             g_poll_restarts, 0, &bad);

		/* the second one is */
		AdaptoidPollComplete(&dx, 1, STATUS_UNSUCCESSFUL, 0);
		sched_expect(g_poll_restarts == 1,
		             "the last one queues it, once", g_poll_restarts, 1,
		             &bad);
		sched_expect(dx.RemoveLockB.IoCount == 2,
		             "holding the remove lock across the work",
		             dx.RemoveLockB.IoCount, 2, &bad);
		groups++;
	}

	/* ---- 6. the cancel handshake frees exactly once ---------------- */
	{
		/* stop first, then complete: the stop latches, so completion
		 * clears the latch and the stop frees */
		input_reset(&dx);
		AdaptoidPollStart(&dx, ADAPTOID_STOP_REASON_PNP);
		g_poll_freed = 0;

		AdaptoidPollStop(&dx, ADAPTOID_STOP_REASON_PNP,
		                 ADAPTOID_POLL_SLOTS);
		sched_expect(g_poll_cancels == 2, "both reads were cancelled",
		             g_poll_cancels, 2, &bad);
		/*
		 * The stop found the latch still set, so it CLEARED it and left
		 * the freeing to the completion that has not run yet. Freeing
		 * here would be freeing an IRP the stack still owns.
		 */
		sched_expect(g_poll_freed == 0, "the stop frees nothing yet",
		             g_poll_freed, 0, &bad);

		AdaptoidPollComplete(&dx, 0, STATUS_CANCELLED, 0);
		AdaptoidPollComplete(&dx, 1, STATUS_CANCELLED, 0);
		sched_expect(g_poll_freed == 2,
		             "the completions free them, exactly twice",
		             g_poll_freed, 2, &bad);

		/* THE OTHER ORDER: complete first, then stop. The completion
		 * finds the latch clear and frees; the stop must not free again. */
		input_reset(&dx);
		AdaptoidPollStart(&dx, ADAPTOID_STOP_REASON_PNP);
		g_poll_freed = 0;
		dx.PollSlot[0].Buffer[CORE_RAW_STATUS] = 0;   /* rejected packet */
		AdaptoidPollComplete(&dx, 0, STATUS_UNSUCCESSFUL, 0);
		sched_expect(g_poll_freed == 1, "completion frees its own",
		             g_poll_freed, 1, &bad);
		AdaptoidPollComplete(&dx, 1, STATUS_UNSUCCESSFUL, 0);
		sched_expect(g_poll_freed == 2, "and so does the other",
		             g_poll_freed, 2, &bad);
		AdaptoidPollStop(&dx, ADAPTOID_STOP_REASON_PNP,
		                 ADAPTOID_POLL_SLOTS);
		sched_expect(g_poll_freed == 2,
		             "a later stop frees nothing a second time",
		             g_poll_freed, 2, &bad);

		/*
		 * THE INTERLEAVED CASE, which is the one the latch is for: the
		 * cancel completes the IRP underneath the stop, so the completion
		 * runs between the stop's two passes. Exactly one of them must
		 * free each slot.
		 */
		input_reset(&dx);
		AdaptoidPollStart(&dx, ADAPTOID_STOP_REASON_PNP);
		g_poll_freed       = 0;
		g_cancel_completes = 1;
		AdaptoidPollStop(&dx, ADAPTOID_STOP_REASON_PNP,
		                 ADAPTOID_POLL_SLOTS);
		sched_expect(g_poll_freed == 2,
		             "an interleaved cancel frees each slot once",
		             g_poll_freed, 2, &bad);
		g_cancel_completes = 0;
		groups++;
	}

	/* ---- 7. a report with no reader is queued ---------------------- */
	{
		input_reset(&dx);

		core_hid_key_event(&dx.Core, 0x04, 1);
		sched_expect(dx.ReportCount == 1, "queued with no reader",
		             dx.ReportCount, 1, &bad);
		sched_expect(g_read_count == 0, "and nothing completed",
		             g_read_count, 0, &bad);

		/* a read now takes it immediately */
		sched_expect(AdaptoidReadReport(&dx, &reads[0]) == STATUS_SUCCESS,
		             "a read is answered from the queue", 1, 1, &bad);
		sched_expect(g_read_count == 1, "with a completion", g_read_count,
		             1, &bad);
		sched_expect(g_read_data[0] == CORE_REPORT_KEYBOARD,
		             "carrying the report ID first", g_read_data[0],
		             CORE_REPORT_KEYBOARD, &bad);
		sched_expect(g_read_len == 13, "and the ID plus twelve bytes",
		             g_read_len, 13, &bad);
		sched_expect(dx.ReportCount == 0, "the queue is empty again",
		             dx.ReportCount, 0, &bad);
		groups++;
	}

	/* ---- 8. a reader waiting is served the moment one arrives ------ */
	{
		input_reset(&dx);

		sched_expect(AdaptoidReadReport(&dx, &reads[0]) == STATUS_PENDING,
		             "a read with nothing queued parks", 1, 1, &bad);
		sched_expect(dx.PendingReadCount == 1, "on the pending list",
		             dx.PendingReadCount, 1, &bad);
		sched_expect(dx.RemoveLockB.IoCount == 2,
		             "holding the remove lock while parked",
		             dx.RemoveLockB.IoCount, 2, &bad);

		core_hid_mouse_button(&dx.Core, 1, 1);
		sched_expect(g_read_count == 1, "and is served on arrival",
		             g_read_count, 1, &bad);
		sched_expect(g_read_data[0] == CORE_REPORT_MOUSE, "with the report",
		             g_read_data[0], CORE_REPORT_MOUSE, &bad);
		sched_expect(dx.ReportCount == 0, "which was never queued",
		             dx.ReportCount, 0, &bad);
		sched_expect(dx.PendingReadCount == 0, "and the reader is gone",
		             dx.PendingReadCount, 0, &bad);
		sched_expect(dx.RemoveLockB.IoCount == 1, "with its lock returned",
		             dx.RemoveLockB.IoCount, 1, &bad);
		groups++;
	}

	/* ---- 9. readers are served oldest first ------------------------ */
	{
		input_reset(&dx);
		for (i = 0; i < 3; i++) {
			AdaptoidReadReport(&dx, &reads[i]);
		}
		sched_expect(dx.PendingReadCount == 3, "three parked",
		             dx.PendingReadCount, 3, &bad);

		g_irp_last = 0;
		core_hid_mouse_button(&dx.Core, 1, 1);
		sched_expect(g_irp_last == &reads[0], "the first one is served",
		             g_irp_last == &reads[0], 1, &bad);
		core_hid_mouse_button(&dx.Core, 2, 1);
		sched_expect(g_irp_last == &reads[1], "then the second",
		             g_irp_last == &reads[1], 1, &bad);
		sched_expect(dx.PendingReadCount == 1, "one still waiting",
		             dx.PendingReadCount, 1, &bad);
		groups++;
	}

	/* ---- 10. a read cancellation claimed elsewhere is skipped ------ */
	{
		input_reset(&dx);
		AdaptoidReadReport(&dx, &reads[0]);
		AdaptoidReadReport(&dx, &reads[1]);

		g_claim_refuse = 1;         /* cancellation took the first */
		g_irp_last = 0;
		core_hid_mouse_button(&dx.Core, 1, 1);

		sched_expect(g_irp_last == &reads[1],
		             "the report went to the next reader",
		             g_irp_last == &reads[1], 1, &bad);
		sched_expect(g_read_count == 1, "and only one was completed",
		             g_read_count, 1, &bad);
		sched_expect(dx.ReportCount == 0, "the report was not wasted",
		             dx.ReportCount, 0, &bad);
		groups++;
	}

	/* ---- 11. a full queue discards the NEW report ------------------ */
	{
		input_reset(&dx);

		/*
		 * Each report must be DISTINGUISHABLE, or dropping the oldest and
		 * dropping the newest look identical. Mouse moves carry their dx
		 * in the payload and emit unconditionally.
		 */
		for (i = 0; i < ADAPTOID_REPORT_QUEUE_MAX + 20; i++) {
			core_hid_mouse_move(&dx.Core, (s32)(s8)(1 + i), 0, 0);
		}
		sched_expect(dx.ReportCount == ADAPTOID_REPORT_QUEUE_MAX,
		             "the queue filled", dx.ReportCount,
		             ADAPTOID_REPORT_QUEUE_MAX, &bad);
		sched_expect(dx.ReportsDropped == 20, "and dropped twenty",
		             (long)dx.ReportsDropped, 20, &bad);

		/* THE OLDEST SURVIVES - the opposite of the notification queue. */
		sched_expect(dx.ReportQueue[dx.ReportHead].Data[2] == 1,
		             "the FIRST report is still at the head",
		             dx.ReportQueue[dx.ReportHead].Data[2], 1, &bad);
		{
			LONG tail = (dx.ReportHead + dx.ReportCount - 1) %
			            ADAPTOID_REPORT_QUEUE_MAX;

			sched_expect(dx.ReportQueue[tail].Data[2] ==
			             ADAPTOID_REPORT_QUEUE_MAX,
			             "and the hundredth is the newest kept",
			             dx.ReportQueue[tail].Data[2],
			             ADAPTOID_REPORT_QUEUE_MAX, &bad);
		}
		sched_expect(AdaptoidReadReport(&dx, &reads[0]) == STATUS_SUCCESS,
		             "and can still be read", 1, 1, &bad);
		groups++;
	}

	/* ---- 12. tearing down fails every parked read ------------------ */
	{
		input_reset(&dx);
		for (i = 0; i < 3; i++) {
			AdaptoidReadReport(&dx, &reads[i]);
		}
		g_irp_count = 0;

		AdaptoidCancelPendingReads(&dx);
		sched_expect(dx.PendingReadCount == 0, "all three withdrawn",
		             dx.PendingReadCount, 0, &bad);
		sched_expect(g_irp_count == 3, "and completed", g_irp_count, 3,
		             &bad);
		sched_expect(g_irp_status == STATUS_CANCELLED,
		             "as cancelled", (long)g_irp_status,
		             (long)STATUS_CANCELLED, &bad);
		groups++;
	}

	/* ---- 13. the device mask still gates the whole path ------------ */
	{
		input_reset(&dx);
		dx.Core.devices_mask = CORE_DEVICE_JOYSTICK;   /* keyboard off */

		AdaptoidReadReport(&dx, &reads[0]);
		core_hid_key_event(&dx.Core, 0x04, 1);
		sched_expect(g_read_count == 0,
		             "a disabled report never reaches a reader",
		             g_read_count, 0, &bad);
		sched_expect(dx.ReportCount == 0, "nor the queue", dx.ReportCount,
		             0, &bad);
		sched_expect(dx.PendingReadCount == 1, "the reader is still parked",
		             dx.PendingReadCount, 1, &bad);
		groups++;
	}

	/* ---- 14. a parked read is cancellable -------------------------
	 *
	 * THE BUG THIS EXISTS FOR. AdaptoidQueueRead once parked the request
	 * without installing a cancel routine. Three things broke at once and
	 * none of them looked like a missing cancel routine:
	 *
	 *   - IoCancelIrp had nothing to call, so hidclass's ping-pong reads
	 *     were never returned. HIDCLASS!CancelAllPingPongIrps waits for
	 *     them on removal, so PnP stalled and the driver could not be
	 *     unloaded at all.
	 *   - AdaptoidClaimIrp decides ownership by exchanging the cancel
	 *     routine for null and testing what was there. With none ever
	 *     installed every claim failed, AdaptoidDequeueRead treated each
	 *     request as already cancelled and dropped it, and NO REPORT WAS
	 *     EVER DELIVERED.
	 *   - Every parked request leaked.
	 *
	 * Checking that the routine is installed is the cheap half; the rest
	 * drives it the way the I/O manager would.
	 */
	{
		typedef void (NTAPI *CANCEL_FN)(PDEVICE_OBJECT, PIRP);
		CANCEL_FN cancel;
		DEVICE_OBJECT devobj;

		input_reset(&dx);
		g_pnp_devext = &dx;
		memset(&devobj, 0, sizeof(devobj));

		sched_expect(AdaptoidReadReport(&dx, &reads[0]) == STATUS_PENDING,
		             "a read with nothing queued parks", 1, 1, &bad);
		sched_expect(reads[0].PendingReturned != 0,
		             "marked pending", 1, 1, &bad);
		sched_expect(reads[0].CancelRoutine != NULL,
		             "with a cancel routine",
		             reads[0].CancelRoutine != NULL, 1, &bad);

		/*
		 * The I/O manager sets Cancel, then calls the routine. GUARDED,
		 * because without one this would be a call through null and the
		 * whole harness would die on the very failure it is reporting.
		 */
		cancel = (CANCEL_FN)reads[0].CancelRoutine;
		reads[0].Cancel = TRUE;
		g_irp_count = 0;
		if (cancel != NULL) {
			cancel(&devobj, &reads[0]);
		}

		sched_expect(g_irp_count == 1, "cancelling completes it",
		             g_irp_count, 1, &bad);
		sched_expect(reads[0].IoStatus.Status == STATUS_CANCELLED,
		             "as cancelled", 1,
		             reads[0].IoStatus.Status == STATUS_CANCELLED, &bad);
		sched_expect(dx.PendingReadCount == 0, "and off the list",
		             dx.PendingReadCount, 0, &bad);
		sched_expect(dx.RemoveLockB.IoCount == 1,
		             "having dropped the remove lock",
		             dx.RemoveLockB.IoCount, 1, &bad);
		groups++;
	}

	/* ---- 15. cancelled before the routine was installed ------------
	 *
	 * IoCancelIrp can win the race, and the I/O manager does NOT call a
	 * routine installed afterwards. The queue has to notice Cancel is
	 * already set and complete the request itself, or it parks something
	 * nothing will ever come back for.
	 */
	{
		input_reset(&dx);
		g_pnp_devext = &dx;
		g_irp_count  = 0;
		reads[0].Cancel = TRUE;

		AdaptoidReadReport(&dx, &reads[0]);

		sched_expect(g_irp_count == 1,
		             "an already-cancelled read is completed at once",
		             g_irp_count, 1, &bad);
		sched_expect(dx.PendingReadCount == 0, "and never stays parked",
		             dx.PendingReadCount, 0, &bad);
		sched_expect(dx.RemoveLockB.IoCount == 1,
		             "with the remove lock dropped",
		             dx.RemoveLockB.IoCount, 1, &bad);
		groups++;
	}

	/* ---- 16. removal fails every parked read and drops its lock ---- */
	{
		int i;

		input_reset(&dx);
		g_pnp_devext = &dx;
		g_irp_count  = 0;

		for (i = 0; i < 3; i++) {
			AdaptoidReadReport(&dx, &reads[i]);
		}
		sched_expect(dx.PendingReadCount == 3, "three parked",
		             dx.PendingReadCount, 3, &bad);
		sched_expect(dx.RemoveLockB.IoCount == 4,
		             "each holding the remove lock",
		             dx.RemoveLockB.IoCount, 4, &bad);

		AdaptoidCancelPendingReads(&dx);

		sched_expect(g_irp_count == 3, "removal completes all three",
		             g_irp_count, 3, &bad);
		sched_expect(dx.PendingReadCount == 0, "the list is empty",
		             dx.PendingReadCount, 0, &bad);
		/*
		 * BACK TO THE INITIAL REFERENCE. Completing without dropping
		 * these left the count above zero for good, and
		 * AdaptoidLockReleaseAndWait then waits on an event nothing can
		 * signal - hanging the removal that asked for the cancel.
		 */
		sched_expect(dx.RemoveLockB.IoCount == 1,
		             "and every remove lock is dropped",
		             dx.RemoveLockB.IoCount, 1, &bad);
		groups++;
	}

	hlog("Input path             : %s (%d groups)\n", bad ? "FAIL" : "ok",
	     groups);
	return bad;
}

/* ------------------------------------------------------------------ */
/* device naming and port recovery                                     */
/* ------------------------------------------------------------------ */

/*
 * A made-up USB topology. Each node is a hub with up to four ports; a port
 * either holds nothing, a leaf device, or another hub.
 */
#define TOPO_NODES 8
#define TOPO_PORTS 4

static struct {
	int    used;
	ULONG  ports;
	struct {
		int    connected;
		int    child;       /* index of the child hub, or -1 for a leaf */
		USHORT vid, pid, addr;
	} port[TOPO_PORTS];
} g_topo[TOPO_NODES];

/* Hub names are just "0".."7", one WCHAR, indexing g_topo. */
static WCHAR g_topo_name[TOPO_NODES][2];
static int   g_topo_roots[ADAPTOID_MAX_CONTROLLERS];

static void topo_reset(void)
{
	int i, p;

	for (i = 0; i < TOPO_NODES; i++) {
		g_topo[i].used  = 0;
		g_topo[i].ports = 0;
		g_topo_name[i][0] = (WCHAR)('0' + i);
		g_topo_name[i][1] = 0;
		for (p = 0; p < TOPO_PORTS; p++) {
			g_topo[i].port[p].connected = 0;
			g_topo[i].port[p].child     = -1;
		}
	}
	for (i = 0; i < ADAPTOID_MAX_CONTROLLERS; i++) {
		g_topo_roots[i] = -1;
	}
}

static int topo_index(const WCHAR *name)
{
	int i;

	for (i = 0; i < TOPO_NODES; i++) {
		if (name == g_topo_name[i]) {
			return i;
		}
	}
	return -1;
}

static NTSTATUS topo_root(void *ctx, ULONG index, const WCHAR **name)
{
	(void)ctx;
	if (index >= ADAPTOID_MAX_CONTROLLERS || g_topo_roots[index] < 0) {
		return STATUS_NO_SUCH_DEVICE;
	}
	*name = g_topo_name[g_topo_roots[index]];
	return STATUS_SUCCESS;
}

static NTSTATUS topo_ports(void *ctx, const WCHAR *name, ULONG *count)
{
	int i = topo_index(name);

	(void)ctx;
	if (i < 0 || !g_topo[i].used) {
		return STATUS_NO_SUCH_DEVICE;
	}
	*count = g_topo[i].ports;
	return STATUS_SUCCESS;
}

static NTSTATUS topo_info(void *ctx, const WCHAR *name, ULONG port,
                          ADAPTOID_PORT_INFO *info)
{
	int i = topo_index(name);

	(void)ctx;
	if (i < 0 || port < 1 || port > TOPO_PORTS) {
		return STATUS_NO_SUCH_DEVICE;
	}
	info->Connected     = g_topo[i].port[port - 1].connected;
	info->IsHub         = g_topo[i].port[port - 1].child >= 0;
	info->VendorId      = g_topo[i].port[port - 1].vid;
	info->ProductId     = g_topo[i].port[port - 1].pid;
	info->DeviceAddress = g_topo[i].port[port - 1].addr;
	info->ChildHubName  = info->IsHub
	                    ? g_topo_name[g_topo[i].port[port - 1].child]
	                    : NULL;
	return STATUS_SUCCESS;
}

static void topo_hub(int node, ULONG ports, int controller)
{
	g_topo[node].used  = 1;
	g_topo[node].ports = ports;
	if (controller >= 0) {
		g_topo_roots[controller] = node;
	}
}

static void topo_leaf(int node, int port, USHORT vid, USHORT pid, USHORT addr)
{
	g_topo[node].port[port - 1].connected = 1;
	g_topo[node].port[port - 1].child     = -1;
	g_topo[node].port[port - 1].vid       = vid;
	g_topo[node].port[port - 1].pid       = pid;
	g_topo[node].port[port - 1].addr      = addr;
}

static void topo_child(int node, int port, int child)
{
	g_topo[node].port[port - 1].connected = 1;
	g_topo[node].port[port - 1].child     = child;
}

static int names_equal(const char *a, const char *b)
{
	while (*a != 0 && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static int test_naming_recovery(void)
{
	int bad    = 0;
	int groups = 0;
	static ADAPTOID_DEVEXT dx;
	ADAPTOID_TOPOLOGY topo;
	char name[CORE_DEVICE_NAME_BYTES];

	topo.RootHub  = topo_root;
	topo.HubPorts = topo_ports;
	topo.PortInfo = topo_info;
	topo.Context  = 0;

	/* ---- 1. a device straight on a root hub ------------------------ */
	{
		topo_reset();
		topo_hub(0, 4, 0);                      /* controller A */
		topo_leaf(0, 2, ADAPTOID_VENDOR_ID, ADAPTOID_PRODUCT_ID, 7);

		sched_expect(AdaptoidBuildLocationName(&topo, 7, name,
		                                       sizeof(name)) == 1,
		             "the device is found", 1, 1, &bad);
		sched_expect(names_equal(name, "A2"), "controller A, port 2",
		             name[1], '2', &bad);
		groups++;
	}

	/* ---- 2. behind hubs, outermost port first ---------------------- */
	{
		topo_reset();
		topo_hub(0, 4, 0);
		topo_hub(1, 4, -1);
		topo_hub(2, 4, -1);
		topo_child(0, 3, 1);                    /* root port 3 -> hub 1 */
		topo_child(1, 1, 2);                    /* hub 1 port 1 -> hub 2 */
		topo_leaf(2, 4, ADAPTOID_VENDOR_ID, ADAPTOID_PRODUCT_ID, 9);

		sched_expect(AdaptoidBuildLocationName(&topo, 9, name,
		                                       sizeof(name)) == 1,
		             "found three tiers down", 1, 1, &bad);
		sched_expect(names_equal(name, "A314"),
		             "the path reads outermost first", name[1], '3', &bad);
		groups++;
	}

	/* ---- 3. the controller letter follows the index ---------------- */
	{
		topo_reset();
		topo_hub(0, 2, 2);                      /* controller C */
		topo_leaf(0, 1, ADAPTOID_VENDOR_ID, ADAPTOID_PRODUCT_ID, 3);

		AdaptoidBuildLocationName(&topo, 3, name, sizeof(name));
		sched_expect(names_equal(name, "C1"), "the third controller is C",
		             name[0], 'C', &bad);
		groups++;
	}

	/* ---- 4. THE ADDRESS is what tells two identical adapters apart -- */
	{
		topo_reset();
		topo_hub(0, 4, 0);
		/* two adapters, same vendor and product, different addresses */
		topo_leaf(0, 1, ADAPTOID_VENDOR_ID, ADAPTOID_PRODUCT_ID, 11);
		topo_leaf(0, 3, ADAPTOID_VENDOR_ID, ADAPTOID_PRODUCT_ID, 12);

		AdaptoidBuildLocationName(&topo, 11, name, sizeof(name));
		sched_expect(names_equal(name, "A1"), "the first is on port 1",
		             name[1], '1', &bad);
		AdaptoidBuildLocationName(&topo, 12, name, sizeof(name));
		sched_expect(names_equal(name, "A3"),
		             "and the second on port 3, not the first's port",
		             name[1], '3', &bad);
		groups++;
	}

	/* ---- 5. other devices are not us ------------------------------- */
	{
		topo_reset();
		topo_hub(0, 4, 0);
		topo_leaf(0, 1, 0x045E, 0x0040, 5);     /* somebody else's mouse */
		topo_leaf(0, 2, ADAPTOID_VENDOR_ID, 0x0002, 6);  /* our VID, not
		                                                  * our product */
		topo_leaf(0, 3, ADAPTOID_VENDOR_ID, ADAPTOID_PRODUCT_ID, 99);

		sched_expect(AdaptoidBuildLocationName(&topo, 7, name,
		                                       sizeof(name)) == 0,
		             "an address that is not there is not found", 0, 0,
		             &bad);
		sched_expect(AdaptoidBuildLocationName(&topo, 5, name,
		                                       sizeof(name)) == 0,
		             "nor is another vendor's device at that address", 0,
		             0, &bad);
		sched_expect(AdaptoidBuildLocationName(&topo, 6, name,
		                                       sizeof(name)) == 0,
		             "nor our vendor's other product", 0, 0, &bad);
		sched_expect(AdaptoidBuildLocationName(&topo, 99, name,
		                                       sizeof(name)) == 1,
		             "but ours is", 1, 1, &bad);
		groups++;
	}

	/* ---- 6. a path too deep to name is not truncated ---------------- */
	{
		char small[4];       /* letter, one digit, NUL, and a spare */

		topo_reset();
		topo_hub(0, 4, 0);
		topo_hub(1, 4, -1);
		topo_hub(2, 4, -1);
		topo_child(0, 1, 1);
		topo_child(1, 2, 2);
		topo_leaf(2, 3, ADAPTOID_VENDOR_ID, ADAPTOID_PRODUCT_ID, 4);

		/* the full name is "A123", which does not fit in four bytes */
		sched_expect(AdaptoidBuildLocationName(&topo, 4, small,
		                                       sizeof(small)) == 0,
		             "a name that does not fit is refused", 0, 0, &bad);
		sched_expect(small[0] == 0, "and nothing is left behind",
		             small[0], 0, &bad);

		/* with room it names it */
		sched_expect(AdaptoidBuildLocationName(&topo, 4, name,
		                                       sizeof(name)) == 1,
		             "and with room it succeeds", 1, 1, &bad);
		sched_expect(names_equal(name, "A123"), "correctly", name[3], '3',
		             &bad);
		groups++;
	}

	/* ---- 7. a missing controller is skipped ------------------------ */
	{
		topo_reset();
		topo_hub(0, 2, 3);                      /* only controller D */
		topo_leaf(0, 2, ADAPTOID_VENDOR_ID, ADAPTOID_PRODUCT_ID, 8);

		sched_expect(AdaptoidBuildLocationName(&topo, 8, name,
		                                       sizeof(name)) == 1,
		             "controllers A to C are absent, D is searched", 1, 1,
		             &bad);
		sched_expect(names_equal(name, "D2"), "and names it", name[0], 'D',
		             &bad);
		groups++;
	}

	/* ---- 8. an uninstalled seam is unnameable, not fatal ----------- */
	{
		ADAPTOID_TOPOLOGY empty;

		empty.RootHub  = 0;
		empty.HubPorts = 0;
		empty.PortInfo = 0;
		empty.Context  = 0;
		sched_expect(AdaptoidBuildLocationName(&empty, 1, name,
		                                       sizeof(name)) == 0,
		             "an empty topology names nothing", 0, 0, &bad);

		wdm_reset(&dx);
		AdaptoidSetDeviceName(&dx);
		sched_expect(dx.Core.device_name[0] == '?',
		             "and the device is called ?", dx.Core.device_name[0],
		             '?', &bad);
		groups++;
	}

	/* ---- 9. recovery resets only a disabled, connected port -------- */
	{
		wdm_reset(&dx);
		g_port_resets = 0;
		g_port_cycles = 0;

		/* still connected, port disabled: worth a reset */
		g_port_status     = ADAPTOID_PORT_CONNECTED;
		g_port_status_ret = STATUS_SUCCESS;
		g_port_reset_ret  = STATUS_SUCCESS;
		sched_expect(AdaptoidRecoverPort(&dx) == STATUS_SUCCESS,
		             "a disabled but connected port is reset", 1, 1, &bad);
		sched_expect(g_port_resets == 1, "once", g_port_resets, 1, &bad);
		sched_expect(g_port_cycles == 0, "without cycling", g_port_cycles,
		             0, &bad);

		/* unplugged: past recovering */
		g_port_resets = 0;
		g_port_status = 0;
		sched_expect(AdaptoidRecoverPort(&dx) == ADAPTOID_STATUS_GAVE_UP,
		             "a disconnected port gives up", 1, 1, &bad);
		sched_expect(g_port_cycles == 1, "after cycling it", g_port_cycles,
		             1, &bad);
		sched_expect(g_port_resets == 0, "and never resetting",
		             g_port_resets, 0, &bad);

		/* still enabled: nothing to recover */
		g_port_cycles = 0;
		g_port_status = ADAPTOID_PORT_CONNECTED | ADAPTOID_PORT_ENABLED;
		sched_expect(AdaptoidRecoverPort(&dx) == ADAPTOID_STATUS_GAVE_UP,
		             "an enabled port gives up too", 1, 1, &bad);

		/* the status query itself failing */
		g_port_cycles = 0;
		g_port_status_ret = STATUS_UNSUCCESSFUL;
		sched_expect(AdaptoidRecoverPort(&dx) == ADAPTOID_STATUS_GAVE_UP,
		             "and so does a failed query", 1, 1, &bad);
		sched_expect(g_port_cycles == 1, "cycling once", g_port_cycles, 1,
		             &bad);
		g_port_status_ret = STATUS_SUCCESS;
		groups++;
	}

	/* ---- 10. the retry ladder -------------------------------------- */
	{
		/* recovers first time */
		wdm_reset(&dx);
		AdaptoidLockAcquire(&dx.RemoveLockB);   /* as the completion does */
		g_port_resets = 0;
		g_port_cycles = 0;
		g_poll_submits = 0;
		dx.PollRestartPending = 1;
		dx.PollStopMask       = ADAPTOID_STOP_REASON_ERROR;
		g_port_status = ADAPTOID_PORT_CONNECTED;

		AdaptoidPollRestartWorker(&dx);
		sched_expect(g_port_resets == 1, "one recovery was enough",
		             g_port_resets, 1, &bad);
		sched_expect(g_poll_submits == 2, "and polling restarted",
		             g_poll_submits, 2, &bad);
		sched_expect(g_port_cycles == 0, "with no cycle", g_port_cycles, 0,
		             &bad);
		sched_expect(dx.RemoveLockB.IoCount == 1,
		             "and the remove lock was released",
		             dx.RemoveLockB.IoCount, 1, &bad);

		/* three failures then a cycle */
		wdm_reset(&dx);
		AdaptoidLockAcquire(&dx.RemoveLockB);
		g_port_resets = 0;
		g_port_cycles = 0;
		g_poll_submits = 0;
		dx.PollRestartPending = 1;
		dx.PollStopMask       = ADAPTOID_STOP_REASON_ERROR;
		g_port_status    = ADAPTOID_PORT_CONNECTED;
		g_port_reset_ret = STATUS_UNSUCCESSFUL;

		AdaptoidPollRestartWorker(&dx);
		sched_expect(g_port_resets == ADAPTOID_RECOVER_TRIES,
		             "it tries three times", g_port_resets,
		             ADAPTOID_RECOVER_TRIES, &bad);
		sched_expect(g_port_cycles == 1, "then cycles the port once",
		             g_port_cycles, 1, &bad);
		sched_expect(g_poll_submits == 0, "and does not restart polling",
		             g_poll_submits, 0, &bad);
		g_port_reset_ret = STATUS_SUCCESS;

		/* a recovery that already cycled stops the ladder AT ONCE */
		wdm_reset(&dx);
		AdaptoidLockAcquire(&dx.RemoveLockB);
		g_port_resets = 0;
		g_port_cycles = 0;
		dx.PollRestartPending = 1;
		g_port_status = 0;                      /* disconnected */

		AdaptoidPollRestartWorker(&dx);
		sched_expect(g_port_cycles == 1,
		             "the give-up sentinel stops after ONE cycle",
		             g_port_cycles, 1, &bad);
		sched_expect(g_port_resets == 0, "with no resets attempted",
		             g_port_resets, 0, &bad);
		sched_expect(dx.RemoveLockB.IoCount == 1, "lock still released",
		             dx.RemoveLockB.IoCount, 1, &bad);

		/* nothing pending: the worker does nothing but release */
		wdm_reset(&dx);
		AdaptoidLockAcquire(&dx.RemoveLockB);
		g_port_cycles = 0;
		g_port_resets = 0;
		dx.PollRestartPending = 0;
		AdaptoidPollRestartWorker(&dx);
		sched_expect(g_port_resets == 0 && g_port_cycles == 0,
		             "a worker with nothing to do does nothing",
		             g_port_resets + g_port_cycles, 0, &bad);
		sched_expect(dx.RemoveLockB.IoCount == 1, "but still releases",
		             dx.RemoveLockB.IoCount, 1, &bad);
		groups++;
	}

	hlog("Naming and recovery    : %s (%d groups)\n", bad ? "FAIL" : "ok",
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

/* ======================================================================
 * THE SDK COMMAND-BLOCK CHANNEL
 *
 * Six of these test something the original does that a reasonable
 * reimplementation would get wrong: the cached read that never reaches the
 * bus, the emulated Rumble Pak, the inverted CRC that means "no accessory",
 * the status byte that lands on top of a command byte and is put back, the
 * reply arriving reversed, and the byte-order word that is also the go flag.
 * ====================================================================== */

typedef struct cmd_xfer_log {
	u8  setup[6];
	u32 len;
	int keep;
} cmd_xfer_log;

static cmd_xfer_log g_cmd_xfers[16];
static int          g_cmd_xfer_count;
static int          g_cmd_claims;
static int          g_cmd_claim_ok = 1;
static int          g_cmd_enables;
static int          g_cmd_enable_last;

/* What the fake transport writes back, and how much of it. */
static u8  g_cmd_reply[64];
static u32 g_cmd_reply_len;

static void cmd_reset_log(void)
{
	g_cmd_xfer_count  = 0;
	g_cmd_claims      = 0;
	g_cmd_claim_ok    = 1;
	g_cmd_enables     = 0;
	g_cmd_enable_last = -1;
	g_cmd_reply_len   = 0;
	memset(g_cmd_xfers, 0, sizeof(g_cmd_xfers));
	memset(g_cmd_reply, 0, sizeof(g_cmd_reply));
}

static int cmd_claim(void *ctx)
{
	(void)ctx;
	g_cmd_claims++;
	return g_cmd_claim_ok;
}

static u32 cmd_xfer(void *ctx, const u8 *setup, u8 *data, u32 len, int keep)
{
	cmd_xfer_log *e;

	(void)ctx;
	if (g_cmd_xfer_count < (int)(sizeof(g_cmd_xfers) /
	                             sizeof(g_cmd_xfers[0]))) {
		e = &g_cmd_xfers[g_cmd_xfer_count];
		memcpy(e->setup, setup, 6);
		e->len  = len;
		e->keep = keep;
	}
	g_cmd_xfer_count++;

	/* A device-to-host transfer gets whatever the test staged. */
	if ((setup[0] & 0x80) != 0 && g_cmd_reply_len != 0) {
		u32 n = g_cmd_reply_len < len ? g_cmd_reply_len : len;

		memcpy(data, g_cmd_reply, n);
	}
	return CORE_ST_SUCCESS;
}

static void cmd_enable(void *ctx, int on)
{
	(void)ctx;
	g_cmd_enables++;
	g_cmd_enable_last = on;
}

/* One registry with one adapter wired to the fake transport. */
static void cmd_setup_one(core_registry *reg, core_device_entry *ent,
                          core_state *cs)
{
	core_registry_init(reg);
	memset(ent, 0, sizeof(*ent));
	memset(cs, 0, sizeof(*cs));
	ent->handle    = 0x1234;
	ent->cs        = cs;
	ent->live      = 1;
	ent->enable    = cmd_enable;
	ent->cmd_claim = cmd_claim;
	ent->cmd_xfer  = cmd_xfer;
	ent->os_ctx    = 0;
	core_registry_add(reg, ent);
	reg->live_count = 1;
	cmd_reset_log();
}

static int test_command_block(void)
{
	int bad    = 0;
	int groups = 0;
	core_registry     reg;
	core_device_entry ent;
	core_state        cs;
	core_cmd_channel  ch;
	u8  blk[CORE_CMD_BLOCK_BYTES];
	u32 st, info;
	int i;

	/* ---- 1. the four emulated CRCs really are CRC-8 ---------------- */
	{
		/*
		 * THE POINT OF THIS GROUP. The original answers an emulated Pak
		 * write with one of four CONSTANTS and never computes anything.
		 * If those constants are the CRC-8 of thirty-two identical
		 * bytes then the emulation is a faithful Rumble Pak and the
		 * reading of the whole path is right; if they are not, it is
		 * something else and the path has been misread.
		 *
		 * core_pak_data_crc8 was ported from drv_N64PakDataCrc8, a
		 * different function analysed at a different time, so this is
		 * an independent check and not a restatement.
		 */
		static const struct { u8 value; u8 crc; } vec[] = {
			{ 0x00, 0x00 }, { 0x01, 0xEB },
			{ 0x80, 0xB8 }, { 0xFE, 0xE1 }
		};
		u8 buf[32];

		for (i = 0; i < 4; i++) {
			memset(buf, vec[i].value, sizeof(buf));
			sched_expect(core_pak_data_crc8(buf, 32) == vec[i].crc,
			             "emulated CRC is the real CRC-8",
			             core_pak_data_crc8(buf, 32), vec[i].crc,
			             &bad);
		}
		groups++;
	}

	/* ---- 2. the cached controller read touches no transport -------- */
	{
		cmd_setup_one(&reg, &ent, &cs);
		/* X, Y, status, buttons high, buttons low */
		cs.raw[0] = 0x11; cs.raw[1] = 0x22; cs.raw[2] = 0x80;
		cs.raw[3] = 0x33; cs.raw[4] = 0x44;

		memset(blk, 0, sizeof(blk));
		blk[0] = 0x01;      /* command length */
		blk[1] = 0x04;      /* reply length   */
		blk[2] = 0x01;      /* joybus: read controller state */
		blk[3 + 4] = CORE_CMD_END;

		core_cmd_process(&reg, blk, 1, 0);
		sched_expect(g_cmd_xfer_count == 0 && g_cmd_claims == 0,
		             "answered without touching the bus",
		             g_cmd_xfer_count, 0, &bad);
		sched_expect(blk[3] == 0x44 && blk[4] == 0x33 &&
		             blk[5] == 0x11 && blk[6] == 0x22,
		             "reply is buttons-hi, buttons-lo, X, Y",
		             blk[3], 0x44, &bad);
		sched_expect((blk[1] & CORE_CMD_FAILED) == 0,
		             "and the entry is not marked failed", 0, 0, &bad);

		/* pass 0 must leave it alone - it is pass 1's entry */
		memset(blk + 3, 0, 4);
		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(blk[3] == 0 && blk[4] == 0,
		             "pass 0 does not answer it", blk[3], 0, &bad);
		groups++;
	}

	/* ---- 3. the emulated Rumble Pak identify sequence -------------- */
	{
		u8 *reply;

		cmd_setup_one(&reg, &ent, &cs);
		cs.emu_pak_present = 1;

		/* write 32 x 0x80 to address 0x8000 (carried as 0x8001) */
		memset(blk, 0, sizeof(blk));
		blk[0] = 0x23;
		blk[1] = 0x01;
		blk[2] = 0x03;          /* joybus write */
		blk[3] = 0x80;
		blk[4] = 0x01;
		memset(blk + 5, 0x80, 32);
		reply = blk + 0x23 + 2;
		*reply = 0x5A;          /* poison, must be overwritten */

		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(g_cmd_xfer_count == 0,
		             "the Pak write never reaches the bus",
		             g_cmd_xfer_count, 0, &bad);
		sched_expect(*reply == 0xB8, "and answers CRC-8 of 32 x 0x80",
		             *reply, 0xB8, &bad);
		sched_expect(cs.emu_pak_value == 0x80, "the value is remembered",
		             (long)cs.emu_pak_value, 0x80, &bad);

		/* read it back: a Rumble Pak returns 32 x 0x80 */
		memset(blk, 0, sizeof(blk));
		blk[0] = 0x03;
		blk[1] = 0x21;
		blk[2] = 0x02;          /* joybus read */
		blk[3] = 0x80;
		blk[4] = 0x01;
		reply = blk + 3 + 2;

		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(reply[0] == 0x80 && reply[31] == 0x80,
		             "read back as thirty-two 0x80 bytes",
		             reply[0], 0x80, &bad);
		sched_expect(reply[32] == 0xB8, "with the matching CRC",
		             reply[32], 0xB8, &bad);

		/* the Controller Pak probe writes 0xFE and reads zeroes */
		memset(blk, 0, sizeof(blk));
		blk[0] = 0x23; blk[1] = 0x01; blk[2] = 0x03;
		blk[3] = 0x80; blk[4] = 0x01;
		memset(blk + 5, 0xFE, 32);
		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(blk[0x23 + 2] == 0xE1, "0xFE answers 0xE1",
		             blk[0x23 + 2], 0xE1, &bad);

		memset(blk, 0, sizeof(blk));
		blk[0] = 0x03; blk[1] = 0x21; blk[2] = 0x02;
		blk[3] = 0x80; blk[4] = 0x01;
		reply = blk + 5;
		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(reply[0] == 0x00 && reply[32] == 0x00,
		             "and then reads back as zeroes", reply[32], 0,
		             &bad);
		groups++;
	}

	/* ---- 4. no accessory inverts the CRC --------------------------- */
	{
		u8 *reply;

		cmd_setup_one(&reg, &ent, &cs);
		cs.emu_pak_present = 0;

		memset(blk, 0, sizeof(blk));
		blk[0] = 0x23; blk[1] = 0x01; blk[2] = 0x03;
		blk[3] = 0x80; blk[4] = 0x01;
		memset(blk + 5, 0x80, 32);
		reply = blk + 0x23 + 2;

		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(*reply == (u8)~0xB8,
		             "with no pak the CRC comes back inverted",
		             *reply, (u8)~0xB8, &bad);

		memset(blk, 0, sizeof(blk));
		blk[0] = 0x03; blk[1] = 0x21; blk[2] = 0x02;
		blk[3] = 0x80; blk[4] = 0x01;
		reply = blk + 5;
		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(reply[32] == 0xFF && reply[0] == 0x00,
		             "and a read gives zeroes and 0xFF", reply[32],
		             0xFF, &bad);
		groups++;
	}

	/* ---- 5. the motor register drives the real enable -------------- */
	{
		cmd_setup_one(&reg, &ent, &cs);
		cs.emu_pak_present = 1;

		memset(blk, 0, sizeof(blk));
		blk[0] = 0x23; blk[1] = 0x01; blk[2] = 0x03;
		blk[3] = 0xC0; blk[4] = 0x1B;   /* address 0xC000, the motor */
		memset(blk + 5, 0x01, 32);
		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(g_cmd_enables == 1 && g_cmd_enable_last == 1,
		             "writing 1 to 0xC000 turns the motor on",
		             g_cmd_enable_last, 1, &bad);

		memset(blk + 5, 0x00, 32);
		blk[1] = 0x01;
		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(g_cmd_enables == 2 && g_cmd_enable_last == 0,
		             "and writing 0 turns it off", g_cmd_enable_last, 0,
		             &bad);

		/* the identify region must NOT drive it */
		blk[3] = 0x80; blk[4] = 0x01; blk[1] = 0x01;
		memset(blk + 5, 0x01, 32);
		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(g_cmd_enables == 2,
		             "the identify region does not", g_cmd_enables, 2,
		             &bad);

		/* and neither does it with no accessory present */
		cs.emu_pak_present = 0;
		blk[3] = 0xC0; blk[4] = 0x1B; blk[1] = 0x01;
		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(g_cmd_enables == 2,
		             "nor does it with no pak", g_cmd_enables, 2, &bad);
		groups++;
	}

	/* ---- 6. the short form, and the borrowed status byte ----------- */
	{
		cmd_setup_one(&reg, &ent, &cs);

		memset(blk, 0, sizeof(blk));
		blk[0] = 0x04;          /* four command bytes */
		blk[1] = 0x02;          /* two reply bytes    */
		blk[2] = 0xAA; blk[3] = 0xBB; blk[4] = 0xCC; blk[5] = 0xDD;

		/* status byte, then the two reply bytes */
		g_cmd_reply[0] = 0x01;
		g_cmd_reply[1] = 0x11;
		g_cmd_reply[2] = 0x22;
		g_cmd_reply_len = 3;

		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(g_cmd_xfer_count == 1, "one transfer",
		             g_cmd_xfer_count, 1, &bad);
		sched_expect(g_cmd_xfers[0].setup[0] == 0xC0 &&
		             g_cmd_xfers[0].setup[1] == 0x24,
		             "bRequest is 0x20 plus the command length",
		             g_cmd_xfers[0].setup[1], 0x24, &bad);
		sched_expect(g_cmd_xfers[0].setup[2] == 0xAA &&
		             g_cmd_xfers[0].setup[3] == 0xBB &&
		             g_cmd_xfers[0].setup[4] == 0xCC &&
		             g_cmd_xfers[0].setup[5] == 0xDD,
		             "command bytes fill wValue then wIndex",
		             g_cmd_xfers[0].setup[2], 0xAA, &bad);
		sched_expect(g_cmd_xfers[0].len == 3,
		             "and it reads reply length plus one",
		             (long)g_cmd_xfers[0].len, 3, &bad);
		sched_expect(g_cmd_xfers[0].keep == 0, "releasing the slot",
		             g_cmd_xfers[0].keep, 0, &bad);
		sched_expect(blk[5] == 0xDD,
		             "the borrowed command byte is put back", blk[5],
		             0xDD, &bad);
		/* two reply bytes, reversed */
		sched_expect(blk[6] == 0x22 && blk[7] == 0x11,
		             "and the reply comes back reversed", blk[6], 0x22,
		             &bad);
		sched_expect((blk[1] & CORE_CMD_FAILED) == 0,
		             "a non-zero status is success", 0, 0, &bad);

		/* a zero status byte marks the entry */
		cmd_reset_log();
		blk[0] = 0x04; blk[1] = 0x02;
		g_cmd_reply[0] = 0x00;
		g_cmd_reply_len = 3;
		core_cmd_process(&reg, blk, 0, 0);
		sched_expect((blk[1] & CORE_CMD_FAILED) != 0,
		             "a zero status marks it failed", 1, 1, &bad);
		groups++;
	}

	/* ---- 7. the long form is two transfers, the first keeping ------ */
	{
		cmd_setup_one(&reg, &ent, &cs);

		memset(blk, 0, sizeof(blk));
		blk[0] = 0x08;          /* eight command bytes -> long form */
		blk[1] = 0x03;          /* three reply bytes                */
		for (i = 0; i < 8; i++) {
			blk[2 + i] = (u8)(0xA0 + i);
		}
		/* status 0x80 | 3, then three reply bytes */
		g_cmd_reply[0] = 0x83;
		g_cmd_reply[1] = 0x01;
		g_cmd_reply[2] = 0x02;
		g_cmd_reply[3] = 0x03;
		g_cmd_reply_len = 4;

		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(g_cmd_claims == 1, "the slot is claimed once",
		             g_cmd_claims, 1, &bad);
		sched_expect(g_cmd_xfer_count == 2, "and there are two transfers",
		             g_cmd_xfer_count, 2, &bad);
		sched_expect(g_cmd_xfers[0].setup[0] == 0x40 &&
		             g_cmd_xfers[0].setup[1] == 0x20,
		             "the first is a host-to-device write",
		             g_cmd_xfers[0].setup[0], 0x40, &bad);
		sched_expect(g_cmd_xfers[0].setup[2] == 0x03 &&
		             g_cmd_xfers[0].setup[3] == 0xA0,
		             "carrying the reply length and the first command byte",
		             g_cmd_xfers[0].setup[2], 0x03, &bad);
		sched_expect(g_cmd_xfers[0].len == 5,
		             "of command length minus three bytes",
		             (long)g_cmd_xfers[0].len, 5, &bad);
		sched_expect(g_cmd_xfers[0].keep == 1,
		             "AND IT KEEPS THE SLOT", g_cmd_xfers[0].keep, 1,
		             &bad);
		sched_expect(g_cmd_xfers[1].setup[0] == 0xC0 &&
		             g_cmd_xfers[1].setup[1] == 0x71 &&
		             g_cmd_xfers[1].setup[2] == 0x30,
		             "the second is the fixed 0x71 read",
		             g_cmd_xfers[1].setup[1], 0x71, &bad);
		sched_expect(g_cmd_xfers[1].keep == 0,
		             "which releases it", g_cmd_xfers[1].keep, 0, &bad);
		sched_expect(blk[9] == (u8)(0xA0 + 7),
		             "the borrowed command byte is put back", blk[9],
		             0xA7, &bad);
		sched_expect(blk[10] == 0x03 && blk[12] == 0x01,
		             "and the three reply bytes are reversed", blk[10],
		             0x03, &bad);
		sched_expect((blk[1] & CORE_CMD_FAILED) == 0,
		             "status 0x80|len is success", 0, 0, &bad);

		/* the status byte is a LENGTH: a mismatch is a failure */
		cmd_reset_log();
		blk[0] = 0x08; blk[1] = 0x03;
		g_cmd_reply[0] = 0x82;      /* says two, we asked for three */
		g_cmd_reply_len = 4;
		core_cmd_process(&reg, blk, 0, 0);
		sched_expect((blk[1] & CORE_CMD_FAILED) != 0,
		             "a short length in the status marks it failed", 1,
		             1, &bad);

		cmd_reset_log();
		blk[0] = 0x08; blk[1] = 0x03;
		g_cmd_reply[0] = 0x03;      /* right length, valid bit clear */
		g_cmd_reply_len = 4;
		core_cmd_process(&reg, blk, 0, 0);
		sched_expect((blk[1] & CORE_CMD_FAILED) != 0,
		             "so does a clear valid bit", 1, 1, &bad);
		groups++;
	}

	/* ---- 8. a busy slot is silent, and so is the long-and-wide gap - */
	{
		cmd_setup_one(&reg, &ent, &cs);
		g_cmd_claim_ok = 0;

		memset(blk, 0, sizeof(blk));
		blk[0] = 0x04; blk[1] = 0x02;
		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(g_cmd_xfer_count == 0,
		             "a busy slot runs no transfer", g_cmd_xfer_count,
		             0, &bad);
		sched_expect((blk[1] & CORE_CMD_FAILED) == 0,
		             "AND DOES NOT MARK THE ENTRY - defect", 0, 0,
		             &bad);

		/*
		 * A command of five bytes or more that also wants four or more
		 * back is rejected by the WALKER, which is what makes the
		 * matching guard inside core_cmd_exec unreachable from here -
		 * see the comment on it.
		 */
		cmd_reset_log();
		memset(blk, 0, sizeof(blk));
		blk[0] = 0x05; blk[1] = 0x04;
		blk[2] = 0x01; blk[3] = 0x04; blk[4] = 0x01;
		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(g_cmd_xfer_count == 0 && g_cmd_claims == 0,
		             "a long command with a wide reply never runs",
		             g_cmd_claims, 0, &bad);
		sched_expect((blk[1] & CORE_CMD_FAILED) != 0,
		             "the walker marks it structurally bad", 1, 1,
		             &bad);
		sched_expect(blk[5] == 0x00,
		             "and stops before the next entry", blk[5], 0x00,
		             &bad);

		/* One byte narrower in either direction and it does run. */
		cmd_reset_log();
		memset(blk, 0, sizeof(blk));
		blk[0] = 0x05; blk[1] = 0x03;
		core_cmd_process(&reg, blk, 0, 0);
		sched_expect(g_cmd_claims == 1,
		             "a three-byte reply is accepted", g_cmd_claims, 1,
		             &bad);
		groups++;
	}

	/* ---- 9. the walk: padding, skip, end, and the two failure kinds  */
	{
		core_device_entry ent2;
		core_state        cs2;

		cmd_setup_one(&reg, &ent, &cs);
		memset(&ent2, 0, sizeof(ent2));
		memset(&cs2, 0, sizeof(cs2));
		ent2.handle = 0x5678;
		ent2.cs     = &cs2;
		ent2.live   = 1;
		core_registry_add(&reg, &ent2);
		reg.live_count = 2;

		cs.raw[0]  = 0x01; cs.raw[1]  = 0x02;
		cs.raw[3]  = 0x03; cs.raw[4]  = 0x04;
		cs2.raw[0] = 0x11; cs2.raw[1] = 0x12;
		cs2.raw[3] = 0x13; cs2.raw[4] = 0x14;

		/* padding, then an entry: the entry addresses adapter 1 */
		memset(blk, 0, sizeof(blk));
		blk[0] = CORE_CMD_PAD;
		blk[1] = 0x01; blk[2] = 0x04; blk[3] = 0x01;
		blk[8] = CORE_CMD_END;
		core_cmd_process(&reg, blk, 1, 0);
		sched_expect(blk[4] == 0x14 && blk[6] == 0x11,
		             "padding advances the device index", blk[4], 0x14,
		             &bad);

		/* 0xFF skips a byte WITHOUT advancing it */
		memset(blk, 0, sizeof(blk));
		blk[0] = CORE_CMD_SKIP;
		blk[1] = 0x01; blk[2] = 0x04; blk[3] = 0x01;
		blk[8] = CORE_CMD_END;
		core_cmd_process(&reg, blk, 1, 0);
		sched_expect(blk[4] == 0x04 && blk[6] == 0x01,
		             "but 0xFF does not", blk[4], 0x04, &bad);

		/* 0xFE stops the walk before the entry after it */
		memset(blk, 0, sizeof(blk));
		blk[0] = CORE_CMD_END;
		blk[1] = 0x01; blk[2] = 0x04; blk[3] = 0x01;
		core_cmd_process(&reg, blk, 1, 0);
		sched_expect(blk[4] == 0x00,
		             "0xFE ends the block", blk[4], 0x00, &bad);

		/*
		 * A STRUCTURAL ERROR STOPS THE WALK, a missing device does not.
		 * Two entries: the first over-long, the second valid. Only the
		 * first is marked and the second is never reached.
		 */
		memset(blk, 0, sizeof(blk));
		blk[0] = 0x30;          /* longer than CORE_CMD_MAX_LEN */
		blk[1] = 0x01;
		blk[2] = 0x01; blk[3] = 0x04; blk[4] = 0x01;
		core_cmd_process(&reg, blk, 1, 0);
		sched_expect((blk[1] & CORE_CMD_FAILED) != 0,
		             "an over-long command is marked", 1, 1, &bad);
		sched_expect(blk[5] == 0x00 && (blk[3] & CORE_CMD_FAILED) == 0,
		             "AND STOPS THE WALK", blk[5], 0x00, &bad);

		/*
		 * A missing device marks and CARRIES ON. Index 2 has no
		 * adapter; the entry after it addresses index 3, also missing,
		 * so both must be marked - which proves the walk continued.
		 */
		memset(blk, 0, sizeof(blk));
		blk[0] = CORE_CMD_PAD;
		blk[1] = CORE_CMD_PAD;      /* index is now 2 */
		blk[2] = 0x01; blk[3] = 0x04; blk[4] = 0x01;
		blk[9] = 0x01; blk[10] = 0x04; blk[11] = 0x01;
		blk[16] = CORE_CMD_END;
		core_cmd_process(&reg, blk, 1, 0);
		sched_expect((blk[3] & CORE_CMD_FAILED) != 0 &&
		             (blk[10] & CORE_CMD_FAILED) != 0,
		             "a missing device marks and carries on",
		             blk[10] & CORE_CMD_FAILED, CORE_CMD_FAILED, &bad);
		groups++;
	}

	/* ---- 10. the identify updates the pak state and keep-alive ----- */
	{
		cmd_setup_one(&reg, &ent, &cs);
		cs.keepalive_time  = 12345;
		cs.emu_pak_present = 0;

		/* three reply bytes with bit 0 set: an accessory is present */
		memset(blk, 0, sizeof(blk));
		blk[0] = 0x01; blk[1] = 0x03; blk[2] = 0x00;
		/*
		 * NOTE THE ORDER. The transport status byte comes first, then
		 * the joybus reply AS THE ADAPTER RETURNS IT - reversed. A
		 * controller identifies as 05 00 <status> on the bus, so over
		 * USB that is <status> 00 05, and the driver reverses it back.
		 */
		g_cmd_reply[0] = 0x04;      /* transport status, non-zero = ok */
		g_cmd_reply[1] = 0x01;      /* joybus status, bit 0 set        */
		g_cmd_reply[2] = 0x00;
		g_cmd_reply[3] = 0x05;
		g_cmd_reply_len = 4;
		core_cmd_process(&reg, blk, 1, 0);
		sched_expect(cs.emu_pak_present == 1,
		             "bit 0 of the status byte is pak present",
		             cs.emu_pak_present, 1, &bad);
		sched_expect(cs.keepalive_time == 12345,
		             "and bits 0..1 of 01 leave the keep-alive alone",
		             (long)cs.keepalive_time, 12345, &bad);
		sched_expect(blk[3] == 0x05 && blk[4] == 0x00 && blk[5] == 0x01,
		             "the reply is back in joybus order", blk[3], 0x05,
		             &bad);

		/* bits 0..1 == 11: the accessory changed, drop the keep-alive */
		cmd_reset_log();
		memset(blk, 0, sizeof(blk));
		blk[0] = 0x01; blk[1] = 0x03; blk[2] = 0x00;
		g_cmd_reply[0] = 0x04;
		g_cmd_reply[1] = 0x03;      /* bits 0..1 both set: it changed */
		g_cmd_reply[2] = 0x00;
		g_cmd_reply[3] = 0x05;
		g_cmd_reply_len = 4;
		core_cmd_process(&reg, blk, 1, 0);
		sched_expect(cs.keepalive_time == 0,
		             "a changed accessory drops the keep-alive",
		             (long)cs.keepalive_time, 0, &bad);
		groups++;
	}

	/* ---- 11. the read and write channel ---------------------------- */
	{
		cmd_setup_one(&reg, &ent, &cs);
		core_cmd_channel_init(&ch);
		cs.raw[0] = 0x11; cs.raw[1] = 0x22;
		cs.raw[3] = 0x33; cs.raw[4] = 0x44;

		/* a one-byte read is the adapter count */
		memset(blk, 0, sizeof(blk));
		st = core_cmd_read(&reg, &ch, blk, 1, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS && blk[0] == 1 && info == 1,
		             "a one-byte read is the adapter count", blk[0], 1,
		             &bad);

		/* write a block asking for the cached controller read */
		memset(blk, 0, sizeof(blk));
		blk[0] = 0x01; blk[1] = 0x04; blk[2] = 0x01;
		blk[7] = CORE_CMD_END;
		blk[CORE_CMD_GO] = 1;
		st = core_cmd_write(&reg, &ch, blk, CORE_CMD_BLOCK_BYTES, 0,
		                    &info);
		sched_expect(st == CORE_ST_SUCCESS &&
		             info == CORE_CMD_BLOCK_BYTES,
		             "the write is accepted", (long)info,
		             CORE_CMD_BLOCK_BYTES, &bad);
		sched_expect(ch.swap_bytes == 0, "little-endian client",
		             ch.swap_bytes, 0, &bad);

		/* read it back: pass 1 runs and the go flag is cleared */
		memset(blk, 0, sizeof(blk));
		st = core_cmd_read(&reg, &ch, blk, CORE_CMD_BLOCK_BYTES, 0,
		                   &info);
		sched_expect(blk[3] == 0x44 && blk[6] == 0x22,
		             "the read answers from the cache", blk[3], 0x44,
		             &bad);
		sched_expect(blk[CORE_CMD_GO] == 0,
		             "the go flag is cleared in the copy",
		             blk[CORE_CMD_GO], 0, &bad);
		sched_expect(ch.block[CORE_CMD_GO] == 1,
		             "BUT NOT IN THE CHANNEL, so a re-read re-polls",
		             ch.block[CORE_CMD_GO], 1, &bad);

		/* and it does re-poll - a fresh packet shows up next read */
		cs.raw[4] = 0x99;
		memset(blk, 0, sizeof(blk));
		core_cmd_read(&reg, &ch, blk, CORE_CMD_BLOCK_BYTES, 0, &info);
		sched_expect(blk[3] == 0x99,
		             "a second read sees the newer packet", blk[3],
		             0x99, &bad);
		groups++;
	}

	/* ---- 12. the byte-order word is also the go flag --------------- */
	{
		cmd_setup_one(&reg, &ent, &cs);
		core_cmd_channel_init(&ch);
		cs.raw[0] = 0xA1; cs.raw[1] = 0xA2;
		cs.raw[3] = 0xA3; cs.raw[4] = 0xA4;

		/*
		 * A big-endian client's block, written as it arrives: every
		 * dword reversed. The entry 01 04 01 .. occupies dword 0, so
		 * big-endian it is 00 01 04 01 at bytes 0..3 - and the
		 * byte-order word is 00 00 00 01 at 0x3C, whose leading byte
		 * is the 1 the driver looks for.
		 */
		memset(blk, 0, sizeof(blk));
		blk[0] = 0x00; blk[1] = 0x01; blk[2] = 0x04; blk[3] = 0x01;
		/* the terminator sits at byte 7 unswapped, so dword 1 */
		blk[4] = CORE_CMD_END;
		blk[CORE_CMD_SWAP_AT + 0] = 0x01;

		st = core_cmd_write(&reg, &ch, blk, CORE_CMD_BLOCK_BYTES, 0,
		                    &info);
		sched_expect(st == CORE_ST_SUCCESS && ch.swap_bytes == 1,
		             "a 1 at 0x3C selects byte-swapped", ch.swap_bytes,
		             1, &bad);
		sched_expect(ch.block[0] == 0x01 && ch.block[1] == 0x04,
		             "the block is swapped on the way in", ch.block[0],
		             0x01, &bad);
		sched_expect(ch.block[CORE_CMD_GO] == 1,
		             "AND THE SWAP WORD BECOMES THE GO FLAG",
		             ch.block[CORE_CMD_GO], 1, &bad);

		/* the read comes back swapped too */
		memset(blk, 0, sizeof(blk));
		core_cmd_read(&reg, &ch, blk, CORE_CMD_BLOCK_BYTES, 0, &info);
		/*
		 * The cached answer went to byte 3 of the block, and byte 3 of
		 * a dword becomes byte 0 when it is reversed - so a big-endian
		 * client reads its reply out of the same place it would have
		 * put the command.
		 */
		sched_expect(blk[0] == 0xA4 && blk[1] == 0x01,
		             "and the reply is swapped on the way out", blk[0],
		             0xA4, &bad);
		sched_expect(blk[3] == 0x01 && blk[2] == 0x04,
		             "with the command bytes reversed with it", blk[3],
		             0x01, &bad);
		groups++;
	}

	/* ---- 13. what the channel refuses ------------------------------ */
	{
		cmd_setup_one(&reg, &ent, &cs);
		core_cmd_channel_init(&ch);

		sched_expect(core_cmd_read(&reg, &ch, blk, 0x41, 0, &info) ==
		             CORE_ST_INVALID_PARAM,
		             "a read longer than a block is refused", 1, 1,
		             &bad);
		sched_expect(core_cmd_write(&reg, &ch, blk, 0x20, 0, &info) ==
		             CORE_ST_INVALID_PARAM,
		             "so is a short write", 1, 1, &bad);
		sched_expect(core_cmd_read(&reg, &ch, blk, 0, 0, &info) ==
		             CORE_ST_SUCCESS,
		             "a zero-length read succeeds", 1, 1, &bad);

		/* with no adapter, only the one-byte read is allowed */
		reg.live_count = 0;
		sched_expect(core_cmd_read(&reg, &ch, blk, 1, 0, &info) ==
		             CORE_ST_SUCCESS && blk[0] == 0,
		             "the count is readable with no adapter", blk[0],
		             0, &bad);
		sched_expect(core_cmd_read(&reg, &ch, blk,
		                           CORE_CMD_BLOCK_BYTES, 0, &info) ==
		             CORE_ST_NO_SUCH_DEVICE,
		             "but a block read is not", 1, 1, &bad);
		sched_expect(core_cmd_write(&reg, &ch, blk,
		                            CORE_CMD_BLOCK_BYTES, 0, &info) ==
		             CORE_ST_NO_SUCH_DEVICE,
		             "and neither is a write", 1, 1, &bad);
		groups++;
	}

	hlog("SDK command block      : %s (%d groups)\n", bad ? "FAIL" : "ok",
	     groups);
	return bad;
}

/* ======================================================================
 * POWER, THE CONTROL DEVICE AND THE NOTIFICATION IRPS
 *
 * The three things stage five added that are decisions rather than
 * plumbing. What is checked here is the ORDER of the power protocol, the
 * system-to-device mapping rule that is easy to get wrong, the two counts
 * that between them decide when the control device dies, and the claim
 * handshake on a cancelled waiter.
 * ====================================================================== */

static DEVICE_OBJECT     g_pw_dev;
static ADAPTOID_DEVEXT   g_pw_ext;
static HID_DEVICE_EXTENSION g_pw_hid;
static DEVICE_OBJECT     g_pw_lower;
static IO_STACK_LOCATION g_pw_sp;
static IRP               g_pw_irp;

static void power_setup(void)
{
	memset(&g_pw_ext, 0, sizeof(g_pw_ext));
	AdaptoidDevExtInit(&g_pw_ext);
	g_pw_ext.NextDeviceObject     = &g_pw_lower;
	g_pw_ext.PhysicalDeviceObject = &g_pw_lower;
	g_pw_ext.Started              = 1;
	g_pw_ext.DevicePowerState     = ADAPTOID_POWER_D0;
	/* AdaptoidDevExtInit leaves polling stopped for PnP, as it should;
	 * these tests are about a device that is already running. */
	g_pw_ext.PollStopMask         = 0;

	g_pw_hid.MiniDeviceExtension = &g_pw_ext;
	g_pw_dev.DeviceExtension     = &g_pw_hid;
	/* AdaptoidDevExtOf is a harness stub that answers from here rather
	 * than walking hidclass's extension, so it has to be pointed at this
	 * device before any dispatch entry point is called. */
	g_pnp_devext = &g_pw_ext;

	memset(&g_pw_irp, 0, sizeof(g_pw_irp));
	memset(&g_pw_sp, 0, sizeof(g_pw_sp));
	g_pw_sp.MajorFunction         = IRP_MJ_POWER;
	g_pw_irp.CurrentStackLocation = &g_pw_sp;
	g_pw_irp.NextStackLocation    = &g_pw_sp;

	power_reset();
	g_power_requests       = 0;
	g_power_complete       = NULL;
	g_power_completion     = NULL;
	g_irp_count            = 0;
}

static void power_irp(UCHAR minor, ULONG type, ULONG state)
{
	g_pw_sp.MajorFunction        = IRP_MJ_POWER;
	g_pw_sp.MinorFunction        = minor;
	g_pw_sp.Parameters.Power.Type  = (POWER_STATE_TYPE)type;
	g_pw_sp.Parameters.Power.State.DeviceState =
	        (DEVICE_POWER_STATE)state;
	g_pw_irp.IoStatus.Status      = 0;
	g_pw_irp.IoStatus.Information = 0;
}

/* Does the log start with "next down"? That is the power protocol. */
static int power_next_before_down(void)
{
	return strncmp(g_power_log, "next down", 9) == 0;
}

static int test_power_and_control(void)
{
	int bad    = 0;
	int groups = 0;

	/* ---- 1. the system-to-device mapping rule ---------------------- */
	{
		power_setup();
		g_pw_ext.Capabilities.DeviceState[2] = 2;   /* S1 -> D2 */
		g_pw_ext.Capabilities.DeviceState[3] = 3;
		g_pw_ext.Capabilities.DeviceWake     = 3;

		sched_expect(AdaptoidDeviceStateFor(&g_pw_ext,
		                                    ADAPTOID_POWER_S0) ==
		             ADAPTOID_POWER_D0,
		             "the working system state is always D0", 1, 1,
		             &bad);

		/*
		 * THE RULE THAT IS EASY TO MISS. With no WAIT_WAKE armed the
		 * device goes all the way to D3 whatever the capability table
		 * says, because a light sleep buys nothing if it cannot wake
		 * the machine.
		 */
		g_pw_ext.WaitWakePending = 0;
		sched_expect(AdaptoidDeviceStateFor(&g_pw_ext, 2) ==
		             ADAPTOID_POWER_D3,
		             "with no wake armed it drops straight to D3",
		             AdaptoidDeviceStateFor(&g_pw_ext, 2),
		             ADAPTOID_POWER_D3, &bad);

		g_pw_ext.WaitWakePending = 1;
		sched_expect(AdaptoidDeviceStateFor(&g_pw_ext, 2) == 2,
		             "with it armed the capability table decides",
		             AdaptoidDeviceStateFor(&g_pw_ext, 2), 2, &bad);
		sched_expect(AdaptoidDeviceStateFor(&g_pw_ext, 3) == 3,
		             "and it is indexed by the system state",
		             AdaptoidDeviceStateFor(&g_pw_ext, 3), 3, &bad);

		/* out of range is bounded, which the original is not */
		sched_expect(AdaptoidDeviceStateFor(&g_pw_ext, 99) ==
		             ADAPTOID_POWER_D3,
		             "an out-of-range system state is bounded", 1, 1,
		             &bad);
		groups++;
	}

	/* ---- 2. a device power IRP down stops polling BEFORE passing on - */
	{
		power_setup();
		AdaptoidPollStart(&g_pw_ext, 0);
		sched_expect(g_pw_ext.PollStopMask == 0, "polling is running",
		             (long)g_pw_ext.PollStopMask, 0, &bad);

		power_irp(IRP_MN_SET_POWER, DevicePowerState, ADAPTOID_POWER_D3);
		AdaptoidPower(&g_pw_dev, &g_pw_irp);

		sched_expect((g_pw_ext.PollStopMask &
		              ADAPTOID_STOP_REASON_POWER) != 0,
		             "going down stops the poll", 1, 1, &bad);
		sched_expect(g_pw_ext.DevicePowerState == ADAPTOID_POWER_D3,
		             "and records the new state",
		             (long)g_pw_ext.DevicePowerState,
		             ADAPTOID_POWER_D3, &bad);
		sched_expect(power_next_before_down(),
		             "PoStartNextPowerIrp came before PoCallDriver", 1,
		             1, &bad);
		sched_expect(strstr(g_power_log, "completed") == NULL,
		             "and no completion routine was attached", 1, 1,
		             &bad);
		groups++;
	}

	/* ---- 3. coming back up restarts it IN THE COMPLETION ------------ */
	{
		power_setup();
		AdaptoidPollStart(&g_pw_ext, 0);
		power_irp(IRP_MN_SET_POWER, DevicePowerState, ADAPTOID_POWER_D3);
		AdaptoidPower(&g_pw_dev, &g_pw_irp);

		power_reset();
		power_irp(IRP_MN_SET_POWER, DevicePowerState, ADAPTOID_POWER_D0);
		AdaptoidPower(&g_pw_dev, &g_pw_irp);

		sched_expect(strstr(g_power_log, "completed") != NULL,
		             "coming up attaches a completion routine", 1, 1,
		             &bad);
		sched_expect((g_pw_ext.PollStopMask &
		              ADAPTOID_STOP_REASON_POWER) == 0,
		             "which is where the poll is restarted", 1, 1,
		             &bad);
		sched_expect(g_pw_ext.DevicePowerState == ADAPTOID_POWER_D0,
		             "and where D0 is recorded",
		             (long)g_pw_ext.DevicePowerState,
		             ADAPTOID_POWER_D0, &bad);
		groups++;
	}

	/* ---- 4. a system IRP is parked until the device one finishes ---- */
	{
		power_setup();
		g_pw_ext.WaitWakePending = 1;
		g_pw_ext.Capabilities.DeviceState[3] = 3;

		power_irp(IRP_MN_SET_POWER, SystemPowerState, 3);
		AdaptoidPower(&g_pw_dev, &g_pw_irp);

		sched_expect(g_power_requests == 1 && g_power_requested == 3,
		             "a device power IRP is requested for D3",
		             (long)g_power_requested, 3, &bad);
		sched_expect(g_pw_ext.PendingSystemPowerIrp == &g_pw_irp,
		             "and the system IRP is PARKED, not completed", 1,
		             1, &bad);
		sched_expect(strstr(g_power_log, "down") == NULL,
		             "nothing went down yet", 1, 1, &bad);

		/* the bus finishes the device IRP */
		power_run_completion();
		sched_expect(g_pw_ext.PendingSystemPowerIrp == NULL,
		             "the completion releases the parked IRP", 1, 1,
		             &bad);
		sched_expect(strstr(g_power_log, "next down") != NULL,
		             "and only then passes it down, next first", 1, 1,
		             &bad);

		/* asking for the state it is already in skips the request */
		power_setup();
		g_pw_ext.WaitWakePending = 1;
		g_pw_ext.Capabilities.DeviceState[3] = ADAPTOID_POWER_D0;
		power_irp(IRP_MN_SET_POWER, SystemPowerState, 3);
		AdaptoidPower(&g_pw_dev, &g_pw_irp);
		sched_expect(g_power_requests == 0,
		             "no request when already in the right state",
		             g_power_requests, 0, &bad);
		sched_expect(power_next_before_down(),
		             "it is just passed down", 1, 1, &bad);
		groups++;
	}

	/* ---- 5. wait-wake is refused when it cannot help ---------------- */
	{
		power_setup();
		g_pw_ext.DevicePowerState        = ADAPTOID_POWER_D0;
		g_pw_ext.Capabilities.DeviceWake = 3;

		power_irp(IRP_MN_WAIT_WAKE, 0, 0);
		sched_expect(AdaptoidPower(&g_pw_dev, &g_pw_irp) ==
		             STATUS_INVALID_DEVICE_STATE,
		             "wake is refused while the device is in D0", 1, 1,
		             &bad);
		sched_expect(g_pw_ext.WaitWakePending == 0,
		             "and nothing is armed",
		             (long)g_pw_ext.WaitWakePending, 0, &bad);

		/*
		 * AND SEPARATELY, with a wake state that would otherwise be
		 * acceptable. Without this case the first half of the
		 * condition is never the one doing the refusing, and a
		 * reimplementation could drop it unnoticed.
		 */
		power_setup();
		g_pw_ext.DevicePowerState        = ADAPTOID_POWER_D0;
		g_pw_ext.Capabilities.DeviceWake = ADAPTOID_POWER_D0;
		power_irp(IRP_MN_WAIT_WAKE, 0, 0);
		sched_expect(AdaptoidPower(&g_pw_dev, &g_pw_irp) ==
		             STATUS_INVALID_DEVICE_STATE,
		             "being in D0 alone is enough to refuse it", 1, 1,
		             &bad);

		/* deeper than the hardware can wake from is refused too */
		power_setup();
		g_pw_ext.DevicePowerState        = 2;
		g_pw_ext.Capabilities.DeviceWake = 3;
		power_irp(IRP_MN_WAIT_WAKE, 0, 0);
		sched_expect(AdaptoidPower(&g_pw_dev, &g_pw_irp) ==
		             STATUS_INVALID_DEVICE_STATE,
		             "and when the device is shallower than its wake "
		             "state", 1, 1, &bad);

		/* but accepted when both conditions hold */
		power_setup();
		g_pw_ext.DevicePowerState        = 3;
		g_pw_ext.Capabilities.DeviceWake = 2;
		power_irp(IRP_MN_WAIT_WAKE, 0, 0);
		AdaptoidPower(&g_pw_dev, &g_pw_irp);
		sched_expect(power_next_before_down(),
		             "otherwise it is armed and passed down", 1, 1,
		             &bad);
		sched_expect(g_pw_ext.WakeIdleDeviceState == 2,
		             "with the wake state recorded",
		             (long)g_pw_ext.WakeIdleDeviceState, 2, &bad);
		groups++;
	}

	/* ---- 6. the remove lock is balanced on every path --------------- */
	{
		static const struct { UCHAR minor; ULONG type; ULONG state; }
		vec[] = {
			{ IRP_MN_SET_POWER, DevicePowerState, ADAPTOID_POWER_D3 },
			{ IRP_MN_SET_POWER, DevicePowerState, ADAPTOID_POWER_D0 },
			{ IRP_MN_SET_POWER, SystemPowerState, ADAPTOID_POWER_S0 },
			{ 0x7F,             0,                0 }
		};
		int i;

		for (i = 0; i < 4; i++) {
			LONG before;

			power_setup();
			before = g_pw_ext.RemoveLockB.IoCount;
			power_irp(vec[i].minor, vec[i].type, vec[i].state);
			AdaptoidPower(&g_pw_dev, &g_pw_irp);
			sched_expect(g_pw_ext.RemoveLockB.IoCount == before,
			             "the power path balances its remove lock",
			             g_pw_ext.RemoveLockB.IoCount, before,
			             &bad);
		}

		/* and a device being removed refuses the IRP outright */
		power_setup();
		g_pw_ext.RemoveLockA.Removed = 1;
		g_pw_ext.RemoveLockB.Removed = 1;
		power_irp(IRP_MN_SET_POWER, DevicePowerState, ADAPTOID_POWER_D3);
		sched_expect(!NT_SUCCESS(AdaptoidPower(&g_pw_dev, &g_pw_irp)),
		             "a removed device refuses power", 1, 1, &bad);
		sched_expect(strstr(g_power_log, "next") != NULL,
		             "still calling PoStartNextPowerIrp first", 1, 1,
		             &bad);
		groups++;
	}

	/* ---- 7. the idle gate that can never wake ---------------------- */
	{
		power_setup();
		g_pw_ext.WakeIdleDeviceState = 3;
		g_pw_ext.AbortedPipeCount    = 0;

		/* The request reaches the bus, so the status is the bus's -
		 * what matters is that a request was made at all. */
		AdaptoidUpdateIdlePower(&g_pw_ext, 1);
		sched_expect(g_power_requests == 1,
		             "idling is allowed with no aborted pipes",
		             g_power_requests, 1, &bad);
		sched_expect(g_power_requested == 3, "into the wake state",
		             (long)g_power_requested, 3, &bad);

		/*
		 * THE DEAD HALF. Waking needs AbortedPipeCount non-zero, and
		 * nothing in the driver ever increments it - so this can never
		 * fire in the original either. Checked so that the day someone
		 * "fixes" the counter, the change is visible.
		 */
		power_setup();
		g_pw_ext.WakeIdleDeviceState = 3;
		g_pw_ext.AbortedPipeCount    = 0;
		sched_expect(AdaptoidUpdateIdlePower(&g_pw_ext, 0) ==
		             STATUS_SUCCESS && g_power_requests == 0,
		             "waking is refused with the count at zero",
		             g_power_requests, 0, &bad);

		/* a request already in flight blocks another */
		power_setup();
		g_pw_ext.WakeIdleDeviceState   = 3;
		g_pw_ext.PowerRequestInProgress = 1;
		sched_expect(AdaptoidUpdateIdlePower(&g_pw_ext, 1) ==
		             STATUS_SUCCESS && g_power_requests == 0,
		             "and so is a second request", g_power_requests, 0,
		             &bad);

		/* as does a device that is not ready */
		power_setup();
		g_pw_ext.WakeIdleDeviceState = 3;
		g_pw_ext.Started             = 0;
		sched_expect(AdaptoidUpdateIdlePower(&g_pw_ext, 1) ==
		             STATUS_DELETE_PENDING,
		             "an unstarted device is not idled", 1, 1, &bad);
		groups++;
	}

	/* ---- 8. the enable keep-alive window ---------------------------- */
	{
		power_setup();
		core_init(&g_pw_ext.Core, harness_sink, &g_pw_ext);
		g_urb_count = 0;
		g_urb_ret   = STATUS_PENDING;

		/* nothing sent yet, so the window is long past */
		g_pw_ext.Core.keepalive_time = 0;
		clock_advance_ms(10000);
		AdaptoidSetDeviceEnable(&g_pw_ext, 1);
		sched_expect(g_urb_setup.bRequest ==
		             ADAPTOID_ENABLE_START_REQUEST,
		             "outside the window it is the full start",
		             g_urb_setup.bRequest,
		             ADAPTOID_ENABLE_START_REQUEST, &bad);
		sched_expect(g_pw_ext.Core.keepalive_time != 0,
		             "and the window is opened", 1, 1, &bad);

		/* inside it, one short kick and no re-initialisation */
		g_pw_ext.Vendor.State = 0;
		clock_advance_ms(1000);
		AdaptoidSetDeviceEnable(&g_pw_ext, 1);
		sched_expect(g_urb_setup.bRequest ==
		             ADAPTOID_ENABLE_KICK_REQUEST &&
		             g_urb_setup.wIndex == ADAPTOID_ENABLE_KICK_INDEX,
		             "inside it, only the short kick",
		             g_urb_setup.bRequest,
		             ADAPTOID_ENABLE_KICK_REQUEST, &bad);

		/* past three seconds it starts over */
		g_pw_ext.Vendor.State = 0;
		clock_advance_ms(3001);
		AdaptoidSetDeviceEnable(&g_pw_ext, 1);
		sched_expect(g_urb_setup.bRequest ==
		             ADAPTOID_ENABLE_START_REQUEST,
		             "past three seconds it starts over",
		             g_urb_setup.bRequest,
		             ADAPTOID_ENABLE_START_REQUEST, &bad);
		groups++;
	}

	/* ---- 9. a busy slot: off defers, on is refused ------------------ */
	{
		power_setup();
		core_init(&g_pw_ext.Core, harness_sink, &g_pw_ext);
		core_set_vendor(&g_pw_ext.Core, harness_vendor, 0);
		g_urb_count = 0;
		g_urb_ret   = STATUS_PENDING;

		g_pw_ext.Vendor.State = 1;          /* somebody else has it */
		sched_expect(AdaptoidSetDeviceEnable(&g_pw_ext, 1) ==
		             STATUS_DEVICE_BUSY,
		             "switching on a busy device is refused", 1, 1,
		             &bad);
		sched_expect(g_pw_ext.Core.claim_idle_command == 0,
		             "and nothing is deferred",
		             g_pw_ext.Core.claim_idle_command, 0, &bad);

		sched_expect(AdaptoidSetDeviceEnable(&g_pw_ext, 0) ==
		             STATUS_DEVICE_BUSY,
		             "switching off is refused too", 1, 1, &bad);
		sched_expect(g_pw_ext.Core.claim_idle_command == 1,
		             "BUT IT IS DEFERRED - a lost off leaves a motor "
		             "running", g_pw_ext.Core.claim_idle_command, 1,
		             &bad);

		/* and the deferred drain picks it up first */
		g_pw_ext.Core.claim_effect_tick = 1;
		g_bus_count = 0;
		g_bus_busy  = 0;
		core_effect_run_deferred(&g_pw_ext.Core, KeQueryInterruptTime());
		sched_expect(g_bus_cur.bRequest == CORE_FX_CMD_STOP &&
		             g_bus_cur.wIndex == CORE_FX_IDLE_INDEX,
		             "the drain sends the idle command before the tick",
		             g_bus_cur.wIndex, CORE_FX_IDLE_INDEX, &bad);
		sched_expect(g_pw_ext.Core.claim_idle_command == 0,
		             "clearing its claim",
		             g_pw_ext.Core.claim_idle_command, 0, &bad);
		groups++;
	}

	/* ---- 10. the control device's two reference counts -------------- */
	{
		PADAPTOID_CDO_EXT cx;
		DRIVER_OBJECT     drv;
		IRP               irp;
		IO_STACK_LOCATION sp;

		memset(&drv, 0, sizeof(drv));
		g_devices_created = g_devices_deleted = 0;
		g_links_created   = g_links_deleted   = 0;

		sched_expect(NT_SUCCESS(AdaptoidCreateControlDevice(&drv)) &&
		             g_devices_created == 1 && g_links_created == 1,
		             "the first adapter creates the control device",
		             g_devices_created, 1, &bad);
		cx = AdaptoidControlDeviceExt();
		sched_expect(cx != NULL && cx->Magic[0] == ADAPTOID_CDO_MAGIC0,
		             "with its magic in place", 1, 1, &bad);

		sched_expect(NT_SUCCESS(AdaptoidCreateControlDevice(&drv)) &&
		             g_devices_created == 1,
		             "a second adapter shares it", g_devices_created, 1,
		             &bad);

		AdaptoidReleaseControlDevice();
		sched_expect(g_devices_deleted == 0,
		             "one leaving does not delete it",
		             g_devices_deleted, 0, &bad);
		AdaptoidReleaseControlDevice();
		sched_expect(g_devices_deleted == 1 && g_links_deleted == 1,
		             "the last one does", g_devices_deleted, 1, &bad);

		/*
		 * AND THE OTHER WAY ROUND: an open handle keeps the device
		 * alive past the last adapter. Both counts have to reach zero,
		 * and either can be last.
		 */
		g_devices_created = g_devices_deleted = 0;
		AdaptoidCreateControlDevice(&drv);
		cx = AdaptoidControlDeviceExt();
		cx->Registry.live_count = 1;

		memset(&irp, 0, sizeof(irp));
		memset(&sp, 0, sizeof(sp));
		irp.CurrentStackLocation = &sp;
		irp.NextStackLocation    = &sp;
		sched_expect(NT_SUCCESS(AdaptoidControlCreate(cx->Self, &irp)) &&
		             cx->OpenCount == 1,
		             "a handle opens", cx->OpenCount, 1, &bad);

		AdaptoidReleaseControlDevice();
		sched_expect(g_devices_deleted == 0,
		             "the last adapter leaving does not delete it "
		             "while a handle is open", g_devices_deleted, 0,
		             &bad);
		AdaptoidControlClose(cx->Self, &irp);
		sched_expect(g_devices_deleted == 1,
		             "closing the handle does", g_devices_deleted, 1,
		             &bad);
		groups++;
	}

	/* ---- 11. a failed creation does not count as a user ------------- */
	{
		DRIVER_OBJECT drv;

		memset(&drv, 0, sizeof(drv));
		g_devices_created = g_devices_deleted = 0;
		g_links_created   = g_links_deleted   = 0;

		g_create_device_fails = 1;
		sched_expect(!NT_SUCCESS(AdaptoidCreateControlDevice(&drv)),
		             "a failed creation reports failure", 1, 1, &bad);
		g_create_device_fails = 0;

		/*
		 * THE ORIGINAL COUNTS IT ANYWAY. drv_CreateControlDevice
		 * increments its reference count outside the success test, so
		 * a device that was never made has a user. Here the next
		 * successful creation must still be the FIRST one.
		 */
		sched_expect(NT_SUCCESS(AdaptoidCreateControlDevice(&drv)),
		             "and a later one succeeds", 1, 1, &bad);
		AdaptoidReleaseControlDevice();
		sched_expect(g_devices_deleted == 1,
		             "with a balanced count - one release deletes it",
		             g_devices_deleted, 1, &bad);

		/* a symlink failure unwinds the device too */
		g_devices_created = g_devices_deleted = 0;
		g_create_link_fails = 1;
		sched_expect(!NT_SUCCESS(AdaptoidCreateControlDevice(&drv)) &&
		             g_devices_created == 1 && g_devices_deleted == 1,
		             "a symlink failure unwinds the device",
		             g_devices_deleted, 1, &bad);
		g_create_link_fails = 0;
		groups++;
	}

	/* ---- 12. notification IRPs: park, deliver, cancel --------------- */
	{
		PADAPTOID_CDO_EXT cx;
		DRIVER_OBJECT     drv;
		IRP               irp[3];
		IO_STACK_LOCATION sp[3];
		FILE_OBJECT       fo[2];
		UCHAR             buf[3][CORE_NOTIFY_BYTES];
		int               i;

		memset(&drv, 0, sizeof(drv));
		AdaptoidCreateControlDevice(&drv);
		cx = AdaptoidControlDeviceExt();
		cx->Registry.live_count = 1;

		for (i = 0; i < 3; i++) {
			memset(&irp[i], 0, sizeof(irp[i]));
			memset(&sp[i], 0, sizeof(sp[i]));
			irp[i].CurrentStackLocation = &sp[i];
			irp[i].NextStackLocation    = &sp[i];
			irp[i].SystemBuffer         = buf[i];
			sp[i].FileObject            = &fo[i < 2 ? 0 : 1];
			memset(buf[i], 0, sizeof(buf[i]));
		}
		g_irp_count = 0;

		sched_expect(AdaptoidWaitNotification(cx, &irp[0]) ==
		             STATUS_PENDING,
		             "a waiter with no event parks", 1, 1, &bad);
		sched_expect(irp[0].PendingReturned != 0,
		             "marked pending", 1, 1, &bad);
		sched_expect(irp[0].CancelRoutine != NULL,
		             "with a cancel routine", 1, 1, &bad);
		sched_expect(g_irp_count == 0, "and not completed",
		             g_irp_count, 0, &bad);

		/* an event finds it */
		core_notify_post(&cx->Registry.notify, CORE_EVENT_DEBUG, 7, 0);
		sched_expect(g_irp_count == 1,
		             "an event completes the parked waiter",
		             g_irp_count, 1, &bad);
		sched_expect(irp[0].IoStatus.Information == CORE_NOTIFY_BYTES,
		             "with twelve bytes",
		             (long)irp[0].IoStatus.Information,
		             CORE_NOTIFY_BYTES, &bad);
		sched_expect(buf[0][4] == 7,
		             "carrying the event's argument", buf[0][4], 7,
		             &bad);

		/*
		 * CANCELLATION SELECTS BY FILE OBJECT. Waiters 0 and 1 share
		 * one handle, waiter 2 has another; cancelling by waiter 1's
		 * IRP must take only the first handle's.
		 */
		g_irp_count = 0;
		AdaptoidWaitNotification(cx, &irp[0]);
		AdaptoidWaitNotification(cx, &irp[1]);
		AdaptoidWaitNotification(cx, &irp[2]);
		AdaptoidCancelNotifications(cx, &irp[1]);
		sched_expect(g_irp_count == 2,
		             "cancelling one handle takes only its waiters",
		             g_irp_count, 2, &bad);
		sched_expect(irp[2].IoStatus.Status != STATUS_CANCELLED,
		             "the other handle's is untouched", 1, 1, &bad);
		sched_expect(irp[0].IoStatus.Status == STATUS_CANCELLED,
		             "and the cancelled ones say so", 1, 1, &bad);

		/* NULL takes everything */
		g_irp_count = 0;
		AdaptoidCancelNotifications(cx, NULL);
		sched_expect(g_irp_count == 1,
		             "and NULL takes what is left", g_irp_count, 1,
		             &bad);

		/*
		 * A WAITER ALREADY CLAIMED IS DROPPED, NOT COMPLETED TWICE.
		 * g_claim_refuse is what a completion already in flight looks
		 * like from here - the harness owns AdaptoidClaimIrp, so
		 * clearing the IRP's cancel routine by hand would prove
		 * nothing.
		 */
		g_irp_count    = 0;
		g_claim_refuse = 1;
		AdaptoidWaitNotification(cx, &irp[0]);
		AdaptoidCancelNotifications(cx, NULL);
		g_claim_refuse = 0;
		sched_expect(g_irp_count == 0,
		             "a waiter already claimed is not completed twice",
		             g_irp_count, 0, &bad);

		cx->Registry.live_count = 0;
		AdaptoidReleaseControlDevice();
		groups++;
	}

	hlog("Power and control dev  : %s (%d groups)\n", bad ? "FAIL" : "ok",
	     groups);
	return bad;
}

/* ======================================================================
 * THE HID MINIDRIVER CONTRACT
 *
 * The first group is the one that matters: the descriptor this driver
 * hands Windows must be BYTE IDENTICAL to the original's, because it is
 * the device's identity. Everything a user has bound, every game's saved
 * configuration and the device's name in Control Panel follow from it.
 *
 * The vector below is the 185 bytes at 00019b40, read out of the Ghidra
 * database. The descriptor in core.c was written out item by item from
 * docs/hid-descriptor.txt section 4, so this is a conformance check of one
 * against the other and not a copy compared with itself.
 *
 * The image is the only source for these bytes - nothing shipped carries a
 * second copy - which is why group 2 also parses the result structurally. A
 * transcription slip that still parsed as three balanced collections with
 * the right report IDs would have to be a deliberate one.
 * ====================================================================== */

static const u8 HID_DESC_ORIGINAL[185] = {
	0x05,0x01,0x09,0x02,0xA1,0x01,0x09,0x01,0xA1,0x00,0x85,0x03,
	0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,0x25,0x01,0x75,0x01,
	0x95,0x03,0x81,0x02,0x75,0x05,0x95,0x01,0x81,0x01,0x05,0x01,
	0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7F,0x75,0x08,
	0x95,0x03,0x81,0x06,0xC0,0xC0,
	0x05,0x01,0x09,0x06,0xA1,0x01,0x85,0x02,0x05,0x07,0x19,0xE0,
	0x29,0xE7,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x08,0x81,0x02,
	0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x05,0x75,0x01,0x05,0x08,
	0x19,0x01,0x29,0x05,0x91,0x02,0x95,0x01,0x75,0x03,0x91,0x01,
	0x95,0x0A,0x75,0x08,0x05,0x07,0x19,0x00,0x2A,0xA5,0x00,0x15,
	0x00,0x26,0xA5,0x00,0x81,0x00,0xC0,
	0x05,0x01,0x09,0x04,0xA1,0x01,0x09,0x01,0xA1,0x00,0x85,0x01,
	0x05,0x01,0x09,0x30,0x09,0x31,0x16,0x50,0xFB,0x26,0xB0,0x04,
	0x36,0x00,0x00,0x46,0x60,0x09,0x75,0x0C,0x95,0x02,0x81,0x02,
	0xC0,0x05,0x09,0x19,0x01,0x29,0x0E,0x15,0x00,0x25,0x01,0x35,
	0x00,0x45,0x01,0x75,0x01,0x95,0x0E,0x81,0x02,0x95,0x02,0x75,
	0x01,0x81,0x01,0xC0
};

/*
 * Walk a report descriptor and pull out its shape: how many top-level
 * application collections it has, how deep the nesting goes, and which
 * report IDs appear. An INDEPENDENT check that the bytes are a well-formed
 * descriptor rather than merely the right ones.
 */
typedef struct hid_shape {
	int collections;        /* top-level Collection(Application)        */
	int depth_ok;           /* every Collection has an End Collection   */
	int report_ids[4];
	int report_id_count;
	int items;
} hid_shape;

static void hid_parse(const u8 *d, u32 len, hid_shape *sh)
{
	u32 i = 0;
	int depth = 0;

	memset(sh, 0, sizeof(*sh));
	sh->depth_ok = 1;

	while (i < len) {
		u8  item = d[i];
		u32 size = item & 0x03u;

		if (size == 3) {
			size = 4;                       /* the 3 means 4 */
		}
		sh->items++;

		/* Collection (0xA0), End Collection (0xC0), Report ID (0x84) */
		if ((item & 0xFCu) == 0xA0u) {
			if (depth == 0 && i + 1 < len && d[i + 1] == 0x01) {
				sh->collections++;
			}
			depth++;
		} else if ((item & 0xFCu) == 0xC0u) {
			depth--;
			if (depth < 0) {
				sh->depth_ok = 0;
			}
		} else if ((item & 0xFCu) == 0x84u) {
			if (sh->report_id_count < 4 && i + 1 < len) {
				sh->report_ids[sh->report_id_count++] =
				        d[i + 1];
			}
		}
		i += 1 + size;
	}
	if (depth != 0 || i != len) {
		sh->depth_ok = 0;       /* unbalanced, or ran off the end */
	}
}

static int test_hid_contract(void)
{
	int bad    = 0;
	int groups = 0;
	core_state cs;
	u8  out[256];
	u32 info;
	u32 st;
	int i;

	/* ---- 1. the descriptor is byte-identical to the original -------- */
	{
		const u8 *d;
		u32       len;
		int       diff = -1;

		memset(&cs, 0, sizeof(cs));
		cs.devices_mask = 7;

		d = core_hid_descriptor(cs.devices_mask, &len);
		sched_expect(len == 185, "the composite is 185 bytes", (long)len,
		             185, &bad);
		for (i = 0; i < 185; i++) {
			if (d[i] != HID_DESC_ORIGINAL[i]) {
				diff = i;
				break;
			}
		}
		sched_expect(diff < 0,
		             "AND IT MATCHES THE ORIGINAL BYTE FOR BYTE",
		             diff, -1, &bad);

		/*
		 * The two fragments are the prefix and the suffix of the
		 * whole. That is what lets one array serve all three.
		 */
		cs.devices_mask = 6;
		d = core_hid_descriptor(cs.devices_mask, &len);
		sched_expect(len == 121 && d == core_hid_descriptor(7, 0),
		             "mouse+keyboard is the PREFIX of the composite",
		             (long)len, 121, &bad);

		cs.devices_mask = 0;
		d = core_hid_descriptor(cs.devices_mask, &len);
		sched_expect(len == 64 && d == core_hid_descriptor(7, 0) + 121,
		             "and joystick is its SUFFIX", (long)len, 64, &bad);
		groups++;
	}

	/* ---- 2. it is a well-formed three-collection descriptor --------- */
	{
		hid_shape sh;

		hid_parse(core_hid_descriptor(7, 0), 185, &sh);
		sched_expect(sh.depth_ok,
		             "the composite parses and its collections balance",
		             sh.depth_ok, 1, &bad);
		sched_expect(sh.collections == 3,
		             "THREE top-level application collections",
		             sh.collections, 3, &bad);
		/*
		 * In descriptor order: mouse is 3, keyboard 2, joystick 1.
		 * The ORDER matters as much as the set - it is what fixes
		 * which collection each report ID belongs to.
		 */
		sched_expect(sh.report_id_count == 3 &&
		             sh.report_ids[0] == CORE_REPORT_MOUSE &&
		             sh.report_ids[1] == CORE_REPORT_KEYBOARD &&
		             sh.report_ids[2] == CORE_REPORT_JOYSTICK,
		             "report IDs 3, 2, 1 in that order",
		             sh.report_ids[0], CORE_REPORT_MOUSE, &bad);

		hid_parse(core_hid_descriptor(6, 0), 121, &sh);
		sched_expect(sh.depth_ok && sh.collections == 2,
		             "the mouse+keyboard fragment stands alone",
		             sh.collections, 2, &bad);

		hid_parse(core_hid_descriptor(0, 0), 64, &sh);
		sched_expect(sh.depth_ok && sh.collections == 1,
		             "and so does the joystick one", sh.collections, 1,
		             &bad);
		groups++;
	}

	/* ---- 3. the selection table, all eight indices ------------------ */
	{
		static const u32 want[8] = { 64, 64, 64, 64, 64, 64, 121, 185 };

		for (i = 0; i < 8; i++) {
			u32 len = 0;

			core_hid_descriptor((u32)i, &len);
			sched_expect(len == want[i],
			             "the selection table is reproduced exactly",
			             (long)len, (long)want[i], &bad);
		}
		/* Bits above the low three are ignored. */
		{
			u32 len = 0;

			core_hid_descriptor(0xFFFFFFF8u | 7u, &len);
			sched_expect(len == 185, "only the low three bits count",
			             (long)len, 185, &bad);
		}
		groups++;
	}

	/* ---- 4. GET_DEVICE_DESCRIPTOR ---------------------------------- */
	{
		memset(&cs, 0, sizeof(cs));
		cs.devices_mask = 7;
		memset(out, 0xAA, sizeof(out));

		st = core_hid_ioctl(&cs, CORE_HID_IOC_DEVICE_DESC, out, 9, 0, 0,
		                    &info);
		sched_expect(st == CORE_ST_SUCCESS && info == 9,
		             "nine bytes of HID descriptor", (long)info, 9,
		             &bad);
		sched_expect(out[0] == 9 && out[1] == 0x21,
		             "bLength and bDescriptorType", out[1], 0x21, &bad);
		sched_expect(out[2] == 0x01 && out[3] == 0x00,
		             "bcdHID is 0x0001, which the spec does not define",
		             out[2], 1, &bad);
		sched_expect(out[5] == 1 && out[6] == 0x22,
		             "one report descriptor, type 0x22", out[6], 0x22,
		             &bad);
		sched_expect(out[7] == 185 && out[8] == 0,
		             "and its length tracks the selection", out[7], 185,
		             &bad);

		/* the length follows the mask */
		cs.devices_mask = 0;
		core_hid_ioctl(&cs, CORE_HID_IOC_DEVICE_DESC, out, 9, 0, 0,
		               &info);
		sched_expect(out[7] == 64 && out[8] == 0,
		             "joystick only says 64", out[7], 64, &bad);

		sched_expect(core_hid_ioctl(&cs, CORE_HID_IOC_DEVICE_DESC, out,
		                            8, 0, 0, &info) ==
		             CORE_ST_BUFFER_TOO_SMALL,
		             "eight bytes is refused", 1, 1, &bad);
		groups++;
	}

	/* ---- 5. GET_REPORT_DESCRIPTOR ---------------------------------- */
	{
		memset(&cs, 0, sizeof(cs));
		cs.devices_mask = 7;
		memset(out, 0xAA, sizeof(out));

		st = core_hid_ioctl(&cs, CORE_HID_IOC_REPORT_DESC, out,
		                    sizeof(out), 0, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS && info == 185,
		             "the whole descriptor comes back", (long)info, 185,
		             &bad);
		sched_expect(memcmp(out, HID_DESC_ORIGINAL, 185) == 0,
		             "with the original's bytes", 1, 1, &bad);
		sched_expect(out[185] == 0xAA,
		             "and nothing past it is touched", out[185], 0xAA,
		             &bad);

		sched_expect(core_hid_ioctl(&cs, CORE_HID_IOC_REPORT_DESC, out,
		                            184, 0, 0, &info) ==
		             CORE_ST_BUFFER_TOO_SMALL,
		             "one byte short is refused", 1, 1, &bad);

		/* the joystick-only selection returns the suffix */
		cs.devices_mask = 3;
		memset(out, 0xAA, sizeof(out));
		core_hid_ioctl(&cs, CORE_HID_IOC_REPORT_DESC, out, sizeof(out),
		               0, 0, &info);
		sched_expect(info == 64 &&
		             memcmp(out, HID_DESC_ORIGINAL + 121, 64) == 0,
		             "mask 3 gives the joystick descriptor", (long)info,
		             64, &bad);
		groups++;
	}

	/* ---- 6. GET_STRING --------------------------------------------- */
	{
		memset(&cs, 0, sizeof(cs));
		memset(out, 0xAA, sizeof(out));

		st = core_hid_ioctl(&cs, CORE_HID_IOC_GET_STRING, out,
		                    sizeof(out), 0,
		                    CORE_HID_STRING_MANUFACTURER, &info);
		sched_expect(st == CORE_ST_SUCCESS && info == 0x30,
		             "the manufacturer is 0x30 bytes", (long)info, 0x30,
		             &bad);
		sched_expect(out[0] == 'W' && out[1] == 0 && out[2] == 'i',
		             "as UTF-16LE", out[0], 'W', &bad);
		sched_expect(out[0x2E] == 0 && out[0x2F] == 0,
		             "and terminated", out[0x2E], 0, &bad);

		memset(out, 0xAA, sizeof(out));
		core_hid_ioctl(&cs, CORE_HID_IOC_GET_STRING, out, sizeof(out),
		               0, CORE_HID_STRING_PRODUCT, &info);
		sched_expect(info == 0x12 && out[0] == 'A' && out[2] == 'd',
		             "the product is 0x12 bytes of Adaptoid",
		             (long)info, 0x12, &bad);

		core_hid_ioctl(&cs, CORE_HID_IOC_GET_STRING, out, sizeof(out),
		               0, CORE_HID_STRING_SERIAL, &info);
		sched_expect(info == 0,
		             "there is no serial number, reported as empty",
		             (long)info, 0, &bad);

		/* the language ID in the high half is ignored */
		core_hid_ioctl(&cs, CORE_HID_IOC_GET_STRING, out, sizeof(out),
		               0, 0x04090000u | CORE_HID_STRING_PRODUCT, &info);
		sched_expect(info == 0x12,
		             "and the language ID is ignored", (long)info, 0x12,
		             &bad);

		/*
		 * A SHORT BUFFER TRUNCATES WITHOUT A TERMINATOR, which is the
		 * original's behaviour - bounded, but the caller is told to
		 * believe a string that has no end.
		 */
		memset(out, 0xAA, sizeof(out));
		core_hid_ioctl(&cs, CORE_HID_IOC_GET_STRING, out, 10, 0,
		               CORE_HID_STRING_MANUFACTURER, &info);
		sched_expect(info == 10 && out[10] == 0xAA,
		             "a short buffer is truncated, not refused",
		             (long)info, 10, &bad);
		sched_expect(out[8] != 0 || out[9] != 0,
		             "AND IS LEFT UNTERMINATED", 1, 1, &bad);

		sched_expect(core_hid_ioctl(&cs, CORE_HID_IOC_GET_STRING, out,
		                            sizeof(out), 0, 0x11, &info) ==
		             CORE_ST_NOT_SUPPORTED,
		             "an unknown index is refused", 1, 1, &bad);
		groups++;
	}

	/* ---- 7. GET_DEVICE_ATTRIBUTES ---------------------------------- */
	{
		memset(&cs, 0, sizeof(cs));
		memset(out, 0xAA, sizeof(out));

		st = core_hid_ioctl(&cs, CORE_HID_IOC_ATTRIBUTES, out, 0x20, 0,
		                    0, &info);
		sched_expect(st == CORE_ST_SUCCESS && info == 0x20,
		             "thirty-two bytes of attributes", (long)info, 0x20,
		             &bad);
		sched_expect(out[0] == 0x20 && out[1] == 0 && out[2] == 0 &&
		             out[3] == 0, "Size is the first dword", out[0],
		             0x20, &bad);
		sched_expect(out[4] == 0xF7 && out[5] == 0x06,
		             "vendor 0x06F7", out[5], 0x06, &bad);
		sched_expect(out[6] == 0x01 && out[7] == 0x00,
		             "product 0x0001", out[6], 1, &bad);
		sched_expect(out[8] == 0x00 && out[9] == 0x01,
		             "version 0x0100", out[9], 1, &bad);
		{
			int zeroed = 1;

			for (i = 10; i < 0x20; i++) {
				if (out[i] != 0) {
					zeroed = 0;
				}
			}
			sched_expect(zeroed, "and the reserved words are zero",
			             zeroed, 1, &bad);
		}
		sched_expect(core_hid_ioctl(&cs, CORE_HID_IOC_ATTRIBUTES, out,
		                            0x1F, 0, 0, &info) ==
		             CORE_ST_BUFFER_TOO_SMALL,
		             "thirty-one bytes is refused", 1, 1, &bad);
		groups++;
	}

	/* ---- 8. write, activate, deactivate, and the unknown code ------- */
	{
		memset(&cs, 0, sizeof(cs));

		/*
		 * A WRITE IS ACCEPTED AND DISCARDED. The keyboard collection
		 * declares five LED bits as Output, so Windows really does
		 * send Num Lock down - and refusing it would make Windows
		 * think the keyboard was broken.
		 */
		st = core_hid_ioctl(&cs, CORE_HID_IOC_WRITE_REPORT, out,
		                    sizeof(out), 42, 0, &info);
		sched_expect(st == CORE_ST_SUCCESS && info == 42,
		             "a write reports the INPUT length back",
		             (long)info, 42, &bad);

		for (i = 0; i < CORE_HID_COLLECTIONS; i++) {
			core_hid_ioctl(&cs, CORE_HID_IOC_ACTIVATE, out,
			               sizeof(out), 0, (u32)i, &info);
		}
		sched_expect(cs.collection_enabled[0] == 1 &&
		             cs.collection_enabled[1] == 1 &&
		             cs.collection_enabled[2] == 1,
		             "activate sets all three collections",
		             cs.collection_enabled[2], 1, &bad);

		core_hid_ioctl(&cs, CORE_HID_IOC_DEACTIVATE, out, sizeof(out),
		               0, 1, &info);
		sched_expect(cs.collection_enabled[1] == 0 &&
		             cs.collection_enabled[0] == 1,
		             "and deactivate clears just the one named",
		             cs.collection_enabled[1], 0, &bad);

		/* out of range succeeds having done nothing */
		st = core_hid_ioctl(&cs, CORE_HID_IOC_DEACTIVATE, out,
		                    sizeof(out), 0, 99, &info);
		sched_expect(st == CORE_ST_SUCCESS &&
		             cs.collection_enabled[0] == 1,
		             "an out-of-range collection SUCCEEDS silently",
		             cs.collection_enabled[0], 1, &bad);

		sched_expect(core_hid_ioctl(&cs, CORE_HID_IOC_READ_REPORT, out,
		                            sizeof(out), 0, 0, &info) ==
		             CORE_ST_NOT_SUPPORTED,
		             "read report is the OS layer's, not this one's", 1,
		             1, &bad);
		sched_expect(core_hid_ioctl(&cs, 0x000B00FFu, out, sizeof(out),
		                            0, 0, &info) ==
		             CORE_ST_NOT_SUPPORTED,
		             "and an unknown code is refused", 1, 1, &bad);
		groups++;
	}

	/* ---- 9. the IRP half: what hidclass actually calls -------------- */
	{
		u8 buf[256];

		power_setup();
		core_init(&g_pw_ext.Core, harness_sink, &g_pw_ext);
		g_pw_ext.Core.devices_mask = 7;
		memset(buf, 0xAA, sizeof(buf));

		g_pw_sp.MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
		g_pw_irp.UserBuffer   = buf;
		g_pw_sp.Parameters.DeviceIoControl.OutputBufferLength =
		        sizeof(buf);
		g_pw_sp.Parameters.DeviceIoControl.InputBufferLength  = 0;
		g_pw_sp.Parameters.DeviceIoControl.Type3InputBuffer   = NULL;
		g_pw_sp.Parameters.DeviceIoControl.IoControlCode =
		        CORE_HID_IOC_REPORT_DESC;

		g_irp_count = 0;
		st = (u32)AdaptoidIntDeviceControl(&g_pw_dev, &g_pw_irp);
		sched_expect(st == CORE_ST_SUCCESS && g_irp_count == 1,
		             "the descriptor request is answered and completed",
		             g_irp_count, 1, &bad);
		sched_expect(g_pw_irp.IoStatus.Information == 185,
		             "with 185 bytes",
		             (long)g_pw_irp.IoStatus.Information, 185, &bad);
		sched_expect(memcmp(buf, HID_DESC_ORIGINAL, 185) == 0,
		             "into UserBuffer, not the system buffer", 1, 1,
		             &bad);

		/*
		 * THE ARGUMENT IS A VALUE IN A POINTER FIELD. If the decode
		 * dereferenced Type3InputBuffer instead of casting it, this
		 * would fault rather than answer.
		 */
		g_pw_sp.Parameters.DeviceIoControl.IoControlCode =
		        CORE_HID_IOC_GET_STRING;
		g_pw_sp.Parameters.DeviceIoControl.Type3InputBuffer =
		        (PVOID)(ULONG_PTR)CORE_HID_STRING_PRODUCT;
		g_irp_count = 0;
		AdaptoidIntDeviceControl(&g_pw_dev, &g_pw_irp);
		sched_expect(g_pw_irp.IoStatus.Information == 0x12 &&
		             buf[0] == 'A',
		             "the string index arrives through Type3InputBuffer",
		             (long)g_pw_irp.IoStatus.Information, 0x12, &bad);

		/* the input length is what a write reports back */
		g_pw_sp.Parameters.DeviceIoControl.IoControlCode =
		        CORE_HID_IOC_WRITE_REPORT;
		g_pw_sp.Parameters.DeviceIoControl.InputBufferLength = 7;
		AdaptoidIntDeviceControl(&g_pw_dev, &g_pw_irp);
		sched_expect(g_pw_irp.IoStatus.Information == 7,
		             "and the input length through its own field",
		             (long)g_pw_irp.IoStatus.Information, 7, &bad);

		/*
		 * READ_REPORT IS THE ONE THAT KEEPS THE IRP. It must park,
		 * not complete - and the remove lock must come back balanced
		 * even though the request is still outstanding.
		 */
		{
			LONG before = g_pw_ext.RemoveLockA.IoCount;

			g_pw_sp.Parameters.DeviceIoControl.IoControlCode =
			        CORE_HID_IOC_READ_REPORT;
			g_irp_count = 0;
			st = (u32)AdaptoidIntDeviceControl(&g_pw_dev,
			                                   &g_pw_irp);
			sched_expect(st == (u32)STATUS_PENDING &&
			             g_irp_count == 0,
			             "a read PARKS rather than completing",
			             g_irp_count, 0, &bad);
			sched_expect(g_pw_ext.RemoveLockA.IoCount == before,
			             "and balances the remove lock",
			             g_pw_ext.RemoveLockA.IoCount, before,
			             &bad);
			AdaptoidCancelPendingReads(&g_pw_ext);
		}

		/* an unstarted device refuses a read specifically */
		g_pw_ext.Started = 0;
		g_irp_count = 0;
		st = (u32)AdaptoidIntDeviceControl(&g_pw_dev, &g_pw_irp);
		sched_expect(st == (u32)STATUS_DELETE_PENDING &&
		             g_irp_count == 1,
		             "an unstarted device refuses everything", 1, 1,
		             &bad);

		/*
		 * AND SO DOES A DEVICE BEING REMOVED - the entry guard is
		 * AdaptoidIsDeviceReady, so a descriptor request is refused
		 * too, not just a read.
		 */
		g_pw_ext.Started       = 1;
		g_pw_ext.RemovePending = 1;
		g_pw_sp.Parameters.DeviceIoControl.IoControlCode =
		        CORE_HID_IOC_REPORT_DESC;
		g_irp_count = 0;
		st = (u32)AdaptoidIntDeviceControl(&g_pw_dev, &g_pw_irp);
		sched_expect(st == (u32)STATUS_DELETE_PENDING &&
		             g_irp_count == 1,
		             "and so does one that is being removed", 1, 1,
		             &bad);
		groups++;
	}
	hlog("HID minidriver contract: %s (%d groups)\n", bad ? "FAIL" : "ok",
	     groups);
	return bad;
}

/* ======================================================================
 * THE WIRING
 *
 * Every subsystem below this driver was tested on its own long before any
 * of them were joined up, and for a while the joins were simply missing:
 * AddDevice was never installed, the clock never ran, and a script's key
 * events went nowhere. Unit tests cannot catch that - each part passed.
 *
 * So these check the JOINS. A seam that is null, a clock that does not
 * advance, an event that reaches no state machine.
 * ====================================================================== */

/*
 * A device extension of this group's own. AddDevice is NOT idempotent - it
 * pushes a registry row and takes a control-device reference - so every
 * scenario gets a fresh start and gives it back.
 */
static ADAPTOID_DEVEXT g_wire_ext;

static PADAPTOID_DEVEXT wire_add(PDRIVER_OBJECT drv, PDEVICE_OBJECT fdo)
{
	AdaptoidAddDevice(drv, fdo);
	g_pnp_devext = &g_wire_ext;
	return &g_wire_ext;
}

static int test_wiring(void)
{
	int bad    = 0;
	int groups = 0;
	DRIVER_OBJECT        drv;
	UNICODE_STRING       regpath;
	DEVICE_OBJECT        fdo, pdo, lower;
	HID_DEVICE_EXTENSION hidext;
	PADAPTOID_DEVEXT     dx;
	ULONG                seen_before = g_reports_seen;

	/* ---- 1. DriverEntry installs AddDevice ------------------------- */
	{
		memset(&drv, 0, sizeof(drv));
		memset(&regpath, 0, sizeof(regpath));
		g_add_device = NULL;

		DriverEntry(&drv, &regpath);
		/*
		 * WITHOUT THIS THE DRIVER NEVER ENUMERATES. It loads, sits
		 * there, and no device is ever created - which looks exactly
		 * like a hardware problem from outside.
		 */
		sched_expect(g_add_device == AdaptoidAddDevice,
		             "DriverEntry installs AddDevice", 1, 1, &bad);
		sched_expect(drv.DriverUnload != NULL,
		             "and a DriverUnload, so the driver may unload at all",
		             1, 1, &bad);
		groups++;
	}

	/* ---- 2. AddDevice fills in every seam --------------------------- */
	{
		memset(&fdo, 0, sizeof(fdo));
		memset(&pdo, 0, sizeof(pdo));
		memset(&lower, 0, sizeof(lower));
		memset(&hidext, 0, sizeof(hidext));
		hidext.PhysicalDeviceObject = &pdo;
		hidext.NextDeviceObject     = &lower;
		hidext.MiniDeviceExtension  = &g_wire_ext;
		fdo.DeviceExtension         = &hidext;
		g_workitems             = 0;
		g_interfaces_registered = 0;

		sched_expect(NT_SUCCESS(AdaptoidAddDevice(&drv, &fdo)),
		             "AddDevice succeeds", 1, 1, &bad);
		dx = &g_wire_ext;
		g_pnp_devext = dx;

		sched_expect(dx->Core.vendor != 0 && dx->Core.vendor_claim != 0 &&
		             dx->Core.vendor_sync != 0,
		             "THE TRANSPORT IS INSTALLED, all three shapes", 1,
		             1, &bad);
		sched_expect(dx->Core.input_hook != 0 && dx->Core.tick_hook != 0,
		             "the packet and the clock reach the script engine",
		             1, 1, &bad);
		sched_expect(dx->Sched.cs == &dx->Core && dx->Sched.arm != 0 &&
		             dx->Sched.emit != 0,
		             "and the scheduler is joined to the core", 1, 1,
		             &bad);
		sched_expect(dx->Registration.cs == &dx->Core &&
		             dx->Registration.vendor != 0 &&
		             dx->Registration.cmd_xfer != 0,
		             "the registry row carries this device's seams", 1,
		             1, &bad);
		sched_expect(dx->ScriptDpc.Routine != 0 &&
		             dx->ScriptDepth == -1,
		             "the scheduler DPC is armed and latched at -1",
		             dx->ScriptDepth, -1, &bad);
		sched_expect(g_workitems == 1 && dx->PollWorkItem != NULL,
		             "a work item exists for the restart ladder",
		             g_workitems, 1, &bad);
		sched_expect(g_interfaces_registered == 1,
		             "and the device interface is published",
		             g_interfaces_registered, 1, &bad);

		/*
		 * POLLING STARTS STOPPED. A read submitted before PnP says
		 * start would go to a device the bus has not configured.
		 */
		sched_expect((dx->PollStopMask & ADAPTOID_STOP_REASON_PNP) != 0,
		             "polling is held until PnP starts the device", 1,
		             1, &bad);
		sched_expect(dx->Core.instance_id >= 50 &&
		             dx->Core.instance_id < 1150,
		             "the instance number is inside the stick range",
		             dx->Core.instance_id, 50, &bad);
		groups++;
	}

	/* ---- 3. the registry decides the HID personality ---------------- */
	{
		sched_expect(dx->Core.devices_mask == CORE_DEVICE_DEFAULT,
		             "absent means all three collections",
		             (long)dx->Core.devices_mask, CORE_DEVICE_DEFAULT,
		             &bad);

		AdaptoidUnwireDevice(dx);
		g_reg_present = 1;
		g_reg_value   = 1;
		wire_add(&drv, &fdo);
		sched_expect(dx->Core.devices_mask == 1 &&
		             core_hid_descriptor(dx->Core.devices_mask, 0) ==
		             core_hid_descriptor(7, 0) + CORE_HID_DESC_JOY_AT,
		             "and a value of 1 makes it a joystick alone",
		             (long)dx->Core.devices_mask, 1, &bad);
		g_reg_present = 0;
		AdaptoidUnwireDevice(dx);
		groups++;
	}

	/* ---- 4. the clock actually advances both engines ---------------- */
	{
		wire_add(&drv, &fdo);
		dx->Started      = 1;
		dx->PollStopMask = 0;
		core_init(&dx->Core, harness_sink, dx);
		AdaptoidWireDevice(dx);
		dx->Core.sink     = harness_sink;
		dx->Core.sink_ctx = dx;

		/*
		 * core_tick used to store the clock and do nothing else, so
		 * the effect ring and the scheduler never ran. Driving it must
		 * reach at least the effect engine, which is observable
		 * through next_tick.
		 */
		{
			s32 before = dx->Core.next_tick;

			clock_advance_ms(1000);
			dx->Core.effect_state = CORE_FX_STATE_TICK;
			core_tick(&dx->Core, KeQueryInterruptTime());
			sched_expect(dx->Core.now_100ns == KeQueryInterruptTime(),
			             "the clock reaches the core", 1, 1, &bad);
			sched_expect(dx->Core.next_tick != before ||
			             dx->Core.effect_state != CORE_FX_STATE_TICK,
			             "AND THE EFFECT ENGINE IS DRIVEN BY IT", 1,
			             1, &bad);
		}
		AdaptoidUnwireDevice(dx);
		groups++;
	}

	/* ---- 5. a script's key event becomes a real HID report ---------- */
	{
		wire_add(&drv, &fdo);
		core_init(&dx->Core, harness_sink, dx);
		AdaptoidWireDevice(dx);
		dx->Core.sink     = harness_sink;
		dx->Core.sink_ctx = dx;
		dx->Core.devices_mask = CORE_DEVICE_DEFAULT;

		g_reports_seen = 0;
		/*
		 * THE POINT OF THE WHOLE DRIVER. The event goes in at the
		 * scheduler's event sink and has to come out of the keyboard
		 * report state machine - not through a user-mode injection
		 * API that applications can ignore.
		 */
		dx->Sched.emit(dx->Sched.emit_ctx, CORE_EVENT_KEY, 0x04, 1);
		sched_expect(dx->Core.key_down_count == 1 &&
		             dx->Core.keys_down[0] == 0x04,
		             "a script _key lands in the keyboard report",
		             dx->Core.key_down_count, 1, &bad);
		sched_expect(g_reports_seen > 0,
		             "and a report actually went out", (long)g_reports_seen,
		             1, &bad);

		dx->Sched.emit(dx->Sched.emit_ctx, CORE_EVENT_KEY, 0x04, 0);
		sched_expect(dx->Core.key_down_count == 0,
		             "and the release closes the gap",
		             dx->Core.key_down_count, 0, &bad);

		dx->Sched.emit(dx->Sched.emit_ctx, CORE_EVENT_MOUSE_BUTTON, 1, 1);
		sched_expect(dx->Core.mouse_buttons != 0,
		             "a mouse button does the same",
		             dx->Core.mouse_buttons, 1, &bad);
		AdaptoidUnwireDevice(dx);
		groups++;
	}

	/* ---- 6. a script takes the stick, but only when it may ---------- */
	{
		static const u8 centre[CORE_RAW_PACKET_BYTES] =
		        { 0x20, 0x20, 0x80, 0x00, 0x00 };

		wire_add(&drv, &fdo);
		core_init(&dx->Core, harness_sink, dx);
		AdaptoidWireDevice(dx);
		dx->Core.sink     = harness_sink;
		dx->Core.sink_ctx = dx;
		dx->Core.accessory_state = CORE_ACC_FOUND_1;

		/*
		 * WITHOUT THE GATE the hardware wins the axes back on the very
		 * next packet and _stick appears to do nothing at all.
		 */
		dx->Core.script_owns_stick = 0;
		core_on_raw_packet(&dx->Core, centre);
		{
			s16 hardware_x = dx->Core.stick_x;

			dx->Core.script_owns_stick = 1;
			dx->Sched.cs->stick_x = 999;
			core_on_raw_packet(&dx->Core, centre);
			sched_expect(dx->Core.stick_x == hardware_x,
			             "a packet decodes over a stale script value",
			             dx->Core.stick_x, hardware_x, &bad);
		}
		AdaptoidUnwireDevice(dx);
		groups++;
	}

	/* ---- 7. teardown puts back everything AddDevice took ------------ */
	{
		PADAPTOID_CDO_EXT cx;

		wire_add(&drv, &fdo);
		cx = AdaptoidControlDeviceExt();
		sched_expect(cx != NULL && cx->Registry.count > 0,
		             "the adapter is on the driver-wide registry",
		             cx ? cx->Registry.count : -1, 1, &bad);

		g_workitems = 1;
		AdaptoidUnwireDevice(dx);
		sched_expect(g_workitems == 0 && dx->PollWorkItem == NULL,
		             "teardown returns the work item", g_workitems, 0,
		             &bad);
		sched_expect(AdaptoidControlDeviceExt() == NULL ||
		             AdaptoidControlDeviceExt()->Registry.count == 0,
		             "and takes the adapter off the registry", 1, 1,
		             &bad);
		groups++;
	}

	/* ---- 8. a stop resets the engine WITHOUT unplugging it ---------- */
	{
		wire_add(&drv, &fdo);
		core_init(&dx->Core, harness_sink, dx);
		AdaptoidWireDevice(dx);
		dx->Core.devices_mask = 6;
		dx->Core.instance_id  = 321;
		dx->Core.stick_clip   = 99;

		core_reset(&dx->Core);

		/*
		 * THE SEAMS MUST SURVIVE. A stop is not a remove - the device
		 * can be started again, and a core_reset that cleared the
		 * transport would leave it polling with nowhere to send.
		 */
		sched_expect(dx->Core.vendor != 0 && dx->Core.vendor_sync != 0 &&
		             dx->Core.input_hook != 0 && dx->Core.tick_hook != 0,
		             "a reset keeps every seam", 1, 1, &bad);
		sched_expect(dx->Core.devices_mask == 6 &&
		             dx->Core.instance_id == 321,
		             "and the identity, which is per arrival",
		             dx->Core.instance_id, 321, &bad);
		sched_expect(dx->Core.stick_clip == CORE_STICK_CLIP_DEFAULT,
		             "but engine state goes back to defaults",
		             dx->Core.stick_clip, CORE_STICK_CLIP_DEFAULT, &bad);
		AdaptoidUnwireDevice(dx);
		groups++;
	}

	/*
	 * The reports this group emitted are its own, not the ones main()
	 * counts through the single device it drives. Put the tally back.
	 */
	g_reports_seen = seen_before;

	hlog("Subsystem wiring       : %s (%d groups)\n", bad ? "FAIL" : "ok",
	     groups);
	return bad;
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

	/*
	 * UNBUFFERED. A test group that crashes takes the whole buffer with
	 * it otherwise, and "no output at all" is the least useful thing a
	 * failing harness can say.
	 */
	setvbuf(stdout, NULL, _IONBF, 0);

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
	bad += test_control_device();
	bad += test_wdm_transport();
	bad += test_triage_pnp();
	bad += test_input_path();
	bad += test_naming_recovery();
	bad += test_command_block();
	bad += test_power_and_control();
	bad += test_hid_contract();
	bad += test_wiring();

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
