/*
 * ioctl.c - the private IOCTL surface.
 *
 * drv_IoctlDeviceCommand (00012bc0) is the original: one switch over
 * eighteen control codes, each a length check and a small action. The
 * dispatch is a jump table at 000135f0 indexed through a byte table at
 * 00013640, and reading those two settled the surface exactly - including
 * that function 0x831 has an index-table entry pointing at the DEFAULT
 * target, so it is not handled despite looking like it might be.
 *
 * TWO OF THE EIGHTEEN CASES HAVE BOUNDS FAILURES. Both are fixed here rather
 * than reproduced, and both are recorded in ../docs/known-defects.txt:
 * function 0x83a receives into a buffer it does not size (section 13), and
 * functions 0x853 and 0x855 take a signed slot index and check only its
 * upper bound (section 14).
 */

#include "ioctl.h"

/* ---- small helpers ----------------------------------------------------- */

static u32 rd32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

static void wr32(u8 *p, u32 v)
{
	p[0] = (u8)(v & 0xFFu);
	p[1] = (u8)((v >> 8) & 0xFFu);
	p[2] = (u8)((v >> 16) & 0xFFu);
	p[3] = (u8)((v >> 24) & 0xFFu);
}

/*
 * Is a slot still running?
 *
 * DIVERGENCE: the slot index is bounds-checked. drv_EffectSlotActive
 * (00013e90) indexes EffectSlot[SlotIndex] with no check at all, and is
 * reached from function 0x855 with an index its caller only bounds ABOVE -
 * so a negative index reads, and on the expiry path WRITES, outside the
 * array. See ../docs/known-defects.txt section 14.
 */
int core_effect_slot_active(core_state *cs, s32 slot, s32 now_tick)
{
	core_effect_slot *fx;
	s32 elapsed;

	if (cs == 0 || slot < 0 || slot >= CORE_EFFECT_SLOTS) {
		return 0;
	}
	fx = &cs->effect[slot];
	if (!fx->running) {
		return 0;
	}
	if (now_tick < fx->start_tick) {
		return 0;               /* scheduled, not started */
	}
	if (fx->duration == CORE_FX_INFINITE) {
		return 1;
	}
	elapsed = now_tick - fx->start_tick;
	/*
	 * The stop is 32 ticks LATE, half a second past the declared duration.
	 * That is the original's grace window, not a slip: the ring runs 32
	 * ticks ahead, so retiring a slot the instant it expires would strand
	 * ticks that were already computed and sent.
	 */
	if (elapsed > fx->duration + 32) {
		fx->running = 0;
	}
	return elapsed < fx->duration;
}

/* ---- the cases --------------------------------------------------------- */

static u32 ioc_status_snap(core_ioctl_env *env, const core_ioctl *r, u32 *info)
{
	core_state *cs = env->cs;
	u32 i;

	if (r->in_len != 0 || r->out_len != 6) {
		return CORE_ST_INVALID_PARAM;
	}
	r->out[0] = (u8)cs->report_pending;
	for (i = 0; i < CORE_RAW_PACKET_BYTES; i++) {
		r->out[1 + i] = cs->raw[i];
	}
	cs->report_pending = 0;     /* reading it clears it */
	*info = 6;
	return CORE_ST_SUCCESS;
}

static u32 ioc_device_name(core_ioctl_env *env, const core_ioctl *r, u32 *info)
{
	core_state *cs = env->cs;
	u32 i;

	if (r->in_len != 0 || r->out_len == 0) {
		return CORE_ST_INVALID_PARAM;
	}
	/* Copies up to the NUL, or fills the buffer. The count returned
	 * INCLUDES the NUL when one was reached, which is what the original
	 * does and what the configurator expects. */
	for (i = 0; i < r->out_len; i++) {
		r->out[i] = (u8)cs->device_name[i];
		if (cs->device_name[i] == 0) {
			i++;
			break;
		}
	}
	*info = i;
	return CORE_ST_SUCCESS;
}

static u32 ioc_read_counter(core_ioctl_env *env, const core_ioctl *r, u32 *info)
{
	core_state *cs = env->cs;
	u32 sel;

	if (r->in_len != 4 || r->out_len != 4) {
		return CORE_ST_INVALID_PARAM;
	}
	sel = rd32(r->in);
	if (sel == 0 || sel > 4) {
		return CORE_ST_INVALID_PARAM;
	}
	wr32(r->out, 0);
	switch (sel) {
	case CORE_COUNTER_FIRMWARE:
		/* bcdDevice from the USB device descriptor, which the
		 * configurator reports as the firmware revision. Sixteen bits
		 * into a zeroed dword. */
		r->out[0] = (u8)(cs->bcd_device & 0xFFu);
		r->out[1] = (u8)((cs->bcd_device >> 8) & 0xFFu);
		break;
	case CORE_COUNTER_TWO:
		wr32(r->out, cs->counter_two);
		break;
	case CORE_COUNTER_REPORTS:
		wr32(r->out, cs->reports_emitted);
		break;
	default:
		wr32(r->out, (u32)cs->accessory_state);
		break;
	}
	*info = 4;
	return CORE_ST_SUCCESS;
}

