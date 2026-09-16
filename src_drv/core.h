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
/* The display name, ten bytes at devext+0x334 including the NUL. */
#define CORE_DEVICE_NAME_BYTES  10

#define CORE_STICK_CLIP_DEFAULT 75    /* range clipping, 0x4B           */
#define CORE_STICK_STRETCH_DEF  10    /* octagon-to-square corner warp  */
#define CORE_STICK_LIMIT        1200  /* 0x4B0, full scale on either axis */

/*
 * ON-CONTROLLER TUNING MODE.
 *
 * A live adjustment mode entered from the controller itself, with no user
 * mode involved at all: hold L + R + Z + Start and the stick and D-pad stop
 * being input and start being calibration controls for the motor.
 *
 * Masks are over the two raw button bytes. Remember the ordering: byte +4
 * carries bit indices 0..7 and byte +3 carries 8..15, MSB first within each.
 */
#define CORE_BTN_HI_LR          0x30u   /* L and R, indices 10 and 11   */
#define CORE_BTN_HI_SHOULDER_C  0x3Fu   /* L, R and the four C buttons  */
#define CORE_BTN_HI_RESET       0x80u   /* index 8                      */
#define CORE_BTN_LO_Z           0x20u   /* index 2                      */
#define CORE_BTN_LO_START       0x10u   /* index 3                      */
#define CORE_BTN_LO_NO_START    0xEFu   /* everything except Start      */
#define CORE_BTN_LO_FACE        0xF0u   /* A, B, Z, Start               */
#define CORE_BTN_LO_DPAD        0x0Fu   /* the four D-pad directions    */

/* TuneMode is a two-bit state: bit 0 active, bit 1 the combination held. */
#define CORE_TUNE_ACTIVE        0x1
#define CORE_TUNE_HELD          0x2

/* Report IDs, fixed by the composite descriptor in
 * ../docs/hid-descriptor.txt section 4. Keeping these values identical to the
 * original is deliberate: existing profiles and muscle memory depend on
 * them. */
#define CORE_REPORT_JOYSTICK    1
#define CORE_REPORT_KEYBOARD    2
#define CORE_REPORT_MOUSE       3

#define CORE_REPORT_MAX_BYTES   16

/* ======================================================================
 * THE REPORT DESCRIPTORS
 *
 * Three top-level collections in one descriptor - that is what makes this a
 * COMPOSITE HID device, and it is the property the whole replacement exists
 * to preserve: Windows creates a keyboard, a mouse and a game controller
 * from one USB endpoint, so a remapped button is real key input to every
 * application, not an injected event some of them ignore.
 *
 * ONE ARRAY, THREE VIEWS. The 185-byte composite is EXACTLY the 121-byte
 * Mouse+Keyboard descriptor followed by the 64-byte Joystick one, verified
 * byte for byte against the original, so the two fragments are slices of
 * the whole rather than separate copies. The original keeps three copies at
 * 00019b40, 00019c00 and 00019c40.
 * ====================================================================== */

/* Three top-level application collections: mouse, keyboard, joystick. */
#define CORE_HID_COLLECTIONS    3

#define CORE_HID_DESC_ALL       185     /* mouse + keyboard + joystick   */
#define CORE_HID_DESC_MK        121     /* mouse + keyboard, at offset 0 */
#define CORE_HID_DESC_JOY       64      /* joystick only                 */
#define CORE_HID_DESC_JOY_AT    121     /* where the joystick part opens */

/*
 * The descriptor selected by devices_mask, and its length.
 *
 * The mask's low three bits index an eight-entry table of which only two
 * entries differ from the first: 0 through 5 all give the joystick, 6 gives
 * mouse + keyboard, 7 gives all three. That is the original's table, not a
 * simplification of it.
 */
const u8 *core_hid_descriptor(u32 devices_mask, u32 *length);

/*
 * THE KEYBOARD AND MOUSE STATE MACHINES.
 *
 * A script's _key and _mouse_* builtins do not build reports; they post
 * events, and drv_DispatchEvents turns each one into a call below. These
 * three functions own the state those reports are built from, which is why
 * the state lives here rather than in the event queue.
 */

