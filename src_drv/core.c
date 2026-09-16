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

/*
 * The default N64-bit to HID-button assignment, as drv_DriverEntry seeds
 * drv_ButtonMap. Values are zero based, so entry 13 = 0 is HID button 1.
 *
 *     A -> 1        B -> 4        Z -> 10       Start -> 9
 *     D-up -> 11    D-down -> 12  D-left -> 13  D-right -> 14
 *     L -> 7        R -> 8
 *     C-up -> 6     C-down -> 2   C-left -> 5   C-right -> 3
 *
 * Indices 8 and 9 are Reset and an unused bit. The original holds 0 there,
 * which makes them press button 1; see core_decode_buttons.
 */
static const u8 CORE_BUTTON_MAP_DEFAULT[CORE_RAW_BUTTON_BITS] = {
	0, 3, 9, 8, 10, 11, 12, 13,
	CORE_BUTTON_NONE, CORE_BUTTON_NONE, 6, 7, 5, 1, 4, 2
};

void core_init(core_state *cs, core_report_fn sink, void *sink_ctx)
{
	int i;

	if (cs == 0) {
		return;
	}
	core_zero(cs, (u32)sizeof(*cs));
	cs->sink          = sink;
	cs->sink_ctx      = sink_ctx;
	cs->devices_mask  = CORE_DEVICE_DEFAULT;
	cs->stick_clip    = CORE_STICK_CLIP_DEFAULT;
	cs->stick_stretch = CORE_STICK_STRETCH_DEF;

	/* Motor drive calibration, as drv_AddDevice seeds it. */
	cs->tune_period          = 500;
	cs->tune_duty            = 50;
	cs->tune_duty_complement = 50;
	cs->tune_strength        = 100;

	for (i = 0; i < CORE_RAW_BUTTON_BITS; i++) {
		cs->button_map[i] = CORE_BUTTON_MAP_DEFAULT[i];
	}
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

/*
 * Stick decode. Specification: ../docs/hid-descriptor.txt section 7.1 and
 * ../docs/known-defects.txt section 3, both of which come from the vendor SDK
 * rather than from guesswork.
 *
 *     1. negate Y - the N64 reports up-positive, HID wants down-positive
 *     2. scale both axes by 16
 *     3. corner stretch, if enabled
 *     4. range clipping, if set to something other than the default
 *     5. clamp to +/-1200
 *
 * THE CORNER STRETCH MULTIPLIES RAW AXES, not scaled ones, and adds the
 * result to the scaled value. That is easy to get wrong and the two notes
 * disagreed about it until it was read out of the code directly:
 *
 *     X += (|rawY| * stretch * rawX) / 256
 *
 * At the diagonal limit of 65 raw on each axis this gives
 * 65*16 + (65*10*65)/256 = 1040 + 165 = 1205, clamping to full scale. That is
 * the whole point: the N64 gate is octagonal and reaches only about 65 into a
 * corner, so a term that is zero on-axis and maximal on the diagonal is
 * exactly the correction needed. It is not an acceleration curve.
 */
static void core_decode_stick(core_state *cs, s32 raw_x, s32 raw_y)
{
	s32 x, y, ax, ay;

	/*
	 * The negation is done in 8 bits, so -(-128) stays -128 rather than
	 * becoming +128, which does not fit. Reachable only from a corrupt
	 * packet - the stick never travels that far - but left explicit.
	 */
	y = -raw_y;
	if (y > 127) {
		y = -128;
	}
	raw_y = y;

	x = raw_x * CORE_STICK_SCALE;
	y = raw_y * CORE_STICK_SCALE;

	if (cs->stick_stretch != 0) {
		ax = (raw_x < 0) ? -raw_x : raw_x;
		ay = (raw_y < 0) ? -raw_y : raw_y;
		x += (ay * (s32)cs->stick_stretch * raw_x) / 256;
		y += (ax * (s32)cs->stick_stretch * raw_y) / 256;
	}

	if (cs->stick_clip != 0 && cs->stick_clip != CORE_STICK_CLIP_DEFAULT) {
		x = (x * CORE_STICK_CLIP_DEFAULT) / (s32)cs->stick_clip;
		y = (y * CORE_STICK_CLIP_DEFAULT) / (s32)cs->stick_clip;
	}

	if (x >  CORE_STICK_LIMIT) { x =  CORE_STICK_LIMIT; }
	if (x < -CORE_STICK_LIMIT) { x = -CORE_STICK_LIMIT; }
	if (y >  CORE_STICK_LIMIT) { y =  CORE_STICK_LIMIT; }
	if (y < -CORE_STICK_LIMIT) { y = -CORE_STICK_LIMIT; }

	cs->stick_x = (s16)x;
	cs->stick_y = (s16)y;
}

/*
 * Buttons. The raw word is walked MSB first: index 0 is bit 7 of the LOW
 * byte at +4, index 15 is bit 0 of the high byte at +3. Each index selects a
 * destination HID button through the map.
 *
 * DIVERGENCE FROM THE ORIGINAL, deliberate. Indices 8 and 9 are the Reset bit
 * and an unused bit; the original table holds 0 there and the original loop
 * does not special-case it, so pressing L+R+Start asserts HID button 1 - the
 * same bit as A. That is Defect 1 in ../docs/known-defects.txt, which
 * recommends exactly the sentinel used here.
 */
static void core_decode_buttons(core_state *cs, u32 lo, u32 hi)
{
	u32 word = (lo << 8) | hi;   /* index 0 is now bit 15 */
	u16 mask = 0;
	int i;

	for (i = 0; i < CORE_RAW_BUTTON_BITS; i++) {
		if (word & (1u << (CORE_RAW_BUTTON_BITS - 1 - i))) {
			u8 dest = cs->button_map[i];
			if (dest != CORE_BUTTON_NONE) {
				mask = (u16)(mask | (u16)(1u << dest));
			}
		}
	}
	cs->buttons = mask;
}

int core_decode(core_state *cs, const u8 *raw)
{
	if (cs == 0 || raw == 0) {
		return 0;
	}

	cs->status = raw[CORE_RAW_STATUS];
	if ((cs->status & CORE_STATUS_MASK) != CORE_STATUS_VALID) {
		/*
		 * The original answers this by resetting the controller and then
		 * submitting the raw bytes unpacked, which puts garbage on the
		 * wire. Here the packet is simply rejected; the reset belongs to
		 * the transport layer and is a TODO for wdm.c.
		 */
		return 0;
	}

	core_decode_stick(cs, (s32)(s8)raw[CORE_RAW_X],
	                      (s32)(s8)raw[CORE_RAW_Y]);
	core_decode_buttons(cs, raw[CORE_RAW_BUTTONS_LO],
	                        raw[CORE_RAW_BUTTONS_HI]);
	return 1;
}

/*
 * Pack report 1. Both axes are 12-bit two's complement, packed little-endian
 * with no padding between them; see ../docs/hid-descriptor.txt section 5.
 *
 *     +0  X bits 0..7
 *     +1  X bits 8..11 low nibble, Y bits 0..3 high nibble
 *     +2  Y bits 4..11
 *     +3  buttons 1..8
 *     +4  buttons 9..16
 */
void core_pack_joystick(const core_state *cs, u8 *out)
{
	u32 x, y;

	if (cs == 0 || out == 0) {
		return;
	}

	x = (u32)cs->stick_x & 0xFFFu;
	y = (u32)cs->stick_y & 0xFFFu;

	out[0] = (u8)(x & 0xFFu);
	out[1] = (u8)(((x >> 8) & 0x0Fu) | ((y & 0x0Fu) << 4));
	out[2] = (u8)((y >> 4) & 0xFFu);
	out[3] = (u8)(cs->buttons & 0xFFu);
	out[4] = (u8)((cs->buttons >> 8) & 0xFFu);
}

void core_on_raw_packet(core_state *cs, const u8 *raw)
{
	u8  report[CORE_REPORT_MAX_BYTES];
	u32 i;

	if (cs == 0 || raw == 0) {
		return;
	}

	for (i = 0; i < CORE_RAW_PACKET_BYTES; i++) {
		cs->raw[i] = raw[i];
	}
	cs->have_raw = 1;

	/*
	 * THE PROBE GATES ALL CONTROLLER INPUT. Until it has concluded, no
	 * report is built at all - the original requires devext+0x3E4 to be 2
	 * or more before it will do anything with a poll. A state of NEEDED
	 * starts it, which is what happens on the first poll and again
	 * whenever the status byte says the accessory was pulled.
	 */
	if (cs->accessory_state < CORE_ACC_FOUND_1) {
		if (cs->accessory_state == CORE_ACC_NEEDED) {
			core_probe_start(cs);
		}
		return;
	}

	if (!core_decode(cs, raw)) {
		return;
	}

	/*
	 * A falling edge on the removed bit sends the probe back to the start,
	 * so a swapped Pak is noticed. See ../docs/hid-descriptor.txt 8.2.
	 */
	if ((cs->prev_status & CORE_STATUS_PAK_REMOVED) !=
	    (cs->status & CORE_STATUS_PAK_REMOVED) &&
	    (cs->status & CORE_STATUS_PAK_REMOVED) == 0) {
		cs->accessory_state = CORE_ACC_NEEDED;
	}

	/*
	 * The accessory came or went. The effect engine only cares once it has
	 * started sending - state PERIODIC or later - because before that there
	 * is nothing playing to interrupt.
	 *
	 * NOTE THE ASYMMETRY, which is the original's and not an oversight
	 * here: in state PERIODIC only an INSERT fires the hook. A remove in
	 * that state merely promotes the state, and it is the next change that
	 * is acted on. From state 3 onwards both directions fire.
	 */
	{
		int was = (cs->prev_status & CORE_STATUS_PAK_PRESENT) ? 1 : 0;
		int now = (cs->status & CORE_STATUS_PAK_PRESENT) ? 1 : 0;

		if (cs->effect_state > CORE_FX_STATE_TICK && was != now) {
			if (cs->effect_state > CORE_FX_STATE_PERIODIC || now) {
				core_effect_on_pak_change(cs, now);
			}
			if (cs->effect_state == CORE_FX_STATE_PERIODIC) {
				cs->effect_state = CORE_FX_STATE_PAK;
				cs->prev_status  = cs->status;
				return;
			}
		}
	}
	cs->prev_status = cs->status;

	/*
	 * The tuning mode, and the stick-moved rescan that only matters when
	 * it is OFF: a live condition effect has to be re-evaluated when the
	 * stick moves, because its output is computed from the stick.
	 */
	{
		int kick = core_tune_update(cs, raw[CORE_RAW_BUTTONS_HI],
		                           raw[CORE_RAW_BUTTONS_LO],
		                           (s32)(s8)raw[CORE_RAW_X],
		                           (s32)(s8)raw[CORE_RAW_Y]);

		if (cs->tune_mode == 0 && cs->tune_strength != 0 &&
		    (raw[CORE_RAW_X] != cs->prev_raw_x ||
		     raw[CORE_RAW_Y] != cs->prev_raw_y)) {
			int slot;

			for (slot = 0; slot < CORE_EFFECT_SLOTS; slot++) {
				if (cs->effect[slot].type == CORE_FX_TUNING &&
				    cs->effect[slot].running) {
					kick = 1;
					break;
				}
			}
		}
		cs->prev_raw_x = raw[CORE_RAW_X];
		cs->prev_raw_y = raw[CORE_RAW_Y];

		if (kick) {
			core_effect_kick(cs, cs->now_100ns);
		}
	}

	/*
	 * TODO: the script override, which lets a loaded script drive the
	 * stick instead of the hardware. See ../docs/hid-descriptor.txt 8.
	 */

	core_zero(report, (u32)sizeof(report));
	core_pack_joystick(cs, report);
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
/* The effect engine                                                   */
/* ------------------------------------------------------------------ */

/*
 * 1000 * sin, every 5 degrees from 0 to 90. Read out of drv_SineTable at
 * 00019d00. The last entry is a duplicate so that interpolating at the very
 * top of the quarter wave can still read idx + 1.
 */
static const s16 CORE_SINE_TABLE[20] = {
	   0,   87,  174,  259,  342,  423,  500,  574,  643,  707,
	 766,  819,  866,  906,  940,  966,  985,  996, 1000, 1000
};

/*
 * DELIBERATE DIVERGENCE: the original interpolates backwards.
 *
 * drv_SineLerp pairs table[idx+1] with (500 - rem) and table[idx] with rem,
 * which is the wrong way round, so within every 5-degree cell the result runs
 * DOWNWARDS and the function is a rising sawtooth rather than a sine. It is
 * correct only where two adjacent table entries are equal. Confirmed at
 * instruction level, not a decompiler artefact; see ../docs/known-defects.txt
 * section 4, which says a replacement should interpolate correctly and treat
 * anything that depended on the sawtooth as accidental.
 *
 * The divide by 50 rather than 500 is NOT part of the bug: it scales the
 * 1000-based table up to the 10000-based output.
 */
s32 core_sine_lerp(s32 angle_hundredths)
{
	s32 idx, rem;

	if (angle_hundredths < 0) {
		angle_hundredths = 0;
	}
	if (angle_hundredths > 9000) {
		angle_hundredths = 9000;
	}
	idx = angle_hundredths / 500;
	rem = angle_hundredths % 500;

	return ((s32)CORE_SINE_TABLE[idx] * (500 - rem)
	      + (s32)CORE_SINE_TABLE[idx + 1] * rem) / 50;
}

/* The periodic waveforms, all returning +/-CORE_FX_SCALE. */
static s32 core_effect_wave(u32 type, s32 pos, s32 period)
{
	s32 angle;

	switch (type) {
	case CORE_FX_SQUARE:
		/*
		 * NOTE: this is UNIPOLAR, 10000 then 0 - not +/-10000 as the
		 * Ghidra plate comment claims. The original computes
		 * ((period/2 <= pos) - 1) & 10000, which is 10000 for the first
		 * half of the period and 0 for the second.
		 */
		return (pos < period / 2) ? CORE_FX_SCALE : 0;

	case CORE_FX_SINE:
		angle = (pos * 36000) / period;
		switch (angle / 9000) {
		case 0:  return  core_sine_lerp(angle);
		case 1:  return  core_sine_lerp(18000 - angle);
		case 2:  return -core_sine_lerp(angle - 18000);
		default: return -core_sine_lerp(36000 - angle);
		}

	case CORE_FX_TRIANGLE:
		if (pos < period / 2) {
			return CORE_FX_SCALE - (pos * 20000) / period;
		}
		return ((pos - period / 2) * 20000) / period - CORE_FX_SCALE;

	case CORE_FX_SAWTOOTH_UP:
		return (pos * 20000) / period - CORE_FX_SCALE;

	case CORE_FX_SAWTOOTH_DOWN:
		return CORE_FX_SCALE - (pos * 20000) / period;

	default:
		return 0;
	}
}

s32 core_effect_axis_value(const core_state *cs,
                           const core_effect_slot *slot,
                           int axis, s32 elapsed)
{
	const core_effect_axis *ax;
	s32 v = 0;
	s32 bias = 0;
	s32 pos, remain;

	if (cs == 0 || slot == 0 || axis < 0 || axis >= CORE_EFFECT_AXES) {
		return 0;
	}
	ax = &slot->axis[axis];

	switch (slot->type) {
	case CORE_FX_CONSTANT:
		v = ax->periodic.magnitude;
		break;

	case CORE_FX_RAMP:
		if (slot->duration == 0 || slot->duration == CORE_FX_INFINITE) {
			v = ax->periodic.magnitude;               /* nothing to ramp over */
		} else {
			v = ((slot->duration - elapsed) * (s32)ax->periodic.magnitude
			     + (s32)ax->periodic.offset * elapsed) / slot->duration;
		}
		break;

	case CORE_FX_SQUARE:
	case CORE_FX_SINE:
	case CORE_FX_TRIANGLE:
	case CORE_FX_SAWTOOTH_UP:
	case CORE_FX_SAWTOOTH_DOWN:
		if (ax->periodic.period != 0) {
			pos = (((s32)ax->periodic.phase * ax->periodic.period)
			       / 36000 + elapsed) % ax->periodic.period;
			v = core_effect_wave(slot->type, pos, ax->periodic.period);
		}
		/* Only the periodic types add the offset as a bias. */
		bias = ax->periodic.offset;
		v = (v * (s32)ax->periodic.magnitude) / CORE_FX_SCALE;
		break;

	case CORE_FX_TUNING:
		/*
		 * The condition output was computed by the pre-pass from the live
		 * stick; here it is only scaled by the D-pad strength preset.
		 */
		v = (cs->tune_strength * (s32)ax->condition.output) / 100;
		break;

	default:
		return 0;
	}

	/* Envelope attack: interpolate from attack_level up to the value. */
	if (slot->attack_time > 0 && elapsed < slot->attack_time) {
		v = ((slot->attack_time - elapsed) * slot->attack_level
		     + v * elapsed) / slot->attack_time;
	}

	/* Envelope fade, only meaningful for a bounded effect. */
	if (slot->fade_time > 0 && slot->duration != CORE_FX_INFINITE) {
		remain = slot->duration - elapsed;
		if (elapsed >= slot->duration - slot->fade_time) {
			v = ((slot->fade_time - remain) * slot->fade_level
			     + remain * v) / slot->fade_time;
		}
	}

	v += bias;
	if (v >  CORE_FX_CLAMP) { v =  CORE_FX_CLAMP; }
	if (v < -CORE_FX_CLAMP) { v = -CORE_FX_CLAMP; }
	return v;
}

/*
 * THE CONDITION PRE-PASS.
 *
 * Runs once per window, before any tick is evaluated, and writes the output
 * byte of every running CORE_FX_TUNING axis from the live stick position.
 * Two consequences follow from it being a pre-pass rather than per-tick, and
 * both are the original's behaviour rather than a simplification:
 *
 *   - the condition is FROZEN for the half second the window covers, so it
 *     responds to the stick at window granularity, not tick granularity;
 *   - it MUTATES the slot, because the output lives in the slot's own bytes.
 *
 * The original also gates the whole pre-pass on TuneStrength being non-zero,
 * which matters: with strength 0 the stale output byte is left in place, but
 * the evaluator multiplies by strength and so contributes nothing anyway.
 *
 * Parameters are in +/-127 and the stick is in +/-1200, so each parameter is
 * scaled by CORE_STICK_LIMIT / CORE_FX_CLAMP on the way in.
 */
void core_effect_condition_update(core_state *cs)
{
	int i, a;

	if (cs == 0 || cs->tune_strength == 0) {
		return;
	}

	for (i = 0; i < CORE_EFFECT_SLOTS; i++) {
		core_effect_slot *slot = &cs->effect[i];

		if (slot->type != CORE_FX_TUNING || !slot->running) {
			continue;
		}
		for (a = 0; a < CORE_EFFECT_AXES; a++) {
			core_effect_condition *cond = &slot->axis[a].condition;
			s32 dead, delta, coeff, sat, limit, out;

			dead = ((s32)cond->dead_band * CORE_STICK_LIMIT)
			       / CORE_FX_CLAMP;
			if (dead < 0) {
				dead = -dead;
			}

			delta = ((a == 0) ? cs->stick_x : cs->stick_y)
			        - ((s32)cond->center * CORE_STICK_LIMIT)
			          / CORE_FX_CLAMP;

			coeff = 0;
			sat   = 0;
			if (delta < 1) {
				if (delta < -dead) {
					coeff = cond->negative_coeff;
					sat   = cond->negative_sat;
					delta += dead;
				}
			} else if (delta > dead) {
				coeff = cond->positive_coeff;
				sat   = cond->positive_sat;
				delta -= dead;
			}

			out = (coeff * delta) / CORE_STICK_LIMIT;

			/* A saturation of zero means "no limit", so use full scale;
			 * otherwise the limit is its magnitude, either sign. */
			limit = (sat == 0) ? CORE_FX_CLAMP : sat;
			if (limit < 0) {
				limit = -limit;
			}
			if (out >  limit) { out =  limit; }
			if (out < -limit) { out = -limit; }

			cond->output = (s8)out;
		}
	}
}

/*
 * Sum every running slot at one tick, run the low-pass, and combine into the
 * modulator input.
 *
 * The combination is worth understanding rather than just transcribing. Two
 * terms are summed, each a SOFT OR of the two axes - a + b - ab/200, which
 * saturates instead of simply adding:
 *
 *     steady    the axis magnitudes,          weighted by duty * period
 *     transient how far each axis moved from  weighted by (100 - duty)
 *               its low-pass                  * period
 *
 * so the duty control trades steady drive against reaction to change. Both
 * are divided by 25000.
 */
s32 core_effect_intensity(core_state *cs, s32 tick)
{
	s32 sum[CORE_EFFECT_AXES];
	s32 mag_x, mag_y, dx, dy, steady, transient, intensity;
	int i, a;
	int any = 0;

	sum[0] = 0;
	sum[1] = 0;

	for (i = 0; i < CORE_EFFECT_SLOTS; i++) {
		const core_effect_slot *slot = &cs->effect[i];
		s32 elapsed;

		if (!slot->running) {
			continue;
		}
		elapsed = tick - slot->start_tick;
		if (elapsed < 0) {
			continue;
		}
		if (slot->duration != CORE_FX_INFINITE &&
		    elapsed >= slot->duration) {
			continue;
		}
		any = 1;
		for (a = 0; a < CORE_EFFECT_AXES; a++) {
			sum[a] += core_effect_axis_value(cs, slot, a, elapsed);
		}
	}

	if (any) {
		cs->effect_idle_ticks = 0;
	} else {
		cs->effect_idle_ticks++;
	}

	mag_x = (sum[0] < 0) ? -sum[0] : sum[0];
	mag_y = (sum[1] < 0) ? -sum[1] : sum[1];

	dx = cs->filtered_x / 6 - sum[0];
	dy = cs->filtered_y / 6 - sum[1];
	cs->filtered_x = (cs->filtered_x * 5) / 6 + sum[0];
	cs->filtered_y = (cs->filtered_y * 5) / 6 + sum[1];
	if (dx < 0) { dx = -dx; }
	if (dy < 0) { dy = -dy; }

	steady    = (mag_y - (mag_y * mag_x) / 200) + mag_x;
	transient = (dy    - (dy    * dx)    / 200) + dx;

	intensity = (steady * (cs->tune_duty * cs->tune_period)) / 25000
	          + (transient * cs->tune_duty_complement * cs->tune_period)
	            / 25000;

	/*
	 * In tuning mode the effect set is ignored entirely and the motor is
	 * driven straight from the calibration, so what you feel is exactly
	 * what you are adjusting.
	 */
	if (cs->tune_mode & CORE_TUNE_ACTIVE) {
		intensity = (cs->tune_duty * cs->tune_period + 999) / 1000;
	}

	if (intensity < 1) {
		intensity = 0;
	} else if (intensity < 32) {
		/* Low-end boost: nudge small values up so they still pulse. */
		intensity = (32 - intensity) / 3 + intensity;
	}
	return intensity;
}

/*
 * The delta-sigma modulator, plus the dither that breaks up its beat.
 *
 *     accumulator += intensity
 *     if accumulator >= 100:  accumulator -= 100, emit a pulse
 *
 * A plain delta-sigma produces a very regular pattern, which is audible and
 * feelable in a motor, so a PRNG stirred with the timestamp occasionally
 * suppresses a pulse in a short burst. The burst length and the suppression
 * test are reproduced as the original computes them; the PRNG is the
 * compiler's magic-number form of state * 8 / 7.
 */
int core_effect_pulse(core_state *cs, s32 intensity, s32 base_tick)
{
	u32 r;
	s32 threshold;
	int pulse;

	/*
	 * base_tick is the time the WINDOW starts at, not the time of this
	 * tick. The original stirs the PRNG with the TimeLow argument to
	 * drv_EffectEvaluate, which is constant across all 32 ticks; only the
	 * effect evaluation above uses the per-tick time. Getting this wrong
	 * reproduces the low magnitudes exactly and then diverges the moment
	 * the dither arms, which is how it was caught.
	 */
	r = ((cs->dither_state * 8u) / 7u + (u32)base_tick) % 10000u;
	cs->dither_state = r;

	cs->accumulator += intensity;

	if (intensity > 100 && cs->dither_burst == 0) {
		cs->dither_burst = (s32)(r % 6u) + 9;
	}

	if (cs->accumulator < 100) {
		cs->dither_burst = 0;
		pulse = 0;
	} else {
		cs->accumulator -= 100;
		pulse = 1;
		if (cs->dither_burst > 0) {
			u32 q = r / 7u;

			r = ((((cs->dither_state - q) >> 1) + q) >> 2)
			    + cs->dither_state + (u32)base_tick;
			r %= 10000u;
			cs->dither_state = r;
			cs->dither_burst--;
			threshold = (s32)(((u32)((intensity - 100) / 20))
			                  / ((r % 3u) + 1u));
			if (cs->dither_burst < 6 && cs->dither_burst < threshold) {
				pulse = 0;
			}
		}
	}

	if (cs->accumulator > 100) {
		cs->accumulator = 100;
	}

	/*
	 * Tuning mode drives the motor from a 12-slot counter instead of the
	 * modulator, forcing a pulse for the first N slots of every twelve.
	 * That gives a steady, obviously periodic buzz to calibrate against
	 * rather than the dithered pattern the modulator produces.
	 */
	if (cs->tune_mode & CORE_TUNE_ACTIVE) {
		s32 slots = cs->tune_duty_complement * cs->tune_period;

		if (slots > 0) {
			slots = (slots + 8000) / 11112 + 1;
		}
		if (cs->tune_counter < slots) {
			pulse = 1;
		}
		cs->tune_counter++;
		if (cs->tune_counter > 11) {
			cs->tune_counter = 0;
		}
	}
	return pulse;
}

/* Ring slots are addressed relative to the head and wrap both ways. */
static int core_ring_index(const core_state *cs, s32 offset)
{
	s32 i = cs->ring_head + offset;

	while (i >= CORE_RING_SIZE) { i -= CORE_RING_SIZE; }
	while (i < 0)               { i += CORE_RING_SIZE; }
	return (int)i;
}

void core_effect_ring_reset(core_state *cs)
{
	if (cs != 0) {
		cs->ring_count = 0;
	}
}

/*
 * Evaluate a window against the ring. See core.h for what the return value
 * and the lookahead argument mean.
 *
 * The verify pass is the whole point of the ring: re-run the ticks that are
 * already stored and compare each pulse with what was stored for it. If they
 * all agree, the controller already holds a correct bitmap and the driver
 * sends nothing. The first disagreement abandons verification, truncates the
 * ring at that tick, widens the window back out to 32 and carries on
 * computing - so a change part way through a window costs one transfer, not
 * a stall.
 *
 * Note the carry - filter, accumulator, dither burst - is seeded from the
 * entry BEFORE the first tick evaluated, which is what makes a partial
 * re-evaluation produce the same numbers as a full one.
 */
int core_effect_evaluate(core_state *cs, int lookahead, s32 tick, u8 *payload)
{
	s32 skip, idx, end;
	int verify, i, r;

	if (cs == 0 || payload == 0) {
		return 0;
	}

	/*
	 * Where the requested time sits in the ring. Before the base, or past
	 * the end of what is stored, means the ring cannot help: start over.
	 */
	skip = tick - cs->ring_base_tick;
	if (skip < 1 || skip > cs->ring_count) {
		cs->ring_base_tick = tick;
		cs->ring_count     = 0;
		skip               = 0;
	}

	if (lookahead == 0) {
		idx    = cs->ring_count - skip;      /* EXTEND from the end */
		end    = CORE_EFFECT_WINDOW;
		verify = 0;
	} else {
		idx    = 0;
		end    = cs->ring_count - skip;
		verify = 1;
		if (end == 0) {
			end    = CORE_EFFECT_WINDOW;     /* nothing to verify   */
			verify = 0;
		}
	}

	/* Seed the carry from the tick immediately before this window. */
	if (idx + skip > 0 && idx + skip <= cs->ring_count) {
		i = core_ring_index(cs, idx + skip - 1);
		cs->filtered_x   = cs->ring[i].filtered_x;
		cs->filtered_y   = cs->ring[i].filtered_y;
		cs->accumulator  = cs->ring[i].accumulator;
		cs->dither_burst = cs->ring[i].dither_burst;
		cs->tune_counter = cs->ring[i].tune_counter;
	}

	/* Sample the stick into every condition once, before any tick. */
	core_effect_condition_update(cs);

	while (idx < end) {
		s32 intensity;
		int pulse;

		/* Verification cannot continue past what the ring holds. */
		if (verify && cs->ring_count <= idx + skip) {
			verify = 0;
			end    = CORE_EFFECT_WINDOW;
		}

		/* Make room for this tick: grow, or roll the oldest out. */
		if (cs->ring_count <= idx + skip) {
			if (cs->ring_count == CORE_RING_SIZE) {
				cs->ring_head = core_ring_index(cs, 1);
				cs->ring_base_tick++;
				skip--;
			} else {
				cs->ring_count++;
			}
		}

		intensity = core_effect_intensity(cs, tick + idx);
		pulse     = core_effect_pulse(cs, intensity, tick);

		r = core_ring_index(cs, idx + skip);

		if (verify && cs->ring[r].pulse != pulse) {
			verify         = 0;
			end            = CORE_EFFECT_WINDOW;
			cs->ring_count = idx + 1 + skip;
		}

		cs->ring[r].pulse        = pulse;
		cs->ring[r].intensity    = intensity;
		cs->ring[r].filtered_x   = cs->filtered_x;
		cs->ring[r].filtered_y   = cs->filtered_y;
		cs->ring[r].accumulator  = cs->accumulator;
		cs->ring[r].dither_burst = cs->dither_burst;
		cs->ring[r].tune_counter = cs->tune_counter;
		idx++;
	}

	if (verify) {
		return 1;                       /* nothing changed, send nothing */
	}

	for (i = 0; i < CORE_EFFECT_PAYLOAD; i++) {
		payload[i] = 0;
	}
	for (i = 0; i < CORE_EFFECT_WINDOW; i++) {
		r = core_ring_index(cs, i + skip);
		if (cs->ring[r].pulse) {
			payload[i >> 3] = (u8)(payload[i >> 3] | (1u << (i & 7)));
		}
	}
	return 0;
}

/*
 * One fresh window with no history. Equivalent to evaluating with the ring
 * empty, which is the COMPUTE path.
 */
void core_effect_window(core_state *cs, s32 start_tick, u8 *payload)
{
	if (cs == 0 || payload == 0) {
		return;
	}
	cs->ring_count     = 0;
	cs->ring_head      = 0;
	cs->ring_base_tick = start_tick;
	core_effect_evaluate(cs, 1, start_tick, payload);
}

/* ------------------------------------------------------------------ */
/* On-controller tuning mode                                           */
/* ------------------------------------------------------------------ */

/*
 * Hold L + R + Z + Start and the controller stops being input: the stick
 * and D-pad become live calibration controls for the motor. Nothing in user
 * mode is involved, which is presumably the point - you can feel the motor
 * while you adjust it.
 *
 *     stick X   the motor PERIOD, on a quadratic curve. About 250 at
 *               centre and 1000 at full right.
 *     stick Y   the DUTY cycle, 0 to 100, and its complement
 *     D-pad     one of five STRENGTH presets
 *
 * The entry test accepts Reset in place of Start, because pressing
 * L + R + Start is exactly what makes an N64 controller assert its own
 * Reset bit - so the combination would otherwise be unusable.
 */

/* The five D-pad strength presets, indexed by the D-pad nibble. */
static s32 core_tune_strength_for(u32 dpad)
{
	switch (dpad) {
	case 0x1:                       /* D-right          */
	case 0x2: return 25;            /* D-left           */
	case 0x4: return 0;             /* D-down           */
	case 0x5:                       /* D-down + right   */
	case 0x6: return 12;            /* D-down + left    */
	case 0x8: return 100;           /* D-up             */
	case 0x9:                       /* D-up + right     */
	case 0xA: return 50;            /* D-up + left      */
	default:  return -1;            /* leave it alone   */
	}
}

int core_tune_update(core_state *cs, u8 buttons_hi, u8 buttons_lo,
                     s32 raw_x, s32 raw_y)
{
	s32 was_mode, x, y, period, duty, strength;
	int kick = 0;

	if (cs == 0) {
		return 0;
	}

	/* Entry: L + R, no C buttons, Z, nothing else on the face, and either
	 * Start or the Reset bit that L + R + Start itself asserts. */
	if ((buttons_hi & CORE_BTN_HI_SHOULDER_C) == CORE_BTN_HI_LR &&
	    (buttons_lo & CORE_BTN_LO_NO_START) == CORE_BTN_LO_Z &&
	    ((buttons_hi & CORE_BTN_HI_RESET) ||
	     (buttons_lo & CORE_BTN_LO_START))) {
		if ((cs->tune_mode & CORE_TUNE_ACTIVE) == 0) {
			cs->tune_period = 0;    /* a fresh entry starts from zero */
		}
		cs->tune_mode = CORE_TUNE_ACTIVE | CORE_TUNE_HELD;
	}

	was_mode = cs->tune_mode;

	if (cs->tune_mode & CORE_TUNE_ACTIVE) {
		/*
		 * Y IS NEGATED HERE, exactly as it is for a normal report. In the
		 * original that is not a separate step: the stick path overwrites
		 * its Y local with the negated value and the tuning block, lower
		 * in the same function, reads that local. So pushing the stick UP
		 * lowers the duty cycle rather than raising it - confirmed by
		 * emulation, where a full-up stick gives duty 0 and complement
		 * 100.
		 */
		y = -raw_y;
		if (y > 127) {
			y = -128;               /* the same 8-bit wrap */
		}

		x = raw_x * CORE_STICK_SCALE;
		y = y * CORE_STICK_SCALE;
		if (x >  CORE_STICK_LIMIT) { x =  CORE_STICK_LIMIT; }
		if (x < -CORE_STICK_LIMIT) { x = -CORE_STICK_LIMIT; }
		if (y >  CORE_STICK_LIMIT) { y =  CORE_STICK_LIMIT; }
		if (y < -CORE_STICK_LIMIT) { y = -CORE_STICK_LIMIT; }

		x += CORE_STICK_LIMIT;              /* 0..2400 */
		duty = (y + CORE_STICK_LIMIT) / 24; /* 0..100  */

		/*
		 * Period is quadratic in the stick, which spreads the useful
		 * range over the travel instead of bunching it at one end.
		 * Guarded against a divide by zero at the far left.
		 */
		if (x < 76 || x > 2399999) {
			period = 0;
		} else {
			period = 2400000 / x;
			period = 1000000000 / (period * period);
		}

		if (period != cs->tune_period || duty != cs->tune_duty) {
			kick = 1;
		}
		cs->tune_period          = period;
		cs->tune_duty            = duty;
		cs->tune_duty_complement = 100 - duty;

		strength = core_tune_strength_for(buttons_lo & CORE_BTN_LO_DPAD);
		if (strength >= 0) {
			cs->tune_strength = strength;
		}
	}

	/*
	 * Exit is two-stage, and the D-pad is deliberately not part of it so
	 * that holding a direction keeps you in the mode.
	 *
	 * Releasing the shoulder and face buttons drops the HELD bit, leaving
	 * the mode ACTIVE and still tracking the stick. Pressing any of them
	 * again from that state leaves the mode altogether and kicks the
	 * engine, so the new calibration takes effect.
	 */
	if ((buttons_hi & CORE_BTN_HI_SHOULDER_C) == 0 &&
	    (buttons_lo & CORE_BTN_LO_FACE) == 0) {
		cs->tune_mode = was_mode & ~CORE_TUNE_HELD;
	} else if (was_mode == CORE_TUNE_ACTIVE) {
		cs->tune_mode = 0;
		kick = 1;
	}
	return kick;
}

/* ------------------------------------------------------------------ */
/* The effect send chain                                               */
/* ------------------------------------------------------------------ */

static int core_effect_send_periodic(core_state *cs, u64 now_100ns);

/* Hand one transfer to the transport and record who owns its completion. */
static int core_effect_issue(core_state *cs, u8 request, u16 value,
                             u16 index, u8 next)
{
	core_vendor_req req;

	if (cs->vendor == 0) {
		return 0;
	}
	core_zero(&req, (u32)sizeof(req));
	req.bmRequestType = (u8)CORE_VENDOR_OUT;
	req.bRequest      = request;
	req.wValue        = value;
	req.wIndex        = index;

	cs->effect_next = next;
	return cs->vendor(cs->vendor_ctx, &req) ? 1 : 0;
}

/*
 * The bitmap travels in the setup packet rather than a data stage: four
 * bytes fit exactly in wValue and wIndex, which is presumably why the
 * design needs no data stage at all.
 */
static int core_effect_send_bitmap(core_state *cs, u8 request,
                                   const u8 *payload, u8 next)
{
	u16 value = (u16)(payload[0] | ((u16)payload[1] << 8));
	u16 index = (u16)(payload[2] | ((u16)payload[3] << 8));

	return core_effect_issue(cs, request, value, index, next);
}

/*
 * Poke a vendor register at most once every three seconds, then continue to
 * the periodic send.
 *
 * The test is that the last poke is in the past AND no more than three
 * seconds old. Failing EITHER half pokes again, so a clock that jumps
 * backwards re-arms rather than going quiet - which matters, because the
 * whole point is that the accessory stops rumbling if it stops hearing from
 * the driver.
 */
static int core_effect_keepalive(core_state *cs, u64 now_100ns)
{
	if (cs->claim_effect_tick) {
		return core_effect_tick(cs, now_100ns);
	}

	if (cs->keepalive_time <= now_100ns &&
	    now_100ns <= cs->keepalive_time + CORE_FX_KEEPALIVE_100NS) {
		return core_effect_send_periodic(cs, now_100ns);
	}

	cs->keepalive_time = now_100ns;
	return core_effect_issue(cs, CORE_FX_CMD_KEEPALIVE,
	                         CORE_FX_KEEPALIVE_VALUE,
	                         CORE_FX_KEEPALIVE_INDEX,
	                         CORE_FX_NEXT_PERIODIC);
}

/*
 * Extend the ring by one window and send it. This transfer carries NO
 * completion: the chain ends here and the next timer tick starts it again.
 */
static int core_effect_send_periodic(core_state *cs, u64 now_100ns)
{
	u8 payload[CORE_EFFECT_PAYLOAD];

	if (cs->claim_effect_tick) {
		return core_effect_tick(cs, now_100ns);
	}

	cs->effect_state = CORE_FX_STATE_PERIODIC;
	core_effect_evaluate(cs, 0, cs->next_tick, payload);
	cs->next_tick += CORE_EFFECT_WINDOW;

	return core_effect_send_bitmap(cs, CORE_FX_CMD_PERIODIC, payload,
	                               CORE_FX_NEXT_NONE);
}

int core_effect_tick(core_state *cs, u64 now_100ns)
{
	u8  payload[CORE_EFFECT_PAYLOAD];
	s32 tick;

	if (cs == 0) {
		return 0;
	}
	cs->claim_effect_tick = 0;
	tick = (s32)(now_100ns / CORE_TICK_100NS);

	if (core_effect_evaluate(cs, 1, tick, payload) == 0) {
		/* The bitmap changed, so it has to go out. */
		cs->next_tick    = tick + CORE_EFFECT_WINDOW;
		cs->effect_state = CORE_FX_STATE_TICK;
		/*
		 * TODO: the original also clears its pending pak-insert and
		 * pak-remove claims here, which this layer does not own yet.
		 */
		return core_effect_send_bitmap(cs, CORE_FX_CMD_TICK, payload,
		                               CORE_FX_NEXT_KEEPALIVE);
	}

	/*
	 * Nothing changed. If the last thing sent was a tick window, keep the
	 * chain alive anyway - otherwise the accessory would eventually stop
	 * hearing from the driver.
	 */
	if (cs->effect_state == CORE_FX_STATE_TICK) {
		return core_effect_keepalive(cs, now_100ns);
	}
	return 0;
}

void core_set_vendor_claim(core_state *cs, core_vendor_claim_fn claim)
{
	if (cs != 0) {
		cs->vendor_claim = claim;
	}
}

/* Take the single vendor slot, or record that this work still wants it. */
static int core_effect_claim(core_state *cs, int *claim_flag)
{
	if (cs->vendor_claim == 0) {
		return 1;               /* no arbitration installed */
	}
	if (cs->vendor_claim(cs->vendor_ctx)) {
		return 1;
	}
	*claim_flag = 1;
	return 0;
}

/*
 * Extend the ring by a window and send it, tagged with the sub-command that
 * says why. The idle case takes over entirely: once the engine has had
 * nothing to play for more than four ticks it stops the motor instead and
 * throws the precomputed ticks away, because they describe silence.
 */
static int core_effect_send_update(core_state *cs, u8 sub)
{
	u8 payload[CORE_EFFECT_PAYLOAD];

	core_effect_evaluate(cs, 0, cs->next_tick, payload);
	cs->next_tick += CORE_EFFECT_WINDOW;

	if (cs->effect_idle_ticks > 4) {
		core_effect_ring_reset(cs);
		return core_effect_issue(cs, CORE_FX_CMD_STOP, 0, 0,
		                         CORE_FX_NEXT_NONE);
	}
	return core_effect_send_bitmap(cs, sub, payload, CORE_FX_NEXT_NONE);
}

static int core_effect_on_pak_insert(core_state *cs)
{
	cs->claim_pak_insert = 0;
	return core_effect_send_update(cs, CORE_FX_CMD_PAK_INSERT);
}

static int core_effect_on_pak_remove(core_state *cs)
{
	cs->claim_pak_remove = 0;
	return core_effect_send_update(cs, CORE_FX_CMD_PAK_REMOVE);
}

void core_effect_on_pak_change(core_state *cs, int present)
{
	if (cs == 0) {
		return;
	}
	if (present) {
		if (core_effect_claim(cs, &cs->claim_pak_insert)) {
			core_effect_on_pak_insert(cs);
		}
	} else {
		if (core_effect_claim(cs, &cs->claim_pak_remove)) {
			core_effect_on_pak_remove(cs);
		}
	}
}

void core_effect_update_complete(core_state *cs)
{
	if (cs != 0) {
		cs->effect_state = CORE_FX_STATE_TICK;
	}
}

int core_effect_run(core_state *cs, u64 now_100ns)
{
	if (cs == 0) {
		return 0;
	}
	if (!core_effect_claim(cs, &cs->claim_effect_tick)) {
		return 0;
	}
	if (core_effect_tick(cs, now_100ns)) {
		return 1;
	}
	/* Nothing went out, so hand the slot on rather than holding it. */
	return core_effect_run_deferred(cs, now_100ns);
}

int core_effect_kick(core_state *cs, u64 now_100ns)
{
	if (cs == 0) {
		return 0;
	}
	cs->effect_idle_ticks = 0;
	return core_effect_run(cs, now_100ns);
}

/*
 * Drain deferred work in the original's fixed priority order, stopping as
 * soon as something puts a transfer in flight - its completion will drain
 * the rest. The idle-command claim sits above these in the original and is
 * not part of the effect engine, so it is not handled here.
 */
int core_effect_run_deferred(core_state *cs, u64 now_100ns)
{
	int guard = 0;

	if (cs == 0) {
		return 0;
	}
	while (guard++ < CORE_EFFECT_SLOTS) {
		if (cs->claim_effect_tick) {
			if (core_effect_tick(cs, now_100ns)) {
				return 1;
			}
			continue;
		}
		if (cs->claim_pak_insert) {
			if (core_effect_on_pak_insert(cs)) {
				return 1;
			}
			continue;
		}
		if (cs->claim_pak_remove) {
			if (core_effect_on_pak_remove(cs)) {
				return 1;
			}
			continue;
		}
		break;                  /* nothing pending; the slot is free */
	}
	return 0;
}

int core_effect_complete(core_state *cs, u64 now_100ns)
{
	u8 next;

	if (cs == 0) {
		return 0;
	}
	next = cs->effect_next;
	cs->effect_next = CORE_FX_NEXT_NONE;

	switch (next) {
	case CORE_FX_NEXT_KEEPALIVE:
		return core_effect_keepalive(cs, now_100ns);
	case CORE_FX_NEXT_PERIODIC:
		return core_effect_send_periodic(cs, now_100ns);
	default:
		return 0;
	}
}

/* ------------------------------------------------------------------ */
/* The accessory probe                                                 */
/* ------------------------------------------------------------------ */

/*
 * A fixed sequence of vendor control transfers that asks the adapter what is
 * plugged into the controller's accessory port. Specification and evidence:
 * ../docs/usb-transport.txt section 4.
 *
 *     steps 1..5   OUT 0x72, five register writes
 *     step  6      IN  0x21, the N64 request-info command, 4 bytes back
 *     classify     conclude, or reselect the port and try once more
 *     step  7      OUT 0x74 wValue 0x00C6, reselect
 *     step  8      IN  0x21 again
 *     classify     conclude, or give up
 *     step  9      OUT 0x74 wValue 0x00A6, final
 *
 * The reply is NOT byte-reversed, because these bypass the N64 transaction
 * wrapper that would have reversed it:
 *
 *     [0] 0x03, the N64 reply byte count   [1] the N64 status byte
 *     [2] 0x00  }  the 0x05 0x00 controller identifier, reversed
 *     [3] 0x05  }
 *
 * WHAT IS NOT ESTABLISHED, and is not guessed at here: what request 0x72
 * actually writes, and which accessory ends up reporting FOUND_1 rather than
 * FOUND_2. The sequence is reproduced because it works, not because its
 * meaning is known.
 */

#define CORE_PROBE_OUT_STEPS    5

#define CORE_PROBE_STEP_READ1   (CORE_PROBE_OUT_STEPS)      /* 5 */
#define CORE_PROBE_STEP_SEL1    (CORE_PROBE_OUT_STEPS + 1)  /* 6 */
#define CORE_PROBE_STEP_READ2   (CORE_PROBE_OUT_STEPS + 2)  /* 7 */
#define CORE_PROBE_STEP_SEL2    (CORE_PROBE_OUT_STEPS + 3)  /* 8 */
#define CORE_PROBE_STEP_DONE    (CORE_PROBE_OUT_STEPS + 4)  /* 9 */

#define CORE_VENDOR_REG_WRITE   0x72u   /* the five setup writes  */
#define CORE_VENDOR_N64         0x21u   /* short-form transaction */
#define CORE_VENDOR_RESELECT    0x74u   /* accessory port select  */

/* The five register writes, in order. */
static const struct {
	u16 value;
	u16 index;
} CORE_PROBE_WRITES[CORE_PROBE_OUT_STEPS] = {
	{ 0x0038, 0x0015 },
	{ 0x0039, 0x0012 },
	{ 0x003A, 0x0023 },
	{ 0xFF20, 0x0004 },
	{ 0xFD23, 0x0000 }
};

/* ------------------------------------------------------------------ */
/* the raw N64 controller-bus transaction                              */
/* ------------------------------------------------------------------ */

/*
 * One N64 bus transaction, synchronously. tx holds the command bytes, rx
 * receives the reply, and *actual is set to how many bytes came back.
 *
 * Returns 1 if the transaction was attempted, 0 if it was refused. A return
 * of 1 with *actual == 0 means the device answered but the reply was
 * rejected.
 *
 * TWO ENCODINGS, chosen by tx_len.
 *
 * SHORT FORM, tx_len < 5. The command bytes ride in the setup packet and the
 * reply is the data stage:
 *
 *     bmRequestType  0xC0                 vendor IN
 *     bRequest       0x20 + tx_len        so 0x21..0x24
 *     wValue         tx[0], tx[1]         zero-filled past tx_len
 *     wIndex         tx[2], tx[3]
 *
 * LONG FORM, tx_len >= 5. Two transfers. The command and its data go out:
 *
 *     bmRequestType  0x40                 vendor OUT
 *     bRequest       0x20
 *     wValue         rx_len, tx[0]
 *     wIndex         tx[1], tx[2]
 *     data           tx[3 ..]
 *
 * then the reply is collected with bRequest 0x71, wValue 0x0030, wIndex 0.
 * The long form validates its status byte - bit 7 set and the low six bits
 * equal to rx_len - where the short form only requires a non-zero first byte.
 *
 * THE REPLY IS BYTE-REVERSED before returning: the device delivers N64
 * replies last-byte-first and this undoes it. Anything going through the
 * vendor seam directly, as the accessory probe does, sees the raw reversed
 * order instead - which is why core_probe_classify reads its identifier
 * backwards.
 */
int core_n64_transaction(core_state *cs, const u8 *tx, s32 tx_len,
                         u8 *rx, s32 rx_len, s32 *actual)
{
	u8  reply[CORE_N64_RX_MAX + 1];
	core_vendor_req req;
	s32 i;
	int ok;

	/*
	 * DIVERGENCE, and it matters. The original leaves *ActualRx untouched
	 * on every path that refuses, so a caller that did not pre-zero it
	 * reads a stale length. Three of its four callers do pre-zero; this
	 * always does.
	 */
	if (actual != 0) {
		*actual = 0;
	}
	if (cs == 0 || cs->vendor_sync == 0 || tx == 0 || rx == 0) {
		return 0;
	}
	if (tx_len < 1) {
		return 0;
	}

	/*
	 * The original's guard, reproduced: it proceeds only when tx_len < 5
	 * OR rx_len < 4, so a long-form transfer expecting four or more bytes
	 * back is silently refused. That is a real restriction on the
	 * passthrough IOCTL and not an accident of this reading - the two arms
	 * below are selected by the same tx_len test.
	 */
	if (!(tx_len < 5 || rx_len < 4)) {
		return 0;
	}

	/*
	 * DIVERGENCE: the receive length is bounded. The original checks only
	 * that it is not too SMALL, then asks the device for rx_len + 1 bytes
	 * into a 64-byte stack buffer and copies rx_len bytes back out of it.
	 * See ../docs/known-defects.txt section 13; this is the one defect
	 * found in the driver that is reachable from user mode and matters.
	 */
	if (rx_len < 0 || rx_len > CORE_N64_RX_MAX) {
		return 0;
	}

	/* One control transfer at a time. The original latches the vendor slot
	 * here and relies on the transport to release it. */
	if (cs->vendor_claim != 0 && !cs->vendor_claim(cs->vendor_ctx)) {
		return 0;
	}

	for (i = 0; i <= rx_len; i++) {
		reply[i] = 0;
	}

	if (tx_len < 5) {
		u8 b1 = (tx_len >= 2) ? tx[1] : 0;
		u8 b2 = (tx_len >= 3) ? tx[2] : 0;
		u8 b3 = (tx_len >= 4) ? tx[3] : 0;

		req.bmRequestType = CORE_VENDOR_IN;
		req.bRequest      = (u8)(CORE_VENDOR_N64_BASE + (u8)tx_len);
		req.wValue        = (u16)((u16)tx[0] | ((u16)b1 << 8));
		req.wIndex        = (u16)((u16)b2 | ((u16)b3 << 8));
		req.wLength       = (u16)(rx_len + 1);
		if (!cs->vendor_sync(cs->vendor_ctx, &req, reply,
		                     (u32)(rx_len + 1))) {
			return 1;
		}
		/* The short form accepts anything with a non-zero first byte. */
		ok = (reply[0] != 0);
	} else {
		req.bmRequestType = CORE_VENDOR_OUT;
		req.bRequest      = CORE_VENDOR_N64_BASE;
		req.wValue        = (u16)((u16)(u8)rx_len | ((u16)tx[0] << 8));
		req.wIndex        = (u16)((u16)tx[1] | ((u16)tx[2] << 8));
		req.wLength       = 0;
		/* The data stage is the command bytes past the three in wValue
		 * and wIndex. */
		if (!cs->vendor_sync(cs->vendor_ctx, &req, (u8 *)tx + 3,
		                     (u32)(tx_len - 3))) {
			return 1;
		}

		req.bmRequestType = CORE_VENDOR_IN;
		req.bRequest      = CORE_VENDOR_N64_FETCH;
		req.wValue        = 0x0030;
		req.wIndex        = 0;
		req.wLength       = (u16)(rx_len + 1);
		if (!cs->vendor_sync(cs->vendor_ctx, &req, reply,
		                     (u32)(rx_len + 1))) {
			return 1;
		}
		/* The long form checks its status byte properly. */
		ok = ((reply[0] & 0x80u) != 0) &&
		     ((s32)(reply[0] & 0x3Fu) == rx_len);
	}

	if (!ok) {
		return 1;               /* *actual stays 0 */
	}

	for (i = 0; i < rx_len; i++) {
		rx[i] = reply[i + 1];
	}

	/*
	 * Reverse in place. DIVERGENCE: the original reverses whether or not
	 * the reply was accepted, so a rejected transaction scrambles whatever
	 * the caller happened to have in its buffer. Reversing bytes nobody
	 * wrote is not behaviour worth preserving.
	 */
	for (i = 0; i < rx_len / 2; i++) {
		u8 t = rx[i];

		rx[i] = rx[rx_len - 1 - i];
		rx[rx_len - 1 - i] = t;
	}
	if (actual != 0) {
		*actual = rx_len;
	}
	return 1;
}

/*
 * Reset the N64 controller: a fire-and-forget vendor OUT with no data stage.
 *
 *     bmRequestType 0x40, bRequest 0x72, wValue 0x0F20, wIndex 0
 *
 * drv_BuildJoystickReport sends this when a packet arrives with a status
 * byte it does not recognise, which is the case core_decode currently
 * rejects outright. Note that the original then goes on to submit the
 * unrecognised packet anyway; only the reset is reproduced here.
 *
 * Returns 1 if the request was issued, 0 if the vendor slot was busy.
 */
int core_controller_reset(core_state *cs)
{
	core_vendor_req req;

	if (cs == 0 || cs->vendor == 0) {
		return 0;
	}
	if (cs->vendor_claim != 0 && !cs->vendor_claim(cs->vendor_ctx)) {
		return 0;
	}
	req.bmRequestType = CORE_VENDOR_OUT;
	req.bRequest      = CORE_VENDOR_RESET;
	req.wValue        = 0x0F20;
	req.wIndex        = 0;
	req.wLength       = 0;
	return cs->vendor(cs->vendor_ctx, &req) ? 1 : 0;
}

void core_set_vendor(core_state *cs, core_vendor_fn fn, void *ctx)
{
	if (cs == 0) {
		return;
	}
	cs->vendor     = fn;
	cs->vendor_ctx = ctx;
}

void core_set_vendor_sync(core_state *cs, core_vendor_sync_fn fn)
{
	if (cs != 0) {
		cs->vendor_sync = fn;
	}
}

/* Give up and let a later poll try again. */
static void core_probe_abandon(core_state *cs)
{
	cs->accessory_state = CORE_ACC_NEEDED;
	cs->probe_step      = CORE_PROBE_STEP_DONE;
}

/* Build and hand over the transfer for the current step. */
static int core_probe_issue(core_state *cs)
{
	core_vendor_req req;

	core_zero(&req, (u32)sizeof(req));

	if (cs->probe_step < CORE_PROBE_OUT_STEPS) {
		req.bmRequestType = (u8)CORE_VENDOR_OUT;
		req.bRequest      = (u8)CORE_VENDOR_REG_WRITE;
		req.wValue        = CORE_PROBE_WRITES[cs->probe_step].value;
		req.wIndex        = CORE_PROBE_WRITES[cs->probe_step].index;
	} else if (cs->probe_step == CORE_PROBE_STEP_READ1 ||
	           cs->probe_step == CORE_PROBE_STEP_READ2) {
		req.bmRequestType = (u8)CORE_VENDOR_IN;
		req.bRequest      = (u8)CORE_VENDOR_N64;
		req.wLength       = CORE_PROBE_REPLY_BYTES;
	} else if (cs->probe_step == CORE_PROBE_STEP_SEL1 ||
	           cs->probe_step == CORE_PROBE_STEP_SEL2) {
		req.bmRequestType = (u8)CORE_VENDOR_OUT;
		req.bRequest      = (u8)CORE_VENDOR_RESELECT;
		req.wValue        = (cs->probe_step == CORE_PROBE_STEP_SEL1)
		                    ? 0x00C6 : 0x00A6;
	} else {
		return 0;
	}

	if (cs->vendor == 0 || !cs->vendor(cs->vendor_ctx, &req)) {
		/*
		 * The transport refused, which in the original means its single
		 * vendor slot is already in flight. Retried from a later poll.
		 */
		core_probe_abandon(cs);
		return 0;
	}
	return 1;
}

int core_probe_start(core_state *cs)
{
	if (cs == 0 || cs->vendor == 0) {
		return 0;
	}
	if (cs->accessory_state == CORE_ACC_PROBING) {
		return 0;               /* already running */
	}
	cs->accessory_state = CORE_ACC_PROBING;
	cs->probe_step      = 0;
	return core_probe_issue(cs);
}

/* Does the four-byte reply say an N64 controller answered? */
static int core_probe_identified(const core_state *cs)
{
	return cs->probe_reply[0] == 0x03 &&
	       cs->probe_reply[3] == 0x05 &&
	       cs->probe_reply[2] == 0x00;
}

void core_probe_complete(core_state *cs, int ok, const u8 *reply, u32 len)
{
	u32 i;

	if (cs == 0 || cs->accessory_state != CORE_ACC_PROBING) {
		return;
	}
	if (!ok) {
		core_probe_abandon(cs);
		return;
	}

	if (reply != 0) {
		core_zero(cs->probe_reply, (u32)sizeof(cs->probe_reply));
		for (i = 0; i < len && i < CORE_PROBE_REPLY_BYTES; i++) {
			cs->probe_reply[i] = reply[i];
		}
	}

	if (cs->probe_step < CORE_PROBE_OUT_STEPS - 1) {
		cs->probe_step++;
		core_probe_issue(cs);
		return;
	}
	if (cs->probe_step == CORE_PROBE_OUT_STEPS - 1) {
		cs->probe_step = CORE_PROBE_STEP_READ1;
		core_probe_issue(cs);
		return;
	}

	if (cs->probe_step == CORE_PROBE_STEP_READ1) {
		/*
		 * A byte count other than 3 means nothing coherent answered, so
		 * the whole probe is dropped and retried later rather than
		 * reselecting the port.
		 */
		if (cs->probe_reply[0] != 0x03) {
			core_probe_abandon(cs);
			return;
		}
		if (core_probe_identified(cs)) {
			cs->accessory_state  = CORE_ACC_FOUND_1;
			cs->accessory_status = cs->probe_reply[1];
			cs->probe_step       = CORE_PROBE_STEP_DONE;
			return;
		}
		cs->probe_step = CORE_PROBE_STEP_SEL1;
		core_probe_issue(cs);
		return;
	}

	if (cs->probe_step == CORE_PROBE_STEP_SEL1) {
		cs->probe_step = CORE_PROBE_STEP_READ2;
		core_probe_issue(cs);
		return;
	}

	if (cs->probe_step == CORE_PROBE_STEP_READ2) {
		if (core_probe_identified(cs)) {
			cs->accessory_state  = CORE_ACC_FOUND_2;
			cs->accessory_status = cs->probe_reply[1];
			cs->probe_step       = CORE_PROBE_STEP_DONE;
			return;
		}
		/*
		 * Note the state is committed BEFORE the last transfer goes out,
		 * and that transfer has no completion routine in the original.
		 * The probe is over either way.
		 */
		cs->accessory_state = CORE_ACC_UNKNOWN;
		cs->probe_step      = CORE_PROBE_STEP_SEL2;
		if (cs->vendor != 0) {
			core_vendor_req req;
			core_zero(&req, (u32)sizeof(req));
			req.bmRequestType = (u8)CORE_VENDOR_OUT;
			req.bRequest      = (u8)CORE_VENDOR_RESELECT;
			req.wValue        = 0x00A6;
			cs->vendor(cs->vendor_ctx, &req);
		}
		cs->probe_step = CORE_PROBE_STEP_DONE;
		return;
	}

	cs->probe_step = CORE_PROBE_STEP_DONE;
}

/* ------------------------------------------------------------------ */
/* Controller Pak CRCs                                                 */
/* ------------------------------------------------------------------ */

/*
 * Both are the genuine N64 accessory-bus CRCs, not something the adapter
 * invented; the adapter is a transparent bridge onto the controller bus.
 * Specification and evidence: ../docs/usb-transport.txt section 3.3.
 *
 * Both are plain MSB-first CRCs with a zero initial value, no input or
 * output reflection and no final XOR. Computed a bit at a time rather
 * than from a table: a 256-entry table would buy nothing at the 1/64
 * second cadence these run at, and would put 256 bytes of constant into
 * a kernel image for it.
 */

/*
 * CRC5, polynomial 0x15 - that is x^5 + x^4 + x^2 + 1, the low five bits
 * of the divisor with the implicit x^5 dropped.
 *
 * The message is the 11-BIT BLOCK ADDRESS, which is the byte address
 * divided by the 32-byte block size. The low five bits of the byte
 * address are not part of the message and are discarded.
 */
u8 core_pak_addr_crc5(u16 address)
{
	u16 msg  = (u16)((address >> 5) & CORE_PAK_ADDR_MASK);
	u8  crc  = 0;
	int i;

	for (i = 0; i < CORE_PAK_ADDR_BITS; i++) {
		/* Line the top message bit up with the top CRC bit. */
		if (msg & (1u << (CORE_PAK_ADDR_BITS - 1))) {
			crc ^= 0x10;
		}
		crc <<= 1;
		if (crc & 0x20) {
			crc ^= 0x15;
		}
		crc &= 0x1f;
		msg = (u16)((msg << 1) & CORE_PAK_ADDR_MASK);
	}
	return crc;
}

/*
 * The 16-bit address word an accessory read or write actually carries:
 * the block address in the top 11 bits, its CRC5 in the low 5. This is
 * what goes on the wire, so it is the form the transport wants.
 */
u16 core_pak_addr_encode(u16 address)
{
	u16 block = (u16)((address >> 5) & CORE_PAK_ADDR_MASK);

	return (u16)((block << 5) | core_pak_addr_crc5(address));
}

/*
 * CRC8, polynomial 0x85 - that is x^8 + x^7 + x^2 + 1.
 *
 * The protocol always runs this over exactly one CORE_PAK_BLOCK_BYTES
 * block; len is a parameter so the function can be tested on shorter
 * inputs, not because the wire format ever varies.
 */
u8 core_pak_data_crc8(const u8 *data, u32 len)
{
	u8  crc = 0;
	u32 n;
	int i;

	if (data == 0) {
		return 0;
	}

	for (n = 0; n < len; n++) {
		u8 byte = data[n];

		for (i = 0; i < 8; i++) {
			if (byte & 0x80) {
				crc ^= 0x80;
			}
			/* Bit 7 leaving the register is what selects the XOR. */
			if (crc & 0x80) {
				crc = (u8)((crc << 1) ^ 0x85);
			} else {
				crc = (u8)(crc << 1);
			}
			byte = (u8)(byte << 1);
		}
	}
	return crc;
}
