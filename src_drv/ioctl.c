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
		/* The transport comes from the REGISTRY ENTRY, not from this
		 * caller: a control-device request names its adapter by handle,
		 * so the only thing that knows how to reach it is the entry. */
		env.vendor     = d->vendor;
		env.vendor_ctx = d->os_ctx;
		env.enable     = d->enable;
		env.enable_ctx = d->os_ctx;
		env.now_100ns  = now_100ns;
		return core_ioctl_dispatch(&env, &fwd, info);
	}
}

/* ======================================================================
 * THE SDK COMMAND-BLOCK CHANNEL
 *
 * Ported from drv_ProcessCommandBlock (00010c30), drv_ExecuteRawCommand
 * (00010e20) and the block half of drv_ControlDeviceReadWrite (000113b0).
 * ====================================================================== */

void core_cmd_channel_init(core_cmd_channel *ch)
{
	u32 i;

	if (ch == 0) {
		return;
	}
	for (i = 0; i < CORE_CMD_BLOCK_BYTES; i++) {
		ch->block[i] = 0;
	}
	ch->swap_bytes = 0;
}

/* Reverse every four-byte group of the block. */
static void cmd_swap(u8 *block)
{
	u32 i;

	for (i = 0; i < CORE_CMD_BLOCK_BYTES; i += 4) {
		u8 t;

		t            = block[i + 0];
		block[i + 0] = block[i + 3];
		block[i + 3] = t;
		t            = block[i + 1];
		block[i + 1] = block[i + 2];
		block[i + 2] = t;
	}
}

/* The Nth adapter on the registry, or null. */
static core_device_entry *cmd_nth(core_registry *reg, s32 index)
{
	core_device_entry *d;
	s32 n = index;

	for (d = reg->devices.flink; d != &reg->devices; d = d->flink) {
		if (n < 1) {
			return d;
		}
		n--;
	}
	return 0;
}

/*
 * The CRC-8 an emulated Controller Pak write of 32 identical bytes answers
 * with.
 *
 * The original carries these four results as CONSTANTS - there is no CRC
 * computation on this path at all, just a four-way compare on the data byte.
 * They are reproduced as constants here for the same reason, and checked
 * against core_pak_data_crc8 in the harness, which is what establishes that
 * they are CRCs of 32 identical bytes and not magic numbers.
 *
 * A value that is none of the four leaves the reply byte UNWRITTEN in the
 * original, so this returns the caller's existing byte to match. See
 * known-defects.txt.
 */
static u8 cmd_emulated_crc(u32 value, u8 current)
{
	switch (value) {
	case 0xFE: return 0xE1;
	case 0x80: return 0xB8;
	case 0x01: return 0xEB;
	case 0x00: return 0x00;
	default:   return current;
	}
}

/*
 * Build a USB setup packet. The joybus command bytes go into wValue and
 * wIndex a byte at a time, LOW BYTE FIRST, and bytes the command is too
 * short to supply are zero.
 */
static void cmd_setup(u8 *setup, u8 request_type, u8 request,
                      u8 v_lo, u8 v_hi, u8 i_lo, u8 i_hi)
{
	setup[0] = request_type;
	setup[1] = request;
	setup[2] = v_lo;
	setup[3] = v_hi;
	setup[4] = i_lo;
	setup[5] = i_hi;
}