/* Keycode ranges drv_HidKeyEvent accepts. Anything else is ignored. */
#define CORE_HID_MOD_FIRST      0xE0u   /* 0xE0..0xE7 are the modifiers   */
#define CORE_HID_MOD_LAST       0xE7u
#define CORE_HID_KEY_FIRST      0x04u   /* 0x04..0xA4 are ordinary keys   */
#define CORE_HID_KEY_LAST       0xA4u

/*
 * The pressed-key array is sized to the ACCEPTED KEYCODE RANGE, not to the
 * ten slots the report carries: 0xA5 entries for 0xA1 possible keycodes.
 * A key already in the array is never appended twice, so the count cannot
 * exceed the number of distinct accepted codes and the array cannot
 * overflow. The original's devext reserves exactly these 0xA5 bytes, which
 * is how we know the bound was deliberate.
 */
#define CORE_HID_KEYS_MAX       0xA5

/* How many fit in one report, and what is sent when more are held. */
#define CORE_HID_KEYS_REPORTED  10
#define CORE_HID_ROLLOVER       0x01u   /* HID ErrorRollOver */

/* Mouse buttons 1..3, as bits 0..2. */
#define CORE_HID_MOUSE_BUTTONS  3

/* The joystick report payload, after the report ID. */
#define CORE_JOY_REPORT_BYTES   5

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

/*
 * WHICH OF THE CORE'S CHAINS OWNS THE TRANSFER IN FLIGHT.
 *
 * The core issues transfers from two places - the accessory probe and the
 * effect engine - and the completion has to go back to the right one. The
 * original routes by the callback pointer it stored with the request; this
 * records the same thing as a value, which is testable and needs no
 * function pointer to survive a suspend.
 */
#define CORE_VENDOR_OWNER_NONE   0
#define CORE_VENDOR_OWNER_PROBE  1
#define CORE_VENDOR_OWNER_EFFECT 2
/* Fire and forget: nothing is waiting for it. */
#define CORE_VENDOR_OWNER_LOOSE  3


/*
 * THE SCRIPT INPUT HOOK.
 *
 * Called from core_on_raw_packet after the packet is decoded and before the
 * report is packed, which is the exact point drv_BuildJoystickReport hands
 * the packet to drv_ScriptDispatchInput. A script's _stick and _button
 * builtins write the decoded state directly, so running here is what lets
 * a script take the stick over.
 *
 * A seam rather than a direct call because sched.c depends on core.c and
 * not the other way round.
 */
typedef void (*core_input_hook_fn)(void *ctx, const u8 *raw, u64 now_100ns);


/*
 * THE TICK HOOK, the same arrangement for the clock: core_tick drives the
 * effect engine itself and calls this for the script scheduler.
 */
typedef void (*core_tick_hook_fn)(void *ctx, u64 now_100ns);



/*
 * THE VENDOR SLOT.
 *
 * There is exactly one control transfer in flight at a time, and the
 * original arbitrates it with a claim: take the slot if it is free,
 * otherwise set a flag so the work is picked up when the slot frees.
 *
 * This callback is that claim. It returns non-zero if the slot was taken.
 * Installing one is optional - with none, the slot is treated as always
 * free, which is what the probe assumes.
 *
 * It matters because a step that cannot send must not evaluate the ring
 * either: doing so would advance the window without transmitting it, and
 * the next verify pass would then compare against a window the controller
 * never received.
 */
typedef int (*core_vendor_claim_fn)(void *ctx);

/*
 * THE SYNCHRONOUS TRANSPORT SEAM.
 *
 * core_vendor_fn above is fire-and-forget: the probe asks for a transfer and
 * is told later how it went. The raw N64 transaction is the other shape -
 * drv_N64Transaction blocks on KeWaitForSingleObject after every transfer and
 * therefore runs at PASSIVE_LEVEL only. Modelling that as the asynchronous
 * seam would turn one readable function into a five-state machine for no
 * gain, so it gets a seam of its own.
 *
 * data is the data stage: filled for an IN, sent for an OUT, and ignored when
 * len is zero. Return non-zero if the transfer completed.
 */
