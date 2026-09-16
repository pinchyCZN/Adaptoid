/*
 * core.h - OS-free logic for the Adaptoid replacement driver (wishk300).
 *
 * NOTHING in this file or in core.c may include a Windows or DDK header, call
 * a kernel API, or name a Windows type. Everything the logic needs from the
 * outside world arrives through the seams declared below.
 *
 * That rule is what lets the same source build into wishk300.sys and into the
 * wishk300.exe test harness. It is load-bearing, not stylistic: measurement
 * against the 2001 driver showed the script, report, effect and CRC code
 * reaches only 18 of that driver's 49 kernel imports, and all of those are
 * reducible to the two seams here - a report sink and a clock.
 *
 * Behavioural reference: ../docs/hid-descriptor.txt (report layouts),
 * ../docs/script-bytecode.txt (interpreter and report builders),
 * ../docs/driver-structures.txt (the state this mirrors).
 */
#ifndef ADAPTOID_CORE_H
#define ADAPTOID_CORE_H

/*
 * Fixed-width types, declared here rather than pulled from <stdint.h>, so the
 * header stays valid in kernel mode where the CRT headers are not available.
 */
typedef unsigned char   u8;
typedef unsigned short  u16;
typedef unsigned int    u32;
typedef signed char     s8;
typedef signed short    s16;
typedef signed int      s32;
#if defined(_MSC_VER)
typedef unsigned __int64 u64;
typedef signed   __int64 s64;
#else
typedef unsigned long long u64;
typedef signed   long long s64;
#endif

/*
 * The adapter reports controller state in five raw bytes. Layout confirmed in
 * drv_BuildJoystickReport; see ../docs/hid-descriptor.txt section 8.
 *
 *     +0  X, signed 8-bit      +1  Y, signed 8-bit     +2  status
 *     +3  button bits 8..15    +4  button bits 0..7
 *
 * Note the two button bytes are in that order: the LOW indices are in the
 * LAST byte.
 */
#define CORE_RAW_PACKET_BYTES   5
#define CORE_RAW_X              0
#define CORE_RAW_Y              1
#define CORE_RAW_STATUS         2
#define CORE_RAW_BUTTONS_HI     3
#define CORE_RAW_BUTTONS_LO     4

/*
 * Status byte. Bit 7 is a validity marker and bits 6..2 must be clear; only
 * the low two bits carry data. See ../docs/hid-descriptor.txt section 8.2.
 */
#define CORE_STATUS_MASK        0xFCu
#define CORE_STATUS_VALID       0x80u
#define CORE_STATUS_PAK_PRESENT 0x01u
#define CORE_STATUS_PAK_REMOVED 0x02u

/* One entry per bit of the raw button word; the value is the destination HID
 * button, zero based, or CORE_BUTTON_NONE. */
#define CORE_RAW_BUTTON_BITS    16
#define CORE_BUTTON_NONE        0xFFu

/*
 * Stick scaling. All three constants are the vendor's, not invented here:
 * the SDK exposes the last two as DirectInput escapes 0x7834BB10 and
 * 0x7834BB11. See ../docs/known-defects.txt section 3.
 */
#define CORE_STICK_SCALE        16    /* raw units to report units      */
#define CORE_STICK_CLIP_DEFAULT 75    /* range clipping, 0x4B           */
#define CORE_STICK_STRETCH_DEF  10    /* octagon-to-square corner warp  */
#define CORE_STICK_LIMIT        1200  /* 0x4B0, full scale on either axis */

/* Report IDs, fixed by the composite descriptor in
 * ../docs/hid-descriptor.txt section 4. Keeping these values identical to the
 * original is deliberate: existing profiles and muscle memory depend on
 * them. */
#define CORE_REPORT_JOYSTICK    1
#define CORE_REPORT_KEYBOARD    2
#define CORE_REPORT_MOUSE       3

#define CORE_REPORT_MAX_BYTES   16

/*
 * THE OUTPUT SEAM.
 *
 * Corresponds to drv_SubmitHidReport in the original. The driver's sink copies
 * into a pending HID read IRP; the harness's sink prints. Cutting here is what
 * removes the last four kernel imports (IoAllocateIrp, IoFreeIrp,
 * IofCallDriver, IofCompleteRequest) from the core's dependency closure.
 */
typedef void (*core_report_fn)(void *ctx, u8 report_id,
                              const u8 *data, u32 len);

/*
 * THE TRANSPORT SEAM.
 *
 * The accessory probe is a fixed sequence of USB vendor control transfers
 * plus a classification of what comes back. The sequencing and the decision
 * are logic and live here; the transfers are the OS's business. This is the
 * second seam, and it works the same way as the report sink: the core asks
 * for a transfer and is told later how it went.
 */