void core_cmd_exec(core_device_entry *dev, u8 *entry)
{
	core_state *cs;
	u8 *reply;
	u32 cmd_len;
	u32 reply_len;
	u8  setup[6];

	if (dev == 0 || dev->cs == 0 || entry == 0) {
		return;
	}
	cs        = dev->cs;
	cmd_len   = entry[0];
	reply_len = entry[1];
	reply     = entry + cmd_len + 2;

	/*
	 * PATH ONE: THE EMULATED PAK WRITE.
	 *
	 * Joybus command 0x03 writes 32 bytes to a Controller Pak address, so
	 * the entry is 1 command byte + 2 address bytes + 32 data = 0x23, and
	 * the reply is the single CRC-8 byte.
	 *
	 * The two addresses handled here are 0x8000 and 0xC000, carried as
	 * 0x8001 and 0xC01B because the low five bits of a joybus address word
	 * are its CRC-5. Those are the Rumble Pak's identify region and its
	 * motor register, and the driver ANSWERS THEM ITSELF rather than
	 * putting them on the bus - the real motor belongs to the effect
	 * engine, and letting an SDK client drive it directly would fight it.
	 *
	 * WHEN NO ACCESSORY IS PRESENT THE CRC COMES BACK INVERTED. That is
	 * the joybus convention for it, not an error code of the driver's.
	 */
	if (cmd_len == 0x23 && reply_len == 1 && entry[2] == 0x03 &&
	    ((entry[3] == 0x80 && entry[4] == 0x01) ||
	     (entry[3] == 0xC0 && entry[4] == 0x1B))) {
		u32 value = entry[5];

		/*
		 * Both addresses write this, which is what makes the identify
		 * sequence work: a client writes 0x80 to 0x8000 and reads it
		 * back to learn that a Rumble Pak is there.
		 */
		cs->emu_pak_value = value;
		reply[0] = cmd_emulated_crc(value, reply[0]);

		if (!cs->emu_pak_present) {
			reply[0] = (u8)~reply[0];
			return;
		}
		/* Only the motor register drives anything. */
		if (entry[3] == 0xC0 && entry[4] == 0x1B &&
		    (value == 0 || value == 1) && dev->enable != 0) {
			dev->enable(dev->os_ctx, (int)value);
		}
		return;
	}

	/*
	 * PATH TWO: THE EMULATED PAK READ.
	 *
	 * Joybus command 0x02 reads 32 bytes plus a CRC from an address; only
	 * 0x8000 is emulated. What comes back is decided by whatever the last
	 * write put in emu_pak_value, which is how an identify resolves:
	 *
	 *     32 x 0x80 -> a Rumble Pak
	 *     32 x 0x00 -> the client wrote 0xFE first, the Controller Pak probe
	 *     no pak    -> 32 x 0x00 and an INVERTED CRC, 0xFF
	 */
	if (cmd_len == 3 && reply_len == 0x21 && entry[2] == 0x02 &&
	    entry[3] == 0x80 && entry[4] == 0x01) {
		u8  fill;
		u32 i;

		if (!cs->emu_pak_present) {
			fill = 0x00;
			reply[0x20] = 0xFF;
		} else if (cs->emu_pak_value == 0xFE) {
			fill = 0x00;
			reply[0x20] = 0x00;
		} else {
			fill = 0x80;
			reply[0x20] = 0xB8;
		}
		for (i = 0; i < 0x20; i++) {
			reply[i] = fill;
		}
		return;
	}

	/*
	 * PATH THREE: THE WIRE.
	 *
	 * THIS GUARD IS UNREACHABLE and is kept only because the original has
	 * it. core_cmd_process rejects exactly the combination it tests -
	 * cmd_len > 4 and reply_len > 3 - as a structural error before it ever
	 * calls here, and that walker is the only caller. Dropping the guard
	 * would change nothing; keeping it means a future caller cannot fall
	 * off the end of the function with the entry unmarked.
	 */
	if (cmd_len >= 5 && reply_len >= 4) {
		return;
	}
	if (dev->cmd_claim == 0 || dev->cmd_xfer == 0) {
		return;
	}
	/*
	 * A BUSY SLOT IS ALSO SILENT. The original claims the slot and, when
	 * it cannot, falls straight out without setting CORE_CMD_FAILED.
	 */
	if (!dev->cmd_claim(dev->os_ctx)) {
		return;
	}

	{
		/*
		 * THE STATUS BYTE LANDS ON THE LAST COMMAND BYTE. Both forms
		 * read reply_len + 1 bytes into reply - 1, so the extra leading
		 * byte overwrites the command byte just before the reply space.
		 * It is saved and put back, which is why this works at all, and
		 * why no bounce buffer is needed.
		 */
		u8 *status_at = reply - 1;
		u8  saved     = *status_at;
		u8  status;

		if (cmd_len < 5) {
			/* Short form: one device-to-host transfer, with the
			 * command length encoded in bRequest. */
			cmd_setup(setup, 0xC0, (u8)(0x20 + cmd_len),
			          entry[2],
			          (u8)(cmd_len >= 2 ? entry[3] : 0),
			          (u8)(cmd_len >= 3 ? entry[4] : 0),
			          (u8)(cmd_len >= 4 ? entry[5] : 0));
			dev->cmd_xfer(dev->os_ctx, setup, status_at,
			              reply_len + 1, 0);
			status = *status_at;
			*status_at = saved;
			if (status == 0) {
				entry[1] |= CORE_CMD_FAILED;
			}
		} else {
			/* Long form: a host-to-device write carrying the
			 * command, then a fixed read collecting the reply. The
			 * first transfer KEEPS THE SLOT so nothing can come
			 * between the two. */
			cmd_setup(setup, 0x40, 0x20,
			          (u8)reply_len, entry[2], entry[3], entry[4]);
			dev->cmd_xfer(dev->os_ctx, setup, entry + 5,
			              cmd_len - 3, 1);

			cmd_setup(setup, 0xC0, 0x71, 0x30, 0x00, 0x00, 0x00);
			dev->cmd_xfer(dev->os_ctx, setup, status_at,
			              reply_len + 1, 0);
			status = *status_at;
			*status_at = saved;
			/*
			 * Here the status byte is a length with a valid bit,
			 * not a plain flag: bit 7 must be set and the low six
			 * bits must be the reply length that was asked for.
			 */
			if ((status & 0x80) == 0 ||
			    (u32)(status & CORE_CMD_LEN_MASK) != reply_len) {
				entry[1] |= CORE_CMD_FAILED;
			}
		}

		/* THE REPLY COMES BACK REVERSED. */
		{
			u32 i;

			for (i = 0; i < reply_len / 2; i++) {
				u8 t = reply[i];

				reply[i] = reply[reply_len - 1 - i];
				reply[reply_len - 1 - i] = t;
			}
		}
	}
}