static u32 ioc_zero_counter(core_ioctl_env *env, const core_ioctl *r, u32 *info)
{
	core_state *cs = env->cs;
	u32 sel;

	if (r->in_len != 4 || r->out_len != 0) {
		return CORE_ST_INVALID_PARAM;
	}
	sel = rd32(r->in);
	if (sel == 0 || sel > 4) {
		return CORE_ST_INVALID_PARAM;
	}
	/* Only two of the four are writable; 1 and 4 are accepted and do
	 * nothing, which is deliberate - they are not counters. */
	if (sel == CORE_COUNTER_TWO) {
		cs->counter_two = 0;
	} else if (sel == CORE_COUNTER_REPORTS) {
		cs->reports_emitted = 0;
	}
	*info = 0;
	return CORE_ST_SUCCESS;
}

/* Get and/or set one dword. Both lengths may be 0 or 4 independently, so a
 * single call can read, write, or do both. */
static u32 ioc_get_set(const core_ioctl *r, s32 *field, u32 *info)
{
	s32 before;

	if (r->in_len != 0 && r->in_len != 4) {
		return CORE_ST_INVALID_PARAM;
	}
	if (r->out_len != 0 && r->out_len != 4) {
		return CORE_ST_INVALID_PARAM;
	}
	before = *field;            /* the OLD value is what is returned */
	if (r->in_len == 4) {
		*field = (s32)rd32(r->in);
	}
	if (r->out_len == 4) {
		wr32(r->out, (u32)before);
		*info = 4;
	} else {
		*info = 0;
	}
	return CORE_ST_SUCCESS;
}

static u32 ioc_n64_passthru(core_ioctl_env *env, const core_ioctl *r, u32 *info)
{
	s32 actual = 0;

	if (r->in_len == 0 || r->out_len == 0) {
		return CORE_ST_INVALID_PARAM;
	}
	/*
	 * DIVERGENCE, and it is the fix for defect 11. The original passes
	 * both lengths straight through after testing only that neither is
	 * zero, and drv_N64Transaction receives into a 64-byte stack buffer it
	 * never sizes the request against. The bound belongs in BOTH places -
	 * core_n64_transaction refuses an oversized length too - because
	 * relying on one of them is how the original came to have none.
	 */
	if (r->out_len > CORE_N64_RX_MAX) {
		return CORE_ST_INVALID_PARAM;
	}
	core_n64_transaction(env->cs, r->in, (s32)r->in_len,
	                     r->out, (s32)r->out_len, &actual);
	*info = (u32)actual;
	return CORE_ST_SUCCESS;
}

static u32 ioc_pak_status(core_ioctl_env *env, const core_ioctl *r, u32 *info)
{
	u8  tx = 0;                 /* N64 command 0x00, request info */
	u8  rx[4];
	s32 actual = 0;

	if (r->in_len != 0 || r->out_len != 1) {
		return CORE_ST_INVALID_PARAM;
	}
	core_n64_transaction(env->cs, &tx, 1, rx, 3, &actual);
	if (actual != 3) {
		*info = 0;
		return CORE_ST_DATA_ERROR;
	}
	/* Reversed by the transaction, so the three bytes are
	 * 0x05, 0x00, status and the status is the last. */
	r->out[0] = rx[2];
	*info = 1;
	return CORE_ST_SUCCESS;
}

/* Both Pak paths report a CRC of 0x80 as "not ready" rather than as a
 * mismatch: that value is the controller saying the accessory is absent. */
static u32 pak_crc_status(u8 crc)
{
	return (crc == 0x80u) ? CORE_ST_DEVICE_NOT_READY : CORE_ST_CRC_ERROR;
}

static u32 ioc_pak_read(core_ioctl_env *env, const core_ioctl *r, u32 *info)
{
	u8  tx[3];
	u8  rx[CORE_PAK_BLOCK_BYTES + 4];
	u16 addr;
	s32 actual = 0;
	u8  crc;
	u32 i;

	if (r->in_len != 4 || r->out_len < CORE_PAK_BLOCK_BYTES) {
		return CORE_ST_INVALID_PARAM;
	}
	addr  = core_pak_addr_encode((u16)rd32(r->in));
	tx[0] = 0x02;               /* read accessory */
	tx[1] = (u8)((addr >> 8) & 0xFFu);
	tx[2] = (u8)(addr & 0xFFu);

	core_n64_transaction(env->cs, tx, 3, rx, CORE_PAK_BLOCK_BYTES + 1,
	                     &actual);
	if (actual != CORE_PAK_BLOCK_BYTES + 1) {
		*info = 0;
		return CORE_ST_DATA_ERROR;
	}
	crc = (u8)(core_pak_data_crc8(rx, CORE_PAK_BLOCK_BYTES) ^
	           rx[CORE_PAK_BLOCK_BYTES]);
	if (crc != 0) {
		*info = 0;
		return pak_crc_status(crc);
	}
	for (i = 0; i < CORE_PAK_BLOCK_BYTES; i++) {
		r->out[i] = rx[i];
	}
	*info = CORE_PAK_BLOCK_BYTES;
	return CORE_ST_SUCCESS;
}

static u32 ioc_pak_write(core_ioctl_env *env, const core_ioctl *r, u32 *info)
{
	u8  tx[3 + CORE_PAK_BLOCK_BYTES];
	u8  rx[4];
	u16 addr;
	s32 actual = 0;
	u8  crc;
	u32 i;

	if (r->in_len != 4 + CORE_PAK_BLOCK_BYTES || r->out_len != 0) {
		return CORE_ST_INVALID_PARAM;
	}
	addr  = core_pak_addr_encode((u16)rd32(r->in));
	tx[0] = 0x03;               /* write accessory */
	tx[1] = (u8)((addr >> 8) & 0xFFu);
	tx[2] = (u8)(addr & 0xFFu);
	for (i = 0; i < CORE_PAK_BLOCK_BYTES; i++) {
		tx[3 + i] = r->in[4 + i];
	}
	crc = core_pak_data_crc8(tx + 3, CORE_PAK_BLOCK_BYTES);

	core_n64_transaction(env->cs, tx, 3 + CORE_PAK_BLOCK_BYTES, rx, 1,
	                     &actual);
	if (actual != 1) {
		*info = 0;
		return CORE_ST_DATA_ERROR;
	}
	/* The controller echoes the CRC it computed; they must agree. */
	crc = (u8)(crc ^ rx[0]);
	*info = 0;
	if (crc != 0) {
		return pak_crc_status(crc);
	}
	return CORE_ST_SUCCESS;
}

