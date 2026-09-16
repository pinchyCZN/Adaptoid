/*
 * core.c - OS-free logic for wishk300.
 *
 * See core.h for the rule this file obeys: no Windows or DDK headers, no
 * kernel calls, no Windows types. Everything arrives through the seams.
 *
 * SKELETON. The bodies here are stubs with correct signatures and correct
 * seams. The script interpreter, report builders and effect engine land on
 * top of this in later work; ../docs/script-bytecode.txt is the specification.
 */

#include "core.h"

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void core_zero(void *p, u32 len)
{
	u8 *b = (u8 *)p;
	while (len--) {
		*b++ = 0;
	}
}

void core_init(core_state *cs, core_report_fn sink, void *sink_ctx)
{
	if (cs == 0) {
		return;
	}
	core_zero(cs, (u32)sizeof(*cs));
	cs->sink         = sink;
	cs->sink_ctx     = sink_ctx;
	cs->devices_mask = CORE_DEVICE_DEFAULT;
}

void core_reset(core_state *cs)
{
	core_report_fn sink;
	void          *ctx;

	if (cs == 0) {
		return;
	}
	sink = cs->sink;
	ctx  = cs->sink_ctx;
	core_init(cs, sink, ctx);
}

/* ------------------------------------------------------------------ */
/* Report emission                                                     */
/* ------------------------------------------------------------------ */

/*
 * Gated on devices_mask the same way drv_SubmitHidReport gates on
 * drv_VirtualDevicesMask with (1 << (reportID - 1)). A report for a collection
 * the descriptor does not declare is dropped rather than sent.
 */
static void core_emit(core_state *cs, u8 report_id, const u8 *data, u32 len)
{
	u32 bit;

	if (cs == 0 || cs->sink == 0 || report_id == 0) {
		return;
	}
	bit = 1u << (report_id - 1);
	if ((cs->devices_mask & bit) == 0) {
		return;
	}
	cs->reports_emitted++;
	cs->sink(cs->sink_ctx, report_id, data, len);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

void core_on_raw_packet(core_state *cs, const u8 *raw)
{
	u32 i;
	u8  report[CORE_REPORT_MAX_BYTES];

	if (cs == 0 || raw == 0) {
		return;
	}

	for (i = 0; i < CORE_RAW_PACKET_BYTES; i++) {
		cs->raw[i] = raw[i];
	}
	cs->have_raw = 1;

	/*
	 * TODO: decode the raw packet and build the joystick report.
	 * Specification: ../docs/hid-descriptor.txt section 4 for the layout and
	 * ../docs/usb-transport.txt for the raw byte meanings. The stick scaling,
	 * including the vendor's octagonal-to-square corner stretch, is described
	 * in ../docs/known-defects.txt section 3 and should be a tunable.
	 *
	 * For now, pass the raw bytes straight through so the seam is exercised
	 * end to end and the harness has something to print.
	 */
	core_zero(report, (u32)sizeof(report));
	for (i = 0; i < CORE_RAW_PACKET_BYTES; i++) {
		report[i] = raw[i];
	}
	core_emit(cs, CORE_REPORT_JOYSTICK, report, CORE_RAW_PACKET_BYTES);
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

void core_tick(core_state *cs, u64 now_100ns)
{
	if (cs == 0) {
		return;
	}
	cs->now_100ns = now_100ns;

	/*
	 * TODO: run the script scheduler and the effect engine from here.
	 * The original's time base is 1/64 second with a 100000-instruction budget
	 * per quantum; see ../docs/script-bytecode.txt section 6.1. Driving both
	 * from a caller-supplied clock is what makes them deterministic under the
	 * harness.
	 */
}

/* ------------------------------------------------------------------ */
/* Controller Pak CRCs                                                 */
/* ------------------------------------------------------------------ */

u8 core_pak_addr_crc5(u16 address)
{
	/* TODO: 5-bit CRC over the 11-bit address field of a Controller Pak
	 * read/write. Specification: ../docs/usb-transport.txt. */
	(void)address;
	return 0;
}

u8 core_pak_data_crc8(const u8 *data, u32 len)
{
	/* TODO: 8-bit CRC over a 32-byte Controller Pak data block.
	 * Specification: ../docs/usb-transport.txt. */
	(void)data;
	(void)len;
	return 0;
}