void core_cmd_process(core_registry *reg, u8 *block, int pass, u64 now_100ns)
{
	u8 *p;
	u8 *end;
	s32 index = 0;

	(void)now_100ns;

	if (reg == 0 || block == 0) {
		return;
	}
	p   = block;
	end = block + CORE_CMD_GO;

	while (p < end) {
		core_device_entry *dev;
		u32 cmd_len;
		u32 reply_len;
		u32 span;

		cmd_len = p[0];
		if (cmd_len == CORE_CMD_PAD) {
			/* Padding advances the device index, so a client can
			 * address adapter 2 without sending anything to 0 or
			 * 1. */
			p++;
			index++;
			continue;
		}
		if (cmd_len == CORE_CMD_END) {
			return;
		}
		if (cmd_len == CORE_CMD_SKIP) {
			p++;
			continue;
		}

		/* Structural errors mark the entry and STOP the walk. */
		reply_len = p[1] & CORE_CMD_LEN_MASK;
		span      = cmd_len + reply_len;
		if (cmd_len > CORE_CMD_MAX_LEN ||
		    index >= CORE_CMD_MAX_DEVICES ||
		    reply_len > CORE_CMD_MAX_LEN ||
		    (cmd_len > 4 && reply_len > 3) ||
		    p + span + 2 >= end) {
			p[1] |= CORE_CMD_FAILED;
			return;
		}

		dev = cmd_nth(reg, index);

		if (cmd_len == 1 && reply_len == 4 && p[2] == 0x01) {
			/*
			 * THE CACHED CONTROLLER READ, and the reason this
			 * channel exists at all. Joybus command 0x01 with a
			 * four-byte reply means "give me the controller
			 * state", and it is answered FROM THE LAST INTERRUPT
			 * PACKET with no USB traffic whatsoever.
			 *
			 * The reordering is not arbitrary: the raw packet is
			 * X, Y, status, buttons-high, buttons-low, and joybus
			 * wants buttons-high, buttons-low, X, Y.
			 */
			if (pass == 1) {
				if (dev == 0 || dev->cs == 0) {
					p[1] |= CORE_CMD_FAILED;
				} else {
					p[3] = dev->cs->raw[4];
					p[4] = dev->cs->raw[3];
					p[5] = dev->cs->raw[0];
					p[6] = dev->cs->raw[1];
				}
			}
		} else if (cmd_len == 1 && reply_len == 3 &&
		           (p[2] == 0x00 || p[2] == 0xFF)) {
			/*
			 * THE IDENTIFY. This one does go on the wire, but its
			 * ANSWER is kept: bit 0 of the status byte is "an
			 * accessory is in the port", and that is what the
			 * emulated Pak above keys off.
			 *
			 * Bits 0 and 1 together being anything but 01 means
			 * the accessory has changed, so the keep-alive is
			 * dropped and the effect engine will re-arm the motor.
			 */
			if (pass == 1) {
				if (dev == 0 || dev->cs == 0) {
					p[1] |= CORE_CMD_FAILED;
				} else {
					p[1] = (u8)reply_len;
					core_cmd_exec(dev, p);
					dev->cs->emu_pak_present =
					        (p[5] & 1) == 1;
					if ((p[5] & 3) != 1) {
						dev->cs->keepalive_time = 0;
					}
				}
			}
		} else {
			/*
			 * Everything else, on pass 0. A missing device or a
			 * maximum-length reply marks the entry and CARRIES ON -
			 * and note that 0x26 passed the structural check above
			 * and is rejected here, an inconsistency of the
			 * original's that a client can observe.
			 */
			if (pass == 0) {
				if (reply_len < CORE_CMD_MAX_LEN && dev != 0) {
					p[1] = (u8)reply_len;
					core_cmd_exec(dev, p);
				} else {
					p[1] |= CORE_CMD_FAILED;
				}
			}
		}

		p += span + 2;
		index++;
	}
}