static u32 ioc_script_load(core_ioctl_env *env, const core_ioctl *r, u32 *info)
{
	s32 code_count, var_count;
	u32 i;
	u32 need;

	if (r->in_len < 8) {
		return CORE_ST_INVALID_PARAM;
	}
	code_count = (s32)rd32(r->in);
	var_count  = (s32)rd32(r->in + 4);

	/*
	 * THE LENGTH IS AN EXACT EQUALITY, not a minimum, so the bytecode copy
	 * cannot over-read the input buffer. Computed in u32 and compared
	 * against the declared count so that a count large enough to overflow
	 * the multiply cannot match a small buffer.
	 */
	if (code_count < 0) {
		return CORE_ST_INVALID_PARAM;
	}
	need = (u32)code_count * 4u + 8u;
	if (need < 8u || need != r->in_len) {
		return CORE_ST_INVALID_PARAM;
	}

	if (env->sched == 0) {
		return CORE_ST_INVALID_PARAM;
	}
	/*
	 * The original guards on a script-load state word and answers
	 * STATUS_DEVICE_BUSY if a load is already under way. That word is PnP
	 * and IOCTL state rather than scheduler state, so it lives with the
	 * device; here the scheduler's own re-entrancy is what matters and a
	 * load while a pass is running cannot happen, because a pass never
	 * returns to user mode.
	 */
	if (env->sched->running) {
		return CORE_ST_DEVICE_BUSY;
	}

	if (code_count == 0 || var_count <= 0) {
		core_sched_unload(env->sched);
		*info = 0;
		return CORE_ST_SUCCESS;
	}
	{
		/* The bytecode is little-endian dwords in the buffer. */
		static u32 words[4096];
		s32 n = code_count;

		if (n > (s32)(sizeof(words) / sizeof(words[0]))) {
			return CORE_ST_INVALID_PARAM;
		}
		for (i = 0; i < (u32)n; i++) {
			words[i] = rd32(r->in + 8 + i * 4);
		}
		core_sched_load(env->sched, words, n, var_count, env->now_100ns);
	}
	*info = 0;
	return CORE_ST_SUCCESS;
}

/*
 * Drain the post-mortem dump left by a faulted script thread: the thread
 * node first, then as much of the globals snapshot as still fits. Both are
 * released either way, and the slot is left empty.
 */
static u32 ioc_script_fault(core_ioctl_env *env, const core_ioctl *r, u32 *info)
{
	core_sched *s = env->sched;
	core_sched_thread *t;
	u32 *vars;
	u32 room, took, i;

	if (r->in_len != 0) {
		return CORE_ST_INVALID_PARAM;
	}
	if (s == 0) {
		*info = 0;
		return CORE_ST_SUCCESS;
	}
	t    = s->fault_thread;
	vars = s->fault_vars;
	s->fault_thread = 0;
	s->fault_vars   = 0;
	if (t == 0) {
		/*
		 * DIVERGENCE: the snapshot is released even when there is no
		 * thread to go with it. The original returns here without
		 * freeing it.
		 */
		if (vars != 0 && s->release != 0) {
			s->release(s->mem_ctx, vars);
		}
		*info = 0;
		return CORE_ST_SUCCESS;
	}

	/* The thread node, header and saved stack, up to what fits. */
	took = (u32)t->stack_size * 4u + (u32)sizeof(*t);
	if (took > r->out_len) {
		took = r->out_len;
	}
	for (i = 0; i < took; i++) {
		r->out[i] = ((const u8 *)t)[i];
	}
	if (s->release != 0) {
		s->release(s->mem_ctx, t);
	}

	/* Then the globals, into whatever is left. */
	room = r->out_len - took;
	if (room > (u32)s->vm.var_count * 4u) {
		room = (u32)s->vm.var_count * 4u;
	}
	if (vars != 0) {
		for (i = 0; i < room; i++) {
			r->out[took + i] = ((const u8 *)vars)[i];
		}
		/*
		 * DIVERGENCE: released unconditionally. The original frees the
		 * snapshot only inside the "there was room for some of it"
		 * branch, so an output buffer exactly the size of the thread
		 * node leaks it - and it has already been taken out of the
		 * device extension, so nothing else can ever free it. See
		 * ../docs/known-defects.txt section 15.
		 */
		if (s->release != 0) {
			s->release(s->mem_ctx, vars);
		}
	} else {
		room = 0;
	}
	*info = took + room;
	return CORE_ST_SUCCESS;
}