typedef struct core_vendor_req {
	u8  bmRequestType;   /* 0x40 host-to-device, 0xC0 device-to-host */
	u8  bRequest;
	u16 wValue;
	u16 wIndex;
	u16 wLength;         /* bytes expected back; 0 for an OUT */
} core_vendor_req;

/*
 * Perform one vendor transfer. Return nonzero if it was accepted; the owner
 * must then call core_probe_complete when it finishes. Returning zero means
 * the transport is busy, which abandons the probe for a later retry - the
 * same answer the original gives when its single vendor slot is occupied.
 */
typedef int (*core_vendor_fn)(void *ctx, const core_vendor_req *req);

/* The N64 request-info reply is four bytes; see ../docs/usb-transport.txt. */
#define CORE_PROBE_REPLY_BYTES  4

/*
 * Accessory probe state, the value the original keeps at devext+0x3E4 and
 * reports through IOCTL fn 0x836 selector 4. Report building is gated on it
 * being FOUND_1 or higher, so the probe gates all controller input.
 */
#define CORE_ACC_NEEDED         0   /* probe not yet run, or accessory pulled */
#define CORE_ACC_PROBING        1
#define CORE_ACC_FOUND_1        2   /* identified on the first read           */
#define CORE_ACC_FOUND_2        3   /* identified after a port reselect       */
#define CORE_ACC_UNKNOWN        4   /* probe finished, nothing identified     */

/*
 * THE FORCE-FEEDBACK EFFECT ENGINE.
 *
 * The motor is on or off, so a proportional effect is delivered as pulse
 * DENSITY rather than pulse width: a delta-sigma modulator turns an
 * intensity into a stream of one-bit samples, 32 of which are packed into
 * vendor command 0x36 and cover the next half second.
 *
 * Time base is 1/64 second. drv_EffectTick divides the interrupt time by
 * 156250 to get ticks and re-arms 32 ticks ahead, exactly matching the 32
 * bits it just sent.
 */
#define CORE_TICK_100NS         156250  /* 10^7 / 64 */
#define CORE_EFFECT_SLOTS       32
#define CORE_EFFECT_AXES        2
#define CORE_EFFECT_WINDOW      32      /* ticks per payload, 0x20 */
#define CORE_EFFECT_PAYLOAD     4       /* bytes of bitmap             */

/* Effect types, exactly the DirectInput set. */
#define CORE_FX_CONSTANT        0x10
#define CORE_FX_RAMP            0x20
#define CORE_FX_SQUARE          0x30
#define CORE_FX_SINE            0x31
#define CORE_FX_TRIANGLE        0x32
#define CORE_FX_SAWTOOTH_UP     0x33
#define CORE_FX_SAWTOOTH_DOWN   0x34
#define CORE_FX_TUNING          0x40

#define CORE_FX_INFINITE        (-1)    /* duration meaning no end     */
#define CORE_FX_CLAMP           0x7F    /* per-axis output limit       */
#define CORE_FX_SCALE           10000   /* waveform full scale         */

/*
 * The periodic reading of an axis parameter block: what every type except
 * CORE_FX_TUNING means by those eight bytes.
 */
typedef struct core_effect_periodic {
	s8  magnitude;
	s8  offset;
	s16 phase;              /* scaled against 36000 */
	s32 period;             /* in ticks             */
} core_effect_periodic;

/*
 * THE SAME EIGHT BYTES, read as a DirectInput CONDITION. This is what
 * CORE_FX_TUNING means by them, and the byte positions were read out of the
 * pre-pass at the top of drv_EffectEvaluate rather than assumed:
 *
 *     +0  centre            was magnitude
 *     +1  positive coeff    was offset
 *     +2  negative coeff    was the low byte of phase
 *     +3  positive sat      was the high byte of phase
 *     +4  negative sat      was byte 0 of period
 *     +5  dead band         was byte 1 of period
 *     +6  OUTPUT            was byte 2 of period - written by the pre-pass
 *                           and read back by the evaluator
 *     +7  unused
 *
 * A condition is a spring: output rises with how far the stick sits from
 * centre, once it leaves the dead band, with independent coefficients and
 * saturations on each side.
 */
typedef struct core_effect_condition {
	s8 center;
	s8 positive_coeff;
	s8 negative_coeff;
	s8 positive_sat;
	s8 negative_sat;
	s8 dead_band;
	s8 output;
	s8 unused;
} core_effect_condition;

/*
 * One axis parameter block, both readings over the same storage. The layout
 * has to stay compatible because IOCTL fn 0x852 writes these eight bytes raw
 * and the type alone decides how they are read.
 */
typedef union core_effect_axis {
	core_effect_periodic  periodic;
	core_effect_condition condition;
} core_effect_axis;