u32 core_cmd_read(core_registry *reg, core_cmd_channel *ch, u8 *buf, u32 len,
                  u64 now_100ns, u32 *info)
{
	u32 scratch = 0;

	if (info == 0) {
		info = &scratch;
	}
	*info = 0;
	if (reg == 0 || ch == 0) {
		return CORE_ST_INVALID_PARAM;
	}
	/*
	 * THE ONE-BYTE READ IS THE EXCEPTION that lets a client poll for an
	 * adapter before one exists. Everything else needs a live device.
	 */
	if (reg->live_count == 0 && len != 1) {
		return CORE_ST_NO_SUCH_DEVICE;
	}
	if (len == 0) {
		return CORE_ST_SUCCESS;
	}
	if (len > CORE_CMD_BLOCK_BYTES || buf == 0) {
		return CORE_ST_INVALID_PARAM;
	}

	if (len == 1) {
		buf[0] = (u8)reg->live_count;
	} else if (len == CORE_CMD_BLOCK_BYTES) {
		u32 i;

		for (i = 0; i < CORE_CMD_BLOCK_BYTES; i++) {
			buf[i] = ch->block[i];
		}
		if (buf[CORE_CMD_GO] == 1) {
			core_cmd_process(reg, buf, 1, now_100ns);
			/*
			 * CLEARED IN THE COPY ONLY. ch->block keeps its go
			 * flag, so every read re-runs pass 1 and a client
			 * polling the controller gets a fresh answer each time
			 * without writing the block again.
			 */
			buf[CORE_CMD_GO] = 0;
		}
		if (ch->swap_bytes) {
			cmd_swap(buf);
		}
	}
	/*
	 * Any other length in 2..0x3F succeeds having written nothing, and
	 * still reports len bytes transferred. The original's own behaviour.
	 */
	*info = len;
	return CORE_ST_SUCCESS;
}

u32 core_cmd_write(core_registry *reg, core_cmd_channel *ch, u8 *buf, u32 len,
                   u64 now_100ns, u32 *info)
{
	u32 scratch = 0;
	u32 i;

	if (info == 0) {
		info = &scratch;
	}
	*info = 0;
	if (reg == 0 || ch == 0) {
		return CORE_ST_INVALID_PARAM;
	}
	if (reg->live_count == 0) {
		return CORE_ST_NO_SUCH_DEVICE;
	}
	if (len == 0) {
		return CORE_ST_SUCCESS;
	}
	*info = len;
	if (len != CORE_CMD_BLOCK_BYTES || buf == 0) {
		return CORE_ST_INVALID_PARAM;
	}

	/*
	 * THE BYTE-ORDER WORD IS ALSO THE GO FLAG. A dword of 1 at 0x3C says
	 * the client is big-endian; swapping dword 15 moves that 1 from byte
	 * 0x3C to byte 0x3F, which is exactly where the go flag is read from
	 * below. So for such a client the swap request and the run request are
	 * the same bit, and it has no way to write a block without running it.
	 */
	if (rd32(buf + CORE_CMD_SWAP_AT) == 1) {
		cmd_swap(buf);
		ch->swap_bytes = 1;
	} else {
		ch->swap_bytes = 0;
	}

	for (i = 0; i < CORE_CMD_BLOCK_BYTES; i++) {
		ch->block[i] = buf[i];
	}
	if (buf[CORE_CMD_GO] == 1) {
		core_cmd_process(reg, ch->block, 0, now_100ns);
	}
	return CORE_ST_SUCCESS;
}