static u32 ioc_effect_prog(core_ioctl_env *env, const core_ioctl *r, u32 *info)
{
	core_state *cs = env->cs;
	core_effect_slot *fx;
	u32 head, flags, slot;
	s32 block, need, tick;
	s32 stream, i;
	const u8 *payload;

	if (r->in_len < CORE_EFFECT_PROG_HEADER || r->out_len != 0) {
		return CORE_ST_INVALID_PARAM;
	}
	head  = rd32(r->in);
	slot  = head & 0xFFFFu;
	flags = (head >> 16) & 0xFFu;
	block = (s32)rd32(r->in + 0x18);

	/* Required length is the header plus one payload per set stream bit. */
	need = CORE_EFFECT_PROG_HEADER;
	for (stream = 0; stream < CORE_EFFECT_AXES; stream++) {
		if (flags & (1u << stream)) {
			need += block;
		}
	}
	if ((s32)r->in_len < need || slot >= CORE_EFFECT_SLOTS) {
		return CORE_ST_INVALID_PARAM;
	}

	/*
	 * The 28-byte header, in the order _ADAPTOID_EFFECT_SLOT declares its
	 * fields. Taken from the applied structure rather than from the
	 * assignment order in the decompilation, which is scheduled and reads
	 * as though AxisBlockLength came third.
	 */
	fx = &cs->effect[slot];
	fx->type         = head >> 24;
	fx->duration     = (s32)rd32(r->in + 0x04);
	fx->attack_level = (s32)rd32(r->in + 0x08);
	fx->attack_time  = (s32)rd32(r->in + 0x0C);
	fx->fade_level   = (s32)rd32(r->in + 0x10);
	fx->fade_time    = (s32)rd32(r->in + 0x14);
	fx->block_length = block;

	payload = r->in + CORE_EFFECT_PROG_HEADER;
	for (stream = 0; stream < CORE_EFFECT_AXES; stream++) {
		if ((flags & (1u << stream)) == 0) {
			continue;
		}
		/* At most sizeof(core_effect_axis) bytes land per stream; the
		 * original caps the copy at eight and the axis block is eight
		 * bytes, which is the same statement. */
		for (i = 0; i < block && i < (s32)sizeof(core_effect_axis); i++) {
			((u8 *)&cs->effect[slot].axis[stream])[i] = payload[i];
		}
		payload += block;
	}

	tick = (s32)(env->now_100ns / CORE_TICK_100NS);

	/*
	 * When to restart the clock. A stopped slot restarts only if the
	 * timestamp bits say so; a running one restarts if bit 0x40 is set or
	 * if it has already expired.
	 */
	if (!fx->running) {
		if (flags & 0xC0u) {
			fx->start_tick = tick;
		}
	} else if ((flags & 0x40u) ||
	           !core_effect_slot_active(cs, (s32)slot, tick)) {
		fx->start_tick = tick;
	}

	/* A zero-length block programs the slot without starting it. */
	if (block > 0) {
		fx->running = 1;
		core_effect_kick(cs, env->now_100ns);
	}
	*info = 0;
	return CORE_ST_SUCCESS;
}

static u32 ioc_effect_ctrl(core_ioctl_env *env, const core_ioctl *r, u32 *info)
{
	core_state *cs = env->cs;
	s32 cmd, slot, i;

	if (r->in_len != 2 || r->out_len != 0) {
		return CORE_ST_INVALID_PARAM;
	}
	cmd  = (s32)r->in[0];
	slot = (s32)(s8)r->in[1];

	/*
	 * DIVERGENCE, and it is the fix for defect 14. The original reads the
	 * slot as a SIGNED char and tests only `slot < 32` with a signed
	 * compare, then sign-extends it into the index. Any byte from 0x80 to
	 * 0xFF therefore passes as a negative slot and the writes below land
	 * at devext + slot * 0x38, up to 0x1C00 bytes BEFORE the extension.
	 * Command 2 does it with no further gate at all.
	 */
	if (slot < 0 || slot >= CORE_EFFECT_SLOTS) {
		return CORE_ST_INVALID_PARAM;
	}

	*info = 0;
	switch (cmd) {
	case CORE_EFFECT_CMD_START:
		/* A slot with no axis block is not startable. Note the original
		 * skips its kick on this path, so a failed start is completely
		 * silent - reproduced. */
		if (cs->effect[slot].block_length > 0) {
			cs->effect[slot].running    = 1;
			cs->effect[slot].start_tick =
			        (s32)(env->now_100ns / CORE_TICK_100NS);
			core_effect_kick(cs, env->now_100ns);
		}
		return CORE_ST_SUCCESS;

	case CORE_EFFECT_CMD_STOP:
		cs->effect[slot].running = 0;
		break;

	case CORE_EFFECT_CMD_STOP_ALL:
		for (i = 0; i < CORE_EFFECT_SLOTS; i++) {
			cs->effect[i].running = 0;
		}
		break;

	default:
		/* An unrecognised command still kicks the engine, which is what
		 * the original does by falling through. */
		break;
	}
	core_effect_kick(cs, env->now_100ns);
	return CORE_ST_SUCCESS;
}

static u32 ioc_effect_query(core_ioctl_env *env, const core_ioctl *r, u32 *info)
{
	s32 slot;

	if (r->in_len != 1 || r->out_len != 1) {
		return CORE_ST_INVALID_PARAM;
	}
	slot = (s32)(s8)r->in[0];
	/* Same signed-index fix as CORE_IOC_EFFECT_CTRL; the original reaches
	 * drv_EffectSlotActive, which bounds-checks nothing. */
	if (slot < 0 || slot >= CORE_EFFECT_SLOTS) {
		return CORE_ST_INVALID_PARAM;
	}
	r->out[0] = (u8)(core_effect_slot_active(
	                     env->cs, slot,
	                     (s32)(env->now_100ns / CORE_TICK_100NS)) != 0);
	*info = 1;
	return CORE_ST_SUCCESS;
}