typedef int (*core_vendor_sync_fn)(void *ctx, const core_vendor_req *req,
                                   u8 *data, u32 len);

/*
 * THE RECEIVE BOUND. The original's reply buffer is 64 bytes of stack, and
 * every transfer asks the device for rx_len + 1 bytes into it - one length or
 * status byte, then the payload. 63 is therefore the largest reply that fits,
 * and the original does not check. See core_n64_transaction.
 */
#define CORE_N64_RX_MAX         63

/* N64 controller bus commands, as the shipped IOCTLs issue them. */
#define CORE_N64_CMD_INFO       0x00u   /* request info, 3 bytes back      */
#define CORE_N64_CMD_READ       0x02u   /* read accessory, 33 bytes back   */
#define CORE_N64_CMD_WRITE      0x03u   /* write accessory, 1 byte back    */

/* Vendor requests the transaction uses. */
#define CORE_VENDOR_N64_BASE    0x20u   /* short form is base + tx_len     */
#define CORE_VENDOR_N64_FETCH   0x71u   /* collect a long-form reply       */
#define CORE_VENDOR_RESET       0x72u   /* also the probe's register write */

/* Transfer direction, the two values bmRequestType ever takes here. */
#define CORE_VENDOR_OUT         0x40u   /* host to device */
#define CORE_VENDOR_IN          0xC0u   /* device to host */

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
	/* Bytes of axis parameter block the programming IOCTL supplied. Zero
	 * means the slot is programmed but not startable. */
	s32              block_length;
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

/*
 * THE SEND CHAIN. The engine is not driven by a loop; each USB transfer's
 * completion kicks the next step, so the motor stays fed for as long as
 * something is playing:
 *
 *     core_effect_tick        timer entry. Evaluates against the ring and,
 *                             if the bitmap changed, sends 0x36.
 *       -> completion         core_effect_keepalive
 *     core_effect_keepalive   poke a vendor register at most once every
 *                             three seconds, then
 *       -> completion         core_effect_send_periodic
 *     core_effect_send_periodic  extend the ring by one window and send
 *                             0x35. NO completion - the chain ends here and
 *                             the next timer tick restarts it.
 *
 * Both 0x35 and 0x36 carry the same thing: the 32-bit pulse bitmap, packed
 * into wValue and wIndex rather than a data stage.
 */
#define CORE_FX_CMD_STOP        0x32    /* motor off, idle path       */

/*
 * The idle command's wValue and wIndex, from drv_SendIdleCommand (00018ca0).
 * Note they are NOT the same as the enable kick's 0x0002 / 0xFE80 that
 * drv_SetDeviceEnable sends on the same request code - the low byte of
 * wValue distinguishes them, and 0xFF against 0x80 in wIndex does too.
 */
#define CORE_FX_IDLE_VALUE      0x0200
#define CORE_FX_IDLE_INDEX      0xFEFF
#define CORE_FX_CMD_PERIODIC    0x35    /* continuation window        */
#define CORE_FX_CMD_TICK        0x36    /* re-armed window            */
#define CORE_FX_CMD_KEEPALIVE   0x72    /* the register poke          */
#define CORE_FX_CMD_PAK_INSERT  0x34    /* an accessory arrived       */
#define CORE_FX_CMD_PAK_REMOVE  0x35    /* an accessory left          */

#define CORE_FX_KEEPALIVE_VALUE 0xFF22
#define CORE_FX_KEEPALIVE_INDEX 0x0094

/* Three seconds, in 100ns units: how stale a keep-alive may get. */
#define CORE_FX_KEEPALIVE_100NS 30000000

/* EffectState in the original: which kind of send was last issued. */
#define CORE_FX_STATE_IDLE      0
#define CORE_FX_STATE_TICK      1
#define CORE_FX_STATE_PERIODIC  2
#define CORE_FX_STATE_PAK       3   /* a pak change has been seen      */