/* ======================================================================
 * THE HID MINIDRIVER CONTRACT
 *
 * Ported from drv_DispatchInternalDeviceControl (000128d2).
 *
 * These requests come from hidclass.sys, never from user mode, and they are
 * what MAKE the driver a HID device: without an answer to
 * CORE_HID_IOC_REPORT_DESC, hidclass never learns what the hardware is and
 * no keyboard, mouse or game controller is ever created.
 * ====================================================================== */

/*
 * The two string descriptors, as UTF-16LE BYTES rather than wide-character
 * literals - this file has no wchar_t and should not acquire one.
 *
 * THE CAPS ARE THE STRING SIZES INCLUDING THE TERMINATOR, which is why they
 * look arbitrary: 24 wide characters is 0x30, 9 is 0x12.
 */
static const u8 CORE_HID_MANUFACTURER[] = {
	'W',0, 'i',0, 's',0, 'h',0, ' ',0,
	'T',0, 'e',0, 'c',0, 'h',0, 'n',0, 'o',0, 'l',0, 'o',0, 'g',0,
	'i',0, 'e',0, 's',0, ',',0, ' ',0,
	'I',0, 'n',0, 'c',0, '.',0, 0,0
};

static const u8 CORE_HID_PRODUCT[] = {
	'A',0, 'd',0, 'a',0, 'p',0, 't',0, 'o',0, 'i',0, 'd',0, 0,0
};

/*
 * Copy at most cap bytes of a string into out, bounded by what the caller
 * offered.
 *
 * A SHORT BUFFER GETS AN UNTERMINATED STRING. The original copies
 * min(out_len, cap) bytes with no terminator of its own and reports that
 * many transferred, so a caller asking for ten bytes of the manufacturer
 * name gets ten bytes and no NUL. Bounded, so not a safety problem, and
 * reproduced because the byte count it reports is what hidclass believes.
 */
static u32 hid_string(u8 *out, u32 out_len, const u8 *str, u32 cap)
{
	u32 n = out_len < cap ? out_len : cap;
	u32 i;

	for (i = 0; i < n; i++) {
		out[i] = str[i];
	}
	return n;
}