/*
 * The raw vendor passthrough. The first six input bytes are a USB setup
 * packet; where the data stage lives depends on its direction bit.
 *
 *     in 6, out >= 1, bit 0x80 set      IN, data into the output buffer
 *     in >= 7, out 0, bit 0x80 clear    OUT, data follows the setup packet
 *     in 6, out 0, bit 0x80 clear       no data stage
 *
 * Anything else is rejected. This is the only case that can answer
 * CORE_ST_PENDING.
 */
static u32 ioc_raw_vendor(core_ioctl_env *env, const core_ioctl *r, u32 *info)
{
	u8 *data;
	u32 data_len;
	int in_dir;

	if (r->in_len < 6) {
		return CORE_ST_INVALID_PARAM;
	}
	in_dir = (r->in[0] & 0x80u) != 0;

	if (r->in_len == 6 && r->out_len >= 1 && in_dir) {
		data     = r->out;
		data_len = r->out_len;
	} else if (r->in_len >= 7 && r->out_len == 0 && !in_dir) {
		data     = (u8 *)(r->in + 6);
		data_len = r->in_len - 6;
	} else if (r->in_len == 6 && r->out_len == 0 && !in_dir) {
		data     = 0;
		data_len = 0;
	} else {
		return CORE_ST_INVALID_PARAM;
	}

	if (env->vendor == 0) {
		return CORE_ST_INVALID_PARAM;
	}
	*info = 0;
	return env->vendor(env->vendor_ctx, r->in, data, data_len);
}

/* ---- the dispatcher ---------------------------------------------------- */

u32 core_ioctl_dispatch(core_ioctl_env *env, const core_ioctl *req, u32 *info)
{
	u32 scratch = 0;

	if (info == 0) {
		info = &scratch;
	}
	*info = 0;
	if (env == 0 || env->cs == 0 || req == 0) {
		return CORE_ST_INVALID_PARAM;
	}
	if ((req->code & 0xFFFF0000u) != CORE_IOCTL_DEVICE_TYPE) {
		return CORE_ST_NOT_SUPPORTED;
	}

	switch (CORE_IOCTL_FN(req->code)) {
	case CORE_IOC_RAW_VENDOR:
		return ioc_raw_vendor(env, req, info);

	case CORE_IOC_STATUS_SNAP:
		return ioc_status_snap(env, req, info);

	case CORE_IOC_ACCEPT_NOP:
		/* Accepted with no observable effect, but only for an input
		 * under 0x201 bytes. Preserved because a caller uses the
		 * distinction to probe. */
		if (req->out_len != 0 || req->in_len > 0x200u) {
			return CORE_ST_INVALID_PARAM;
		}
		return CORE_ST_SUCCESS;

	case CORE_IOC_DEVICE_NAME:
		return ioc_device_name(env, req, info);

	case CORE_IOC_READ_COUNTER:
		return ioc_read_counter(env, req, info);

	case CORE_IOC_ZERO_COUNTER:
		return ioc_zero_counter(env, req, info);

	case CORE_IOC_SET_ENABLE:
		if (req->in_len != 4 || req->out_len != 0) {
			return CORE_ST_INVALID_PARAM;
		}
		if (env->enable != 0) {
			env->enable(env->enable_ctx, rd32(req->in) != 0);
		}
		return CORE_ST_SUCCESS;

	case CORE_IOC_N64_PASSTHRU:
		return ioc_n64_passthru(env, req, info);

	case CORE_IOC_SCRIPT_LOAD:
		return ioc_script_load(env, req, info);

	case CORE_IOC_SCRIPT_FAULT:
		return ioc_script_fault(env, req, info);

	case CORE_IOC_STICK_CLIP:
		return ioc_get_set(req, &env->cs->stick_clip, info);

	case CORE_IOC_STICK_STRETCH:
		return ioc_get_set(req, &env->cs->stick_stretch, info);

	case CORE_IOC_PAK_STATUS:
		return ioc_pak_status(env, req, info);

	case CORE_IOC_PAK_READ:
		return ioc_pak_read(env, req, info);

	case CORE_IOC_PAK_WRITE:
		return ioc_pak_write(env, req, info);

	case CORE_IOC_EFFECT_PROG:
		return ioc_effect_prog(env, req, info);

	case CORE_IOC_EFFECT_CTRL:
		return ioc_effect_ctrl(env, req, info);

	case CORE_IOC_EFFECT_QUERY:
		return ioc_effect_query(env, req, info);

	default:
		/*
		 * Everything else, INCLUDING function 0x831. Its index-table
		 * entry points at the same target as the default, so despite
		 * having an entry it is not handled.
		 */
		return CORE_ST_NOT_SUPPORTED;
	}
}

/* ------------------------------------------------------------------ */
/* the notification queue                                              */
/* ------------------------------------------------------------------ */

static void nw_init(core_notify_waiter *head)
{
	head->flink = head;
	head->blink = head;
}

static int nw_empty(const core_notify_waiter *head)
{
	return head->flink == head;
}

static void nw_append(core_notify_waiter *head, core_notify_waiter *w)
{
	w->flink          = head;
	w->blink          = head->blink;
	head->blink->flink = w;
	head->blink        = w;
}

static void nw_unlink(core_notify_waiter *w)
{
	w->blink->flink = w->flink;
	w->flink->blink = w->blink;
	w->flink = 0;
	w->blink = 0;
}