/* Which step owns the completion of the transfer now in flight. */
#define CORE_FX_NEXT_NONE       0
#define CORE_FX_NEXT_KEEPALIVE  1
#define CORE_FX_NEXT_PERIODIC   2
/*
 * The pak insert/remove update's completion, which puts the engine back
 * into its ticking state. Without it the state machine stalls in
 * CORE_FX_STATE_PAK and the motor never runs again after an accessory
 * change - drv_EffectSendUpdate passes drv_EffectUpdateComplete for
 * exactly this reason.
 */
#define CORE_FX_NEXT_UPDATE     3

typedef struct core_effect_ring_entry {
	s32 pulse;              /* the bit that ships          */
	s32 intensity;
	s32 filtered_x;
	s32 filtered_y;
	s32 accumulator;
	s32 dither_burst;
	s32 tune_counter;
} core_effect_ring_entry;

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
	/*
	 * s32, not u8: IOCTL functions 0x83e and 0x83f get and set these as
	 * DWORDs and the original stores them as int at devext+0x3ec/0x3f0.
	 * Narrowing them here would silently truncate a value the
	 * configurator is entitled to write.
	 */
	s32             stick_clip;
	s32             stick_stretch;

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
	core_effect_ring_entry ring[CORE_RING_SIZE];
	s32             ring_head;      /* index of the oldest valid entry     */
	s32             ring_count;     /* 0..CORE_RING_SIZE                   */
	s32             ring_base_tick; /* the tick ring_head stands for       */

	/* The send chain. */
	s32             effect_state;   /* CORE_FX_STATE_*                     */
	s32             next_tick;      /* first tick of the next window       */
	u64             keepalive_time; /* raw 100ns of the last register poke */
	u8              effect_next;    /* CORE_FX_NEXT_*, owns the completion */

	/*
	 * Deferred work. Each flag means "this could not run because the
	 * vendor slot was busy"; core_effect_run_deferred drains them in the
	 * original's fixed priority order.
	 */
	/*
	 * THE IDLE COMMAND IS THE HIGHEST PRIORITY of the four, per
	 * drv_NextDeferredWork (00019620). It is the "motor off" that
	 * core_set_enable could not send because the slot was busy, and
	 * losing it would leave a motor running - which is why it is
	 * deferred at all when the enable-on case is simply refused.
	 */
	int             claim_idle_command;
	int             claim_effect_tick;
	int             claim_pak_insert;
	int             claim_pak_remove;
	core_vendor_claim_fn vendor_claim;

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

	/* CORE_TUNE_*; non-zero means the stick is calibrating, not playing. */
	s32             tune_mode;
	u8              prev_raw_x;
	u8              prev_raw_y;

	/*
	 * Read out by the private IOCTL surface. report_pending is set when a
	 * controller packet arrives and cleared by the snapshot call;
	 * device_name is the display string, ten bytes including its NUL in
	 * the original; bcd_device is the firmware revision from the USB
	 * device descriptor.
	 */
	int             report_pending;
	char            device_name[CORE_DEVICE_NAME_BYTES];
	u16             bcd_device;
	u32             counter_two;

	/* The accessory probe. */
	core_vendor_fn  vendor;
	void           *vendor_ctx;

	/* The blocking transport, used only by core_n64_transaction. */
	core_vendor_sync_fn vendor_sync;
	u8              accessory_state;
	u8              accessory_status;   /* the N64 status byte the probe read */
	u8              probe_step;
	u8              probe_reply[CORE_PROBE_REPLY_BYTES];

	/*
	 * Keyboard state. keys_down is dense and ordered by press time: a
	 * press appends, a release closes the gap. See core_hid_key_event.
	 */
	u8              key_modifiers;
	u8              keys_down[CORE_HID_KEYS_MAX];
	s32             key_down_count;

	/* Mouse state. The totals are accumulated and, in the original, never
	 * read back by anything; they are kept for parity. */
	u8              mouse_buttons;
	s32             mouse_total_x;
	s32             mouse_total_y;
	s32             mouse_total_wheel;

	/*
	 * Virtual joystick mode. When on, joystick reports are synthesised
	 * instead of coming from the controller, and real reports are dropped.
	 * reports_enabled selects it; in the original that is a driver-wide
	 * global written only by the control device's IOCTL surface, so the OS
	 * layer mirrors it into each device.
	 */
	s32             virtual_mode;

	/*
	 * THE DEVICE INSTANCE NUMBER, and the synthetic stick position, which
	 * are the same field. drv_AddDevice assigns it as
	 *
	 *     (InterlockedIncrement(counter) % 1100) + 50
	 *
	 * so it is a per-adapter number in 50..1149 - deliberately inside the
	 * +/-1200 stick range, because virtual mode reports it as the Y axis.
	 * Each adapter therefore parks its stick at its own identity, and
	 * control-device function 0x822 resolves a handle from the same value.
	 * An earlier reading called this VirtualStickValue and treated it as a
	 * calibration constant; it is an identifier that doubles as one.
	 */
	s32             instance_id;
	int             reports_enabled;
	u8              last_report[CORE_JOY_REPORT_BYTES];

	/* Which virtual devices are live. Mirrors drv_VirtualDevicesMask: bit 0
	 * joystick, bit 1 keyboard, bit 2 mouse. The 2001 driver defaults this to
	 * 7 in DriverEntry and lets a registry value override it. */
	u32             devices_mask;

	/*
	 * WHICH TOP-LEVEL COLLECTIONS hidclass has opened. WRITE-ONLY: the
	 * original sets these from IOCTL_HID_ACTIVATE_DEVICE and
	 * IOCTL_HID_DEACTIVATE_DEVICE and never reads them anywhere - all
	 * four references to the field are in that one dispatcher. hidclass
	 * tracks collection state itself; the driver only acknowledges.
	 */
	u8              collection_enabled[CORE_HID_COLLECTIONS];

	u32             reports_emitted;

	/*
	 * THE EMULATED CONTROLLER PAK, which belongs to the SDK command-block
	 * channel and to nothing else.
	 *
	 * Two joybus addresses are answered by the driver itself instead of
	 * being put on the wire: 0x8000, where a Rumble Pak identifies itself,
	 * and 0xC000, the motor register. emu_pak_value is the last byte
	 * written to either, emu_pak_present is whether the last identify saw
	 * an accessory at all. See core_cmd_exec.
	 *
	 * They are here rather than beside the effect engine because they are
	 * genuinely separate state: in the original they are devext+0x42C and
	 * +0x430, and between them they have seven references, every one of
	 * them in drv_ProcessCommandBlock, drv_ExecuteRawCommand or the reset
	 * that zeroes them. The real motor is driven by the effect engine and
	 * never looks at either.
	 */
	/*
	 * Which chain owns the transfer in flight; see
	 * CORE_VENDOR_OWNER_*. Set at every issue site.
	 */
	u8              vendor_owner;

	/*
	 * NON-ZERO WHILE A SCRIPT OWNS THE STICK. The original gates this on
	 * ScriptState >= 0, a counter that starts latched at -1 - so a script
	 * cannot take the stick until something lifts it. When set, the values
	 * a script's _stick left behind survive into the report instead of
	 * being overwritten by the next packet's decode.
	 */
	int             script_owns_stick;

	core_input_hook_fn input_hook;
	void              *input_ctx;
	core_tick_hook_fn  tick_hook;
	void              *tick_ctx;

	u32             emu_pak_value;
	int             emu_pak_present;
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
/*
 * Compute one window of the effect ring. EXPOSED FOR TESTING ONLY - the
 * engine reaches it through core_effect_evaluate, and nothing in the driver
 * calls it directly. Worth having reachable: it is the densest arithmetic
 * in the port and the easiest place for a transcription slip to hide.
 */