u32 core_hid_ioctl(core_state *cs, u32 code, u8 *out, u32 out_len,
                   u32 in_len, u32 arg, u32 *info)
{
	u32 scratch = 0;

	if (info == 0) {
		info = &scratch;
	}
	*info = 0;
	if (cs == 0) {
		return CORE_ST_INVALID_PARAM;
	}

	switch (code) {
	case CORE_HID_IOC_DEVICE_DESC: {
		/*
		 * The 9-byte HID_DESCRIPTOR, built in place. It is a
		 * descriptor ABOUT the report descriptor: how long it is and
		 * what type it is.
		 */
		u32 len;

		if (out_len < CORE_HID_DEVICE_DESC_BYTES || out == 0) {
			return CORE_ST_BUFFER_TOO_SMALL;
		}
		core_hid_descriptor(cs->devices_mask, &len);

		out[0] = CORE_HID_DEVICE_DESC_BYTES;   /* bLength         */
		out[1] = 0x21;                         /* HID descriptor  */
		/*
		 * bcdHID = 0x0001, WHICH IS NOT A VERSION THE SPEC DEFINES -
		 * it should be 0x0100 or 0x0110. Windows does not check it,
		 * and it is reproduced rather than corrected because the
		 * value is observable to anything that reads the descriptor.
		 */
		out[2] = 0x01;
		out[3] = 0x00;
		out[4] = 0x00;                         /* bCountryCode    */
		out[5] = 0x01;                         /* bNumDescriptors */
		out[6] = 0x22;                         /* report type     */
		out[7] = (u8)(len & 0xFF);             /* wReportLength   */
		out[8] = (u8)((len >> 8) & 0xFF);
		*info = CORE_HID_DEVICE_DESC_BYTES;
		return CORE_ST_SUCCESS;
	}

	case CORE_HID_IOC_REPORT_DESC: {
		/*
		 * THE ONE REQUEST THE DRIVER CANNOT DO WITHOUT. Everything
		 * else here is detail; this is what tells Windows the device
		 * is a keyboard AND a mouse AND a game controller.
		 */
		const u8 *desc;
		u32       len;
		u32       i;

		desc = core_hid_descriptor(cs->devices_mask, &len);
		if (out_len < len || out == 0) {
			return CORE_ST_BUFFER_TOO_SMALL;
		}
		for (i = 0; i < len; i++) {
			out[i] = desc[i];
		}
		*info = len;
		return CORE_ST_SUCCESS;
	}

	case CORE_HID_IOC_WRITE_REPORT:
		/*
		 * ACCEPTED AND DISCARDED. The keyboard collection declares
		 * five LED bits as Output, so Windows sends Num Lock and Caps
		 * Lock changes down - and there is no keyboard to light up.
		 * Reporting success is right: failing would make Windows
		 * think the keyboard was broken.
		 *
		 * The byte count reported back is the INPUT length, which is
		 * how much the caller sent.
		 */
		*info = in_len;
		return CORE_ST_SUCCESS;

	case CORE_HID_IOC_GET_STRING:
		/* The index is the low half of the packed argument; the high
		 * half is a language ID the driver ignores. */
		switch (arg & 0xFFFFu) {
		case CORE_HID_STRING_MANUFACTURER:
			*info = hid_string(out, out_len, CORE_HID_MANUFACTURER,
			                   (u32)sizeof(CORE_HID_MANUFACTURER));
			return CORE_ST_SUCCESS;
		case CORE_HID_STRING_PRODUCT:
			*info = hid_string(out, out_len, CORE_HID_PRODUCT,
			                   (u32)sizeof(CORE_HID_PRODUCT));
			return CORE_ST_SUCCESS;
		case CORE_HID_STRING_SERIAL:
			/* THERE IS NO SERIAL NUMBER. Answered as present and
			 * empty rather than refused, which is what stops
			 * Windows treating the omission as a failure. */
			*info = 0;
			return CORE_ST_SUCCESS;
		default:
			return CORE_ST_NOT_SUPPORTED;
		}

	case CORE_HID_IOC_ACTIVATE:
	case CORE_HID_IOC_DEACTIVATE:
		/*
		 * WRITE-ONLY STATE. hidclass tells the driver which of the
		 * three top-level collections is open, and the driver records
		 * it and NEVER READS IT BACK - the three bytes have four
		 * references in the whole original and all four are here.
		 * Kept because the field is observable in a crash dump and
		 * because a later revision might want it.
		 *
		 * AN OUT-OF-RANGE INDEX SUCCEEDS having done nothing, which
		 * is the original's behaviour and not an oversight worth
		 * correcting: hidclass is the only caller and it does not
		 * send one.
		 */
		if (arg < CORE_HID_COLLECTIONS) {
			cs->collection_enabled[arg] =
			        (u8)(code == CORE_HID_IOC_ACTIVATE);
		}
		return CORE_ST_SUCCESS;

	case CORE_HID_IOC_ATTRIBUTES: {
		/* HID_DEVICE_ATTRIBUTES: a size, the USB ids, a version, and
		 * eleven reserved words that must be zero. */
		u32 i;

		if (out_len < CORE_HID_ATTRIBUTES_BYTES || out == 0) {
			return CORE_ST_BUFFER_TOO_SMALL;
		}
		for (i = 0; i < CORE_HID_ATTRIBUTES_BYTES; i++) {
			out[i] = 0;
		}
		wr32(out, CORE_HID_ATTRIBUTES_BYTES);
		out[4] = (u8)(CORE_USB_VENDOR_ID & 0xFF);
		out[5] = (u8)(CORE_USB_VENDOR_ID >> 8);
		out[6] = (u8)(CORE_USB_PRODUCT_ID & 0xFF);
		out[7] = (u8)(CORE_USB_PRODUCT_ID >> 8);
		out[8] = (u8)(CORE_USB_VERSION & 0xFF);
		out[9] = (u8)(CORE_USB_VERSION >> 8);
		*info = CORE_HID_ATTRIBUTES_BYTES;
		return CORE_ST_SUCCESS;
	}

	case CORE_HID_IOC_READ_REPORT:
		/*
		 * NOT ANSWERED HERE. A read parks an IRP on the report queue
		 * and completes it when a packet arrives, which is entirely
		 * the OS layer's business - AdaptoidIntDeviceControl
		 * intercepts it before calling this.
		 */
		return CORE_ST_NOT_SUPPORTED;

	default:
		return CORE_ST_NOT_SUPPORTED;
	}
}