void core_notify_init(core_notify *n, core_notify_claim_fn claim,
                      core_notify_deliver_fn deliver,
                      core_notify_abort_fn abort, void *ctx)
{
	s32 i;

	if (n == 0) {
		return;
	}
	for (i = 0; i < CORE_NOTIFY_MAX; i++) {
		n->events[i].type = 0;
		n->events[i].arg1 = 0;
		n->events[i].arg2 = 0;
	}
	n->head    = 0;
	n->count   = 0;
	n->dropped = 0;
	nw_init(&n->waiters);
	n->waiter_count = 0;
	n->delivering   = 0;
	n->claim   = claim;
	n->deliver = deliver;
	n->abort   = abort;
	n->ctx     = ctx;
}

/*
 * Hand events to waiters, one for one, until either runs out.
 *
 * A waiter the owner will not claim has been cancelled underneath us; it is
 * dropped WITHOUT consuming its event, because the canceller completes it and
 * the event is still owed to somebody.
 */
static void notify_pump(core_notify *n)
{
	while (n->count > 0 && !nw_empty(&n->waiters)) {
		core_notify_waiter *w = n->waiters.flink;
		core_sched_event e;

		nw_unlink(w);
		n->waiter_count--;

		if (n->claim != 0 && !n->claim(n->ctx, w)) {
			continue;       /* cancellation won; the event stays */
		}

		e = n->events[n->head];
		n->head = (n->head + 1) % CORE_NOTIFY_MAX;
		n->count--;
		if (n->deliver != 0) {
			n->deliver(n->ctx, w, e.type, e.arg1, e.arg2);
		}
	}
	n->delivering = 0;
}

void core_notify_post(core_notify *n, u32 type, u32 arg1, u32 arg2)
{
	s32 slot;

	if (n == 0) {
		return;
	}
	if (n->count >= CORE_NOTIFY_MAX) {
		/* Full: drop the OLDEST. A listener that stopped reading loses
		 * history rather than stalling the driver. */
		n->head = (n->head + 1) % CORE_NOTIFY_MAX;
		n->count--;
		n->dropped++;
	}
	slot = (n->head + n->count) % CORE_NOTIFY_MAX;
	n->events[slot].type = type;
	n->events[slot].arg1 = arg1;
	n->events[slot].arg2 = arg2;
	n->count++;

	if (!nw_empty(&n->waiters) && !n->delivering) {
		n->delivering = 1;
		notify_pump(n);
	}
}

int core_notify_wait(core_notify *n, core_notify_waiter *w)
{
	s32 before;

	if (n == 0 || w == 0) {
		return 0;
	}
	nw_append(&n->waiters, w);
	n->waiter_count++;

	if (n->count > 0 && !n->delivering) {
		before = n->count;
		n->delivering = 1;
		notify_pump(n);
		return n->count < before;
	}
	return 0;
}

int core_notify_cancel(core_notify *n, core_notify_waiter *w)
{
	if (n == 0 || w == 0 || w->flink == 0) {
		return 0;           /* delivery already took it off the list */
	}
	nw_unlink(w);
	n->waiter_count--;
	return 1;
}

void core_notify_flush(core_notify *n)
{
	if (n == 0) {
		return;
	}
	while (!nw_empty(&n->waiters)) {
		core_notify_waiter *w = n->waiters.flink;

		nw_unlink(w);
		n->waiter_count--;
		if (n->claim != 0 && !n->claim(n->ctx, w)) {
			continue;
		}
		if (n->abort != 0) {
			n->abort(n->ctx, w);
		}
	}
}

/* ------------------------------------------------------------------ */
/* the device registry                                                 */
/* ------------------------------------------------------------------ */

/* The button map the original ships, mirrored here so a fresh registry
 * hands out the same mapping core_init does. */
static const u8 CTL_BUTTON_MAP_DEFAULT[CORE_RAW_BUTTON_BITS] = {
	0, 3, 9, 8, 10, 11, 12, 13,
	CORE_BUTTON_NONE, CORE_BUTTON_NONE, 6, 7, 5, 1, 4, 2
};

void core_registry_init(core_registry *reg)
{
	s32 i;

	if (reg == 0) {
		return;
	}
	reg->devices.flink = &reg->devices;
	reg->devices.blink = &reg->devices;
	reg->count           = 0;
	reg->live_count      = 0;
	reg->generation      = 0;
	reg->reports_enabled = 0;
	for (i = 0; i < CORE_RAW_BUTTON_BITS; i++) {
		reg->button_map[i] = CTL_BUTTON_MAP_DEFAULT[i];
	}
	core_notify_init(&reg->notify, 0, 0, 0, 0);
}

void core_registry_add(core_registry *reg, core_device_entry *dev)
{
	if (reg == 0 || dev == 0) {
		return;
	}
	dev->flink = &reg->devices;
	dev->blink = reg->devices.blink;
	reg->devices.blink->flink = dev;
	reg->devices.blink        = dev;
	reg->count++;
}

void core_registry_remove(core_registry *reg, core_device_entry *dev)
{
	if (reg == 0 || dev == 0 || dev->flink == 0) {
		return;
	}
	if (dev->live) {
		core_registry_set_live(reg, dev, 0);
	}
	dev->blink->flink = dev->flink;
	dev->flink->blink = dev->blink;
	dev->flink = 0;
	dev->blink = 0;
	reg->count--;
}