void core_effect_window(core_state *cs, s32 start_tick, u8 *payload);

/* Drop every precomputed tick, as the idle motor-stop path does. */
void core_effect_ring_reset(core_state *cs);

/*
 * THE SEND CHAIN.
 *
 * core_effect_tick is the timer entry point; now_100ns is raw interrupt time,
 * which it divides by CORE_TICK_100NS to get the 1/64-second tick.
 *
 * core_effect_complete is called when the transfer the core last issued has
 * finished, and runs whichever step owns that completion. It returns zero
 * once the chain has run out, which is the normal end of a round.
 *
 * Both return non-zero if they issued a transfer.
 */
int core_effect_tick(core_state *cs, u64 now_100ns);
int core_effect_complete(core_state *cs, u64 now_100ns);

/* Install the vendor-slot claim; see core_vendor_claim_fn. Optional. */
void core_set_vendor_claim(core_state *cs, core_vendor_claim_fn claim);

/*
 * PAK INSERT AND REMOVE.
 *
 * core_on_raw_packet calls core_effect_on_pak_change by itself when the
 * status byte says the accessory came or went, so a driver does not have to.
 * present is non-zero when a Pak is now in the port.
 *
 * The hook sends an effect update carrying sub-command 0x34 for an insert
 * and 0x35 for a remove - or the motor-stop 0x32 instead, if the engine has
 * been idle long enough. If the vendor slot is busy the work is deferred.
 */