/* One effect slot; the original has 32, written by IOCTL fn 0x852. */
typedef struct core_effect_slot {
	u32              type;
	int              running;
	s32              start_tick;
	s32              duration;      /* CORE_FX_INFINITE for no end */
	s32              attack_level;
	s32              attack_time;
	s32              fade_level;
	s32              fade_time;
	core_effect_axis axis[CORE_EFFECT_AXES];
} core_effect_slot;

/*
 * THE EFFECT RING.
 *
 * 96 precomputed ticks, so the engine can run ahead of real time. It is not
 * a buffer of pending output - it is how the driver AVOIDS SENDING. On each
 * timer tick the engine re-evaluates the ticks it already computed and
 * compares them against what it stored; if every pulse still agrees, the
 * bitmap already on the controller is still correct and no USB transfer is
 * needed at all.
 *
 * 96 entries against a 32-tick window gives three windows of slack.
 */
#define CORE_RING_SIZE          96

typedef struct core_effect_tick {
	s32 pulse;              /* the bit that ships          */
	s32 intensity;
	s32 filtered_x;
	s32 filtered_y;
	s32 accumulator;
	s32 dither_burst;
	s32 tune_counter;
} core_effect_tick;

/*
 * Core state. Mirrors the parts of the 0x1800-byte device extension that hold
 * no Windows types; see ../docs/driver-structures.txt sections 2.5 to 2.8.
 * Deliberately a plain struct with no pointers to OS objects.
 */
typedef struct core_state {
	core_report_fn  sink;
	void           *sink_ctx;

	/* Clock, in 100ns units, as handed to core_tick. Replaces
	 * KeQueryInterruptTime. Advanced by the caller, never read from the OS. */
	u64             now_100ns;

	/* Most recent raw controller packet. */
	u8              raw[CORE_RAW_PACKET_BYTES];
	int             have_raw;

	/* Decoded from that packet. Stick axes are in report units, +/-1200. */
	s16             stick_x;
	s16             stick_y;
	u16             buttons;
	u8              status;
	u8              prev_status;

	/*
	 * Tunables. Both are exposed by the vendor SDK and a replacement is
	 * meant to keep them adjustable: stretch 0 disables the corner warp,
	 * clip 128 restores the raw range.
	 */
	u8              stick_clip;
	u8              stick_stretch;

	/* Raw button bit -> HID button, zero based, or CORE_BUTTON_NONE. */
	u8              button_map[CORE_RAW_BUTTON_BITS];

	/* The effect engine. */
	core_effect_slot effect[CORE_EFFECT_SLOTS];
	s32             filtered_x;     /* 5/6 decay low-pass on each axis   */
	s32             filtered_y;
	s32             accumulator;    /* delta-sigma, emits a pulse past 100 */
	s32             dither_burst;
	s32             tune_counter;
	u32             dither_state;
	s32             effect_idle_ticks;

	/*
	 * The ring. The five carry fields above are the LIVE values while a
	 * window is being evaluated; they are seeded from the entry before the
	 * window starts and written back into each entry as it is computed.
	 */
	core_effect_tick ring[CORE_RING_SIZE];
	s32             ring_head;      /* index of the oldest valid entry     */
	s32             ring_count;     /* 0..CORE_RING_SIZE                   */
	s32             ring_base_tick; /* the tick ring_head stands for       */

	/*
	 * Motor drive calibration. These are NOT tuning-mode-only values: the
	 * original seeds them in drv_AddDevice and the on-controller tuning
	 * mode is a live editor for them. With all four at zero the engine
	 * produces no pulses at all, because intensity is multiplied by
	 * duty * period.
	 */
	s32             tune_period;
	s32             tune_duty;
	s32             tune_duty_complement;
	s32             tune_strength;

	/* The accessory probe. */
	core_vendor_fn  vendor;
	void           *vendor_ctx;
	u8              accessory_state;
	u8              accessory_status;   /* the N64 status byte the probe read */
	u8              probe_step;
	u8              probe_reply[CORE_PROBE_REPLY_BYTES];

	/* Which virtual devices are live. Mirrors drv_VirtualDevicesMask: bit 0
	 * joystick, bit 1 keyboard, bit 2 mouse. The 2001 driver defaults this to
	 * 7 in DriverEntry and lets a registry value override it. */
	u32             devices_mask;

	u32             reports_emitted;
} core_state;

#define CORE_DEVICE_JOYSTICK    0x1u
#define CORE_DEVICE_KEYBOARD    0x2u
#define CORE_DEVICE_MOUSE       0x4u
#define CORE_DEVICE_DEFAULT     0x7u

/* Lifecycle. */
void core_init(core_state *cs, core_report_fn sink, void *sink_ctx);
void core_reset(core_state *cs);

/* Input: one raw packet off the interrupt pipe. Decodes it, packs report 1
 * and pushes it through the sink. */
void core_on_raw_packet(core_state *cs, const u8 *raw);