void core_registry_set_live(core_registry *reg, core_device_entry *dev,
                            int live)
{
	if (reg == 0 || dev == 0 || (!dev->live) == (!live)) {
		return;
	}
	dev->live = live ? 1 : 0;
	reg->generation++;
	reg->live_count += live ? 1 : -1;

	/*
	 * ONE EVENT FOR BOTH DIRECTIONS. Arrival and departure post the same
	 * type 99, so a listener must re-enumerate to learn which happened.
	 * That was corrected once already in the Ghidra notes and is worth
	 * restating: the event is "the interface state changed", not
	 * "a device arrived".
	 */
	core_notify_post(&reg->notify, CORE_EVENT_INTERFACE, 0, dev->handle);

	/* The last adapter leaving releases everyone parked on a notification;
	 * there will never be another event to serve them. */
	if (!live && reg->live_count == 0) {
		core_notify_flush(&reg->notify);
	}
}

static core_device_entry *registry_find(core_registry *reg, u32 handle)
{
	core_device_entry *d;

	for (d = reg->devices.flink; d != &reg->devices; d = d->flink) {
		if (d->handle == handle) {
			return d;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* the control-device dispatcher                                       */
/* ------------------------------------------------------------------ */

/*
 * Get and set the driver-wide button map, function 0x817.
 *
 * NO LENGTH VALIDATION AT ALL, which is unusual on this surface and is
 * reproduced: both loops simply stop at the shorter of the caller's buffer
 * and sixteen entries. So a zero-length call is legal and does nothing.
 *
 * The OLD map is snapshotted before the new one is applied, so a single call
 * that both sets and gets returns what was there before - the same
 * convention as the two stick tunables on the per-device surface.
 *
 * Only values below 14 are accepted; anything else leaves that entry alone.
 * Fourteen is the number of HID buttons the joystick collection declares,
 * and the map holds them zero-based.
 */
static u32 ctl_button_map(core_registry *reg, const core_ioctl *r, u32 *info)
{
	u8  old[CORE_RAW_BUTTON_BITS];
	u32 i;
	core_device_entry *d;

	for (i = 0; i < CORE_RAW_BUTTON_BITS; i++) {
		old[i] = reg->button_map[i];
	}
	for (i = 0; i < r->in_len && i < CORE_RAW_BUTTON_BITS; i++) {
		if (r->in[i] < 14u) {
			reg->button_map[i] = r->in[i];
		}
	}
	/* Push the driver-wide map down to every adapter's own copy. */
	for (d = reg->devices.flink; d != &reg->devices; d = d->flink) {
		if (d->cs == 0) {
			continue;
		}
		for (i = 0; i < CORE_RAW_BUTTON_BITS; i++) {
			d->cs->button_map[i] = reg->button_map[i];
		}
	}
	for (i = 0; i < r->out_len && i < CORE_RAW_BUTTON_BITS; i++) {
		r->out[i] = old[i];
	}
	*info = i;
	return CORE_ST_SUCCESS;
}

/*
 * The virtual-joystick switch, function 0x816.
 *
 * Sets the driver-wide flag, then marks every adapter and resubmits a NULL
 * joystick report to each one in turn. That resubmit is the only thing that
 * re-evaluates the mode - see hid-descriptor.txt section 5.2 - so without
 * this sweep the switch would not take effect until the next idle report.
 */
static u32 ctl_set_reports(core_registry *reg, const core_ioctl *r, u32 *info)
{
	core_device_entry *d;

	if (r->in_len != 4 || r->out_len != 0) {
		return CORE_ST_INVALID_PARAM;
	}
	reg->reports_enabled = (rd32(r->in) != 0);

	for (d = reg->devices.flink; d != &reg->devices; d = d->flink) {
		d->needs_resubmit = 1;
	}
	for (;;) {
		core_device_entry *hit = 0;

		for (d = reg->devices.flink; d != &reg->devices; d = d->flink) {
			if (d->needs_resubmit) {
				hit = d;
				break;
			}
		}
		if (hit == 0) {
			break;
		}
		hit->needs_resubmit = 0;
		if (hit->cs != 0) {
			hit->cs->reports_enabled = reg->reports_enabled;
			core_submit_joystick(hit->cs, 0);
		}
	}
	*info = 0;
	return CORE_ST_SUCCESS;
}

/*
 * Walk the registry, function 0x821. Input is the previous handle or zero to
 * start; output is the next live adapter's handle and the live count.
 *
 * A previous handle that is not in the list is CORE_ST_NO_SUCH_DEVICE, which
 * is how a caller learns its enumeration went stale mid-walk.
 */
static u32 ctl_enum_devices(core_registry *reg, const core_ioctl *r, u32 *info)
{
	core_device_entry *d;
	u32 prev;
	int seeking;

	if (r->in_len != 4 || r->out_len != 8) {
		return CORE_ST_INVALID_PARAM;
	}
	prev    = rd32(r->in);
	seeking = (prev != 0);

	wr32(r->out, 0);
	wr32(r->out + 4, (u32)reg->live_count);

	for (d = reg->devices.flink; d != &reg->devices; d = d->flink) {
		if (seeking) {
			if (d->handle == prev) {
				seeking = 0;
			}
			continue;
		}
		if (d->live) {
			wr32(r->out, d->handle);
			break;
		}
	}
	if (seeking) {
		return CORE_ST_NO_SUCH_DEVICE;  /* the previous handle is gone */
	}
	*info = 8;
	return CORE_ST_SUCCESS;
}

u32 core_ctl_dispatch(core_registry *reg, const core_ioctl *req,
                      core_notify_waiter *waiter, u64 now_100ns, u32 *info)
{
	u32 scratch = 0;
	u32 fn;

	if (info == 0) {
		info = &scratch;
	}
	*info = 0;
	if (reg == 0 || req == 0) {
		return CORE_ST_INVALID_PARAM;
	}
	if ((req->code & 0xFFFF0000u) != CORE_IOCTL_DEVICE_TYPE) {
		return CORE_ST_NOT_SUPPORTED;
	}
	fn = CORE_IOCTL_FN(req->code);

	switch (fn) {
	case CORE_CTL_DEVICE_COUNT:
		if (req->in_len != 0 || req->out_len != 1) {
			return CORE_ST_INVALID_PARAM;
		}
		/* A single byte, so more than 255 adapters would wrap. The list
		 * itself is unbounded; this is the original's own narrowing. */
		req->out[0] = (u8)reg->live_count;
		*info = 1;
		return CORE_ST_SUCCESS;

	case CORE_CTL_RESERVED_802:
	case CORE_CTL_RESERVED_803:
		/* Accepted, reports 0x40 bytes, does nothing whatsoever. Both
		 * codes share one handler. */
		if (req->in_len != 0x40 || req->out_len != 0x40) {
			return CORE_ST_INVALID_PARAM;
		}
		*info = 0x40;
		return CORE_ST_SUCCESS;

	case CORE_CTL_VERSION:
		if (req->in_len != 0 || req->out_len < CORE_CTL_VERSION_BYTES) {
			return CORE_ST_INVALID_PARAM;
		}
		req->out[0] = 2;
		req->out[1] = 1;
		req->out[2] = 0;
		req->out[3] = 0;
		*info = CORE_CTL_VERSION_BYTES;
		return CORE_ST_SUCCESS;

	case CORE_CTL_GENERATION:
		if (req->in_len != 0 || req->out_len != 4) {
			return CORE_ST_INVALID_PARAM;
		}
		wr32(req->out, reg->generation);
		*info = 4;
		return CORE_ST_SUCCESS;

	case CORE_CTL_UNIMPLEMENTED:
		/*
		 * VALIDATED AND THEN REFUSED. The lengths must be 0 or 4 either
		 * way, and a request that passes gets CORE_ST_NOT_SUPPORTED
		 * anyway. Reproduced because the two answers are distinguishable
		 * and a caller probing the surface can tell this code apart from
		 * one that was never assigned.
		 */
		if ((req->in_len != 0 && req->in_len != 4) ||
		    (req->out_len != 0 && req->out_len != 4)) {
			return CORE_ST_INVALID_PARAM;
		}
		return CORE_ST_NOT_SUPPORTED;

	case CORE_CTL_SET_REPORTS:
		return ctl_set_reports(reg, req, info);

	case CORE_CTL_BUTTON_MAP:
		return ctl_button_map(reg, req, info);

	case CORE_CTL_WAIT_NOTIFY:
		if (req->in_len != 0 || req->out_len != CORE_NOTIFY_BYTES) {
			return CORE_ST_INVALID_PARAM;
		}
		/* With no adapter present there will never be an event, so the
		 * request is refused rather than parked forever. */
		if (reg->live_count == 0) {
			return CORE_ST_DELETE_PENDING;
		}
		if (waiter == 0) {
			return CORE_ST_INVALID_PARAM;
		}
		if (core_notify_wait(&reg->notify, waiter)) {
			*info = CORE_NOTIFY_BYTES;
			return CORE_ST_SUCCESS;     /* served immediately */
		}
		return CORE_ST_PENDING;

	case CORE_CTL_ENUM_DEVICES:
		return ctl_enum_devices(reg, req, info);

	case CORE_CTL_LOOKUP_DEVICE: {
		core_device_entry *d;
		u32 id;

		if (req->in_len != 4 || req->out_len != 4) {
			return CORE_ST_INVALID_PARAM;
		}
		id = rd32(req->in);
		for (d = reg->devices.flink; d != &reg->devices; d = d->flink) {
			/* Resolved by the device INSTANCE NUMBER, the same value
			 * virtual mode reports as the stick position. */
			if (d->cs != 0 && (u32)d->cs->instance_id == id) {
				wr32(req->out, d->handle);
				*info = 4;
				return CORE_ST_SUCCESS;
			}
		}
		return CORE_ST_NO_SUCH_DEVICE;
	}

	default:
		break;
	}

	/*
	 * Anything else that is a per-device code is FORWARDED. The first four
	 * input bytes are the adapter handle; the rest is that surface's input.
	 *
	 * NOTE THE BUFFERS OVERLAP. The forwarded input starts four bytes into
	 * the same buffer the output is written to from offset zero, because
	 * METHOD_BUFFERED gives one allocation for both. That is safe only
	 * because every case reads all of its input before writing any output,
	 * which is a property the per-device dispatcher has to keep.
	 */
	if (req->in_len < 4) {
		return CORE_ST_INVALID_PARAM;
	}
	{
		core_device_entry *d = registry_find(reg, rd32(req->in));
		core_ioctl fwd;
		core_ioctl_env env;

		if (d == 0 || !d->live) {
			return CORE_ST_NO_SUCH_DEVICE;
		}
		fwd.code    = req->code;
		fwd.in      = req->in + 4;
		fwd.in_len  = req->in_len - 4;
		fwd.out     = req->out;
		fwd.out_len = req->out_len;

		env.cs         = d->cs;
		env.sched      = d->sched;
		env.vendor     = 0;
		env.vendor_ctx = 0;
		env.enable     = 0;
		env.enable_ctx = 0;
		env.now_100ns  = now_100ns;
		return core_ioctl_dispatch(&env, &fwd, info);
	}
}