void core_effect_on_pak_change(core_state *cs, int present);

/*
 * One poll of the tuning mode. Handles entry, the live adjustment and exit,
 * and returns non-zero when the effect engine should be kicked - which
 * happens on leaving the mode and whenever an adjustment actually changed a
 * value. core_on_raw_packet calls it, so a driver does not have to.
 *
 * While the mode is active the stick and D-pad are NOT input: X sets the
 * motor period on a quadratic curve, Y sets the duty cycle, and the D-pad
 * picks one of five strength presets.
 */
int core_tune_update(core_state *cs, u8 buttons_hi, u8 buttons_lo,
                     s32 raw_x, s32 raw_y);

/*
 * core_effect_run claims the slot and runs one timer tick; core_effect_kick
 * clears the idle count first, which is what a change to the effect set
 * does. core_effect_run_deferred drains whatever could not run earlier and
 * releases the slot when nothing is left.
 *
 * core_effect_update_complete is the completion of the motor-stop transfer.
 */
int  core_effect_run(core_state *cs, u64 now_100ns);
int  core_effect_kick(core_state *cs, u64 now_100ns);
int  core_effect_run_deferred(core_state *cs, u64 now_100ns);

/*
 * Send the idle command - "motor off", vendor request 0x32 with wValue
 * 0x0200 and wIndex 0xFEFF - and clear the claim. Exposed because the OS
 * layer sends it directly when the slot was free, and the deferred drain
 * sends it when it was not; one function, two callers, one wire format.
 */
int  core_effect_send_idle(core_state *cs);
void core_effect_update_complete(core_state *cs);

/*
 * One transfer the core issued has finished. Routes to whichever of the
 * core's chains owns it and returns non-zero if that chain issued another.
 *
 * THE OS LAYER MUST CALL THIS, and calling it is what makes the accessory
 * probe and the rumble engine run at all - without it both stall after
 * their first transfer.
 */
int core_vendor_completed(core_state *cs, int ok, const u8 *reply, u32 len,
                          u64 now_100ns);

/* Install the script input hook and the tick hook; see their typedefs. */
void core_set_input_hook(core_state *cs, core_input_hook_fn fn, void *ctx);
void core_set_tick_hook(core_state *cs, core_tick_hook_fn fn, void *ctx);

void core_set_vendor(core_state *cs, core_vendor_fn fn, void *ctx);

/* Install the blocking transport; see core_vendor_sync_fn. */
void core_set_vendor_sync(core_state *cs, core_vendor_sync_fn fn);

/*
 * One raw N64 controller-bus transaction. See core.c for the two encodings
 * and for what is bounded here that the original left unbounded.
 */
int  core_n64_transaction(core_state *cs, const u8 *tx, s32 tx_len,
                          u8 *rx, s32 rx_len, s32 *actual);

/*
 * The keyboard and mouse report state machines. A script's _key and
 * _mouse_* builtins reach these through the event queue.
 */
void core_hid_key_event(core_state *cs, u32 usage, int down);
void core_hid_mouse_button(core_state *cs, u32 button, int down);
void core_hid_mouse_move(core_state *cs, s32 dx, s32 dy, s32 wheel);

/* Submit joystick report 1, or NULL to replay the last one. */
void core_submit_joystick(core_state *cs, const u8 *report);

/* Ask the controller to reset. Fire and forget. */
int  core_controller_reset(core_state *cs);
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