/*
 * The two halves of that, exposed because they are pure and worth testing
 * on their own.
 *
 * core_decode returns 0 and changes nothing if the status byte fails the
 * validity test; otherwise it fills stick_x, stick_y and buttons.
 * core_pack_joystick writes the 5-byte payload of report 1 - the report ID
 * is not part of it, the sink carries that separately.
 */
int  core_decode(core_state *cs, const u8 *raw);
void core_pack_joystick(const core_state *cs, u8 *out);

/*
 * The accessory probe.
 *
 * core_set_vendor installs the transport. core_probe_start begins the
 * sequence and returns nonzero if the first transfer was accepted;
 * core_on_raw_packet starts it by itself when the accessory state says it is
 * needed, which is how the original does it.
 *
 * core_probe_complete is the completion callback: ok is zero if the transfer
 * failed, and reply/len carry whatever came back from a device-to-host
 * transfer. It is safe to call with the probe idle, and it never calls the
 * transport re-entrantly from within core_vendor_fn - the owner drives.
 */
/*
 * The effect engine.
 *
 * core_sine_lerp is an interpolated quarter-wave sine: the angle is in
 * hundredths of a degree, 0..9000, and the result is 10000 * sin.
 *
 * core_effect_axis_value evaluates one axis of one slot at an elapsed time,
 * waveform and envelope included, clamped to +/-CORE_FX_CLAMP.
 *
 * core_effect_intensity sums every running slot at one tick and returns the
 * modulator input. core_effect_window runs CORE_EFFECT_WINDOW ticks and packs
 * the resulting pulses into the four payload bytes of vendor command 0x36.
 */
s32  core_sine_lerp(s32 angle_hundredths);
s32  core_effect_axis_value(const core_state *cs,
                            const core_effect_slot *slot,
                            int axis, s32 elapsed);
/*
 * The condition pre-pass. Samples the LIVE stick position into the output
 * byte of every running CORE_FX_TUNING axis, once per window rather than per
 * tick, so the condition is frozen for the half second that window covers.
 * core_effect_window calls it; it is exposed for testing.
 */
void core_effect_condition_update(core_state *cs);

s32  core_effect_intensity(core_state *cs, s32 tick);
int  core_effect_pulse(core_state *cs, s32 intensity, s32 tick);

/*
 * THE REAL ENTRY POINT. Evaluates a window against the ring and says whether
 * anything needs to be sent:
 *
 *     0   payload is filled and should go out as vendor command 0x36
 *     1   the ring still agrees with what the controller already has, so
 *         there is nothing to send and the payload is untouched
 *
 * lookahead selects the mode, matching the original:
 *
 *     non-zero, ring populated   VERIFY  re-evaluate what is already there,
 *                                        stopping at the first disagreement
 *     non-zero, ring empty       COMPUTE a fresh 32-tick window
 *     zero                       EXTEND  fill from the end of the ring out
 *                                        to 32, and always send
 */
int  core_effect_evaluate(core_state *cs, int lookahead, s32 tick,
                          u8 *payload);

/* Compute one fresh window, discarding the ring. Convenience for tests. */
void core_effect_window(core_state *cs, s32 start_tick, u8 *payload);

/* Drop every precomputed tick, as the idle motor-stop path does. */
void core_effect_ring_reset(core_state *cs);

void core_set_vendor(core_state *cs, core_vendor_fn fn, void *ctx);
int  core_probe_start(core_state *cs);
void core_probe_complete(core_state *cs, int ok, const u8 *reply, u32 len);

/* Time: advance the engine. now_100ns is monotonic, in 100ns units. */
void core_tick(core_state *cs, u64 now_100ns);

/*
 * Controller Pak CRCs. Pure functions, no state.
 *
 * The genuine N64 accessory-bus algorithms; see ../docs/usb-transport.txt
 * section 3.3 for the specification and how it was established.
 */

/* An accessory read or write moves one 32-byte block. */
#define CORE_PAK_BLOCK_BYTES    32

/* The block address is 11 bits: the byte address divided by the block size. */
#define CORE_PAK_ADDR_BITS      11
#define CORE_PAK_ADDR_MASK      0x7ffu

/* CRC5 of the block address, polynomial 0x15. Takes a BYTE address; the
 * low five bits are not part of the message and are discarded. */
u8 core_pak_addr_crc5(u16 address);

/* The 16-bit address word that goes on the wire: block address in the top
 * 11 bits, its CRC5 in the low 5. */
u16 core_pak_addr_encode(u16 address);

/* CRC8 of a data block, polynomial 0x85. The protocol always passes
 * CORE_PAK_BLOCK_BYTES; len exists so the function can be tested. */
u8 core_pak_data_crc8(const u8 *data, u32 len);

#endif /* ADAPTOID_CORE_H */
