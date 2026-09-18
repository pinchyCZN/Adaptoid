/*
 * ioctl.h - the private IOCTL surface for wishk300.
 *
 * This is the whole contract with the configurator. wishd201.exe drives the
 * driver through eighteen function codes on one device-control entry point,
 * specified in ../docs/ioctl-surface.txt.
 *
 * The dispatcher is deliberately OS-FREE, which the original's is not. Every
 * one of its eighteen cases is a length check followed by a small action on
 * device state, and none of that needs an IRP - so the IRP is decoded into a
 * core_ioctl by the caller, and what comes back is a status and a byte count
 * to put in IoStatus. The two cases that genuinely need the OS, the raw
 * vendor passthrough and the enable toggle, are seams.
 *
 * Holding that line is what lets the entire surface be tested in the harness,
 * including the two bounds failures the original has here.
 */
#ifndef ADAPTOID_IOCTL_H
#define ADAPTOID_IOCTL_H

#include "sched.h"

/*
 * Control codes are CTL_CODE(0xB98C, fn, METHOD_BUFFERED, FILE_ANY_ACCESS),
 * so code = 0xB98C0000 | (fn << 2) with fn in the vendor range 0x800+.
 */
#define CORE_IOCTL_DEVICE_TYPE  0xB98C0000u
#define CORE_IOCTL_CODE(fn)     (CORE_IOCTL_DEVICE_TYPE | ((u32)(fn) << 2))
#define CORE_IOCTL_FN(code)     (((code) >> 2) & 0xFFFu)

/* The eighteen function codes the per-device dispatcher handles. */
#define CORE_IOC_RAW_VENDOR     0x832u  /* raw vendor control transfer     */
#define CORE_IOC_STATUS_SNAP    0x833u  /* last raw report, clears pending */
#define CORE_IOC_ACCEPT_NOP     0x834u  /* accepted, no observable effect  */
#define CORE_IOC_DEVICE_NAME    0x835u  /* NUL-terminated display name     */
#define CORE_IOC_READ_COUNTER   0x836u  /* one of four counters            */
#define CORE_IOC_ZERO_COUNTER   0x837u  /* zero counter 2 or 3             */
#define CORE_IOC_SET_ENABLE     0x839u  /* device on/off toggle            */
#define CORE_IOC_N64_PASSTHRU   0x83Au  /* raw N64 bus transaction         */
#define CORE_IOC_SCRIPT_LOAD    0x83Cu  /* download compiled bytecode      */
#define CORE_IOC_SCRIPT_FAULT   0x83Du  /* drain the post-mortem dump      */
#define CORE_IOC_STICK_CLIP     0x83Eu  /* get and/or set                  */
#define CORE_IOC_STICK_STRETCH  0x83Fu  /* get and/or set                  */
#define CORE_IOC_PAK_STATUS     0x840u  /* 3-byte status read, 1 byte out  */
#define CORE_IOC_PAK_READ       0x841u  /* Controller Pak block read       */
#define CORE_IOC_PAK_WRITE      0x842u  /* Controller Pak block write      */
#define CORE_IOC_EFFECT_PROG    0x852u  /* program an effect slot          */
#define CORE_IOC_EFFECT_CTRL    0x853u  /* start, stop, or stop all        */
#define CORE_IOC_EFFECT_QUERY   0x855u  /* is a slot still running         */

/* The NTSTATUS values this surface returns. Kept as plain constants so the
 * dispatcher needs no Windows header. */
#define CORE_ST_SUCCESS         0x00000000u
#define CORE_ST_PENDING         0x00000103u
#define CORE_ST_DEVICE_BUSY     0x80000011u
#define CORE_ST_INVALID_PARAM   0xC000000Du
#define CORE_ST_DEVICE_NOT_READY 0xC0000013u
#define CORE_ST_DATA_ERROR      0xC000003Eu
#define CORE_ST_CRC_ERROR       0xC000003Fu
#define CORE_ST_NOT_SUPPORTED   0xC00000BBu
#define CORE_ST_BUFFER_TOO_SMALL 0xC0000023u
#define CORE_ST_DEVICE_NOT_READY_A3 0xC00000A3u

/*
 * STATUS_INSUFFICIENT_RESOURCES. NOT A CODE THE ORIGINAL CAN RETURN -
 * drv_ScriptLoad is void, so fn 0x83C answers success whether or not the
 * allocation succeeded. See ioc_script_load for why that is diverged from.
 */
#define CORE_ST_NO_MEMORY       0xC000009Au

/* Counter selectors for CORE_IOC_READ_COUNTER and CORE_IOC_ZERO_COUNTER. */
#define CORE_COUNTER_FIRMWARE   1   /* bcdDevice; read only            */
#define CORE_COUNTER_TWO        2
#define CORE_COUNTER_REPORTS    3
#define CORE_COUNTER_PROBE      4   /* accessory probe state; read only */

/* Commands for CORE_IOC_EFFECT_CTRL. */
#define CORE_EFFECT_CMD_START   1
#define CORE_EFFECT_CMD_STOP    2
#define CORE_EFFECT_CMD_STOP_ALL 3

/*
 * The header of a CORE_IOC_EFFECT_PROG request, before the payload streams.
 * Twenty-eight bytes; see ../docs/ioctl-surface.txt.
 */
#define CORE_EFFECT_PROG_HEADER 0x1C

/* One decoded device-control request. in and out are the caller's buffers;
 * for METHOD_BUFFERED they are the same allocation, which is why the
 * dispatcher never writes output before it has finished reading input. */
typedef struct core_ioctl {
	u32       code;
	const u8 *in;
	u32       in_len;
	u8       *out;
	u32       out_len;
} core_ioctl;

/*
 * THE TWO SEAMS.
 *
 * The raw vendor passthrough hands a caller-supplied setup packet to the
 * transport and may complete asynchronously, which is the only case on this
 * surface that can return CORE_ST_PENDING. The enable toggle reaches into
 * PnP state. Both are the OS's business.
 */
typedef u32 (*core_ioctl_vendor_fn)(void *ctx, const u8 *setup, u8 *data,
                                    u32 data_len);
typedef void (*core_ioctl_enable_fn)(void *ctx, int on);

typedef struct core_ioctl_env {
	core_state *cs;
	core_sched *sched;

	core_ioctl_vendor_fn vendor;
	void                *vendor_ctx;
	core_ioctl_enable_fn enable;
	void                *enable_ctx;

	/* Milliseconds-since-boot equivalent, in 100ns units, for the effect
	 * timestamps. The caller advances it; nothing here reads a clock. */
	u64 now_100ns;
} core_ioctl_env;

/*
 * Dispatch one request. Returns the status; *info receives what belongs in
 * IoStatus.Information. An unrecognised code gives CORE_ST_NOT_SUPPORTED and
 * a recognised one with the wrong buffer sizes gives CORE_ST_INVALID_PARAM -
 * a distinction worth preserving, because it is how a caller probing a live
 * device tells "this driver does not do that" from "I asked wrongly".
 */
u32 core_ioctl_dispatch(core_ioctl_env *env, const core_ioctl *req, u32 *info);

/*
 * Is an effect slot still running? Exposed because CORE_IOC_EFFECT_QUERY is
 * a thin wrapper over it and because it has a side effect: a slot whose
 * duration has elapsed is marked stopped here.
 */
int core_effect_slot_active(core_state *cs, s32 slot, s32 now_tick);

/* ======================================================================
 * THE HID MINIDRIVER CONTRACT
 *
 * IRP_MJ_INTERNAL_DEVICE_CONTROL, and the reason this driver is a HID
 * device at all. hidclass.sys - never user mode - asks these eight
 * questions, and the answer to CORE_HID_IOC_REPORT_DESC is what tells
 * Windows there is a keyboard, a mouse and a game controller here.
 *
 * WITHOUT THIS THE DRIVER DOES NOTHING. It would load, enumerate, poll the
 * adapter and produce reports that no device object exists to carry.
 *
 * Codes are CTL_CODE(FILE_DEVICE_KEYBOARD, fn, METHOD_NEITHER,
 * FILE_ANY_ACCESS) = 0x000B0000 | (fn << 2) | 3, which is why they look
 * like keyboard codes on a device that is not only a keyboard.
 * ====================================================================== */

#define CORE_HID_IOC_DEVICE_DESC    0x000B0003u  /* fn 0 */
#define CORE_HID_IOC_REPORT_DESC    0x000B0007u  /* fn 1 */
#define CORE_HID_IOC_READ_REPORT    0x000B000Bu  /* fn 2 */
#define CORE_HID_IOC_WRITE_REPORT   0x000B000Fu  /* fn 3 */
#define CORE_HID_IOC_GET_STRING     0x000B0013u  /* fn 4 */
#define CORE_HID_IOC_ACTIVATE       0x000B001Fu  /* fn 7 */
#define CORE_HID_IOC_DEACTIVATE     0x000B0023u  /* fn 8 */
#define CORE_HID_IOC_ATTRIBUTES     0x000B0027u  /* fn 9 */

/* String indices, in the low half of the packed Type3InputBuffer value. */
#define CORE_HID_STRING_MANUFACTURER 0x0Eu
#define CORE_HID_STRING_PRODUCT      0x0Fu
#define CORE_HID_STRING_SERIAL       0x10u

#define CORE_HID_DEVICE_DESC_BYTES  9
#define CORE_HID_ATTRIBUTES_BYTES   0x20

/* The USB identity the device reports, matching its descriptors. */
#define CORE_USB_VENDOR_ID      0x06F7u
#define CORE_USB_PRODUCT_ID     0x0001u
#define CORE_USB_VERSION        0x0100u

/*
 * Answer one minidriver request.
 *
 * out and out_len are the caller's output buffer; in_len is its input
 * length, which CORE_HID_IOC_WRITE_REPORT reports straight back; arg is the
 * Type3InputBuffer value, carrying the string index or the collection
 * number depending on the code.
 *
 * CORE_HID_IOC_READ_REPORT IS NOT ANSWERED HERE - it parks an IRP, which is
 * the OS layer's business, and the caller must intercept it first.
 */
u32 core_hid_ioctl(core_state *cs, u32 code, u8 *out, u32 out_len,
                   u32 in_len, u32 arg, u32 *info);

/* ======================================================================
 * THE CONTROL DEVICE
 *
 * A second surface on a second device object. drv_CreateControlDevice makes
 * one singleton control device for the whole driver, with its own extension,
 * its own symbolic link and its own dispatcher, drv_IoctlControlDevice
 * (0001061f). Nothing on the per-device surface above touches it.
 *
 * It does three things: it answers questions about the DRIVER rather than
 * about one adapter, it owns the user-mode notification queue, and it
 * forwards the eighteen per-device codes after stripping a four-byte device
 * handle off the front of the input.
 *
 * The device object itself - IoCreateDevice, the symlink, the open count -
 * is wdm.c's business. What lives here is the registry those handles name,
 * the queue, and the dispatch.
 * ====================================================================== */

/* Control-device function codes. Same encoding as the per-device ones. */
#define CORE_CTL_DEVICE_COUNT   0x801u  /* out 1: live adapters           */
#define CORE_CTL_RESERVED_802   0x802u  /* in 0x40 out 0x40, does nothing */
#define CORE_CTL_RESERVED_803   0x803u  /* the same handler               */
#define CORE_CTL_VERSION        0x811u  /* out >= 4: the driver version   */
#define CORE_CTL_GENERATION     0x812u  /* out 4: list generation counter */
#define CORE_CTL_UNIMPLEMENTED  0x814u  /* validated, then not supported  */
#define CORE_CTL_SET_REPORTS    0x816u  /* in 4: the virtual-mode switch  */
#define CORE_CTL_BUTTON_MAP     0x817u  /* get and set the global map     */
#define CORE_CTL_WAIT_NOTIFY    0x818u  /* out 0xc: park until an event   */
#define CORE_CTL_ENUM_DEVICES   0x821u  /* in 4 out 8: walk the registry  */
#define CORE_CTL_LOOKUP_DEVICE  0x822u  /* in 4 out 4: resolve an id      */

/* Two more statuses this surface returns. */
#define CORE_ST_NO_SUCH_DEVICE  0xC000000Eu
#define CORE_ST_DELETE_PENDING  0xC0000056u

/* The driver version this reports, as four bytes. */
#define CORE_CTL_VERSION_BYTES  4

/* An event delivered to a waiter is three dwords. */
#define CORE_NOTIFY_BYTES       12

/*
 * THE NOTIFICATION QUEUE.
 *
 * Events that are not HID input - script faults, the absolute mouse, _debug,
 * interface state changes - end up here rather than in a report. User mode
 * parks a request on function 0x818 and gets twelve bytes back when one
 * arrives.
 *
 * THE HUNDRED-EVENT CAP IS LIVE HERE, unlike the identical-looking guard in
 * drv_QueueEvent that nothing ever arms (see sched.h). A full queue drops
 * its OLDEST entry, so a listener that stops reading loses history rather
 * than blocking the driver.
 *
 * THE QUEUE IS DRIVER-WIDE, NOT PER DEVICE. Every adapter posts into this
 * one queue and any waiter can receive any adapter's event, which is why an
 * event carries no device identity and a listener has to re-enumerate to
 * find out what changed.
 */
#define CORE_NOTIFY_MAX         100

/*
 * A parked waiter. The queue never touches the request itself; it holds an
 * opaque handle and asks the owner to claim it before delivering, because
 * whether a request is still ours is a cancellation question and therefore
 * the OS's to answer.
 */
typedef struct core_notify_waiter {
	struct core_notify_waiter *flink;
	struct core_notify_waiter *blink;
	void *request;
} core_notify_waiter;

/*
 * Claim a waiter for delivery. Return non-zero if it is still ours; zero
 * means cancellation got there first and the queue must drop it silently -
 * the canceller owns completing it. This is the InterlockedExchange on the
 * cancel routine that drv_CompleteNotificationIrps does.
 */
typedef int (*core_notify_claim_fn)(void *ctx, core_notify_waiter *w);

/* Hand one event to one claimed waiter, twelve bytes. */
typedef void (*core_notify_deliver_fn)(void *ctx, core_notify_waiter *w,
                                       u32 type, u32 arg1, u32 arg2);

/* Complete a waiter that will never be served, on teardown. */
typedef void (*core_notify_abort_fn)(void *ctx, core_notify_waiter *w);

typedef struct core_notify {
	core_sched_event events[CORE_NOTIFY_MAX];
	s32 head;
	s32 count;
	u32 dropped;

	core_notify_waiter waiters;     /* list head sentinel */
	s32 waiter_count;

	/*
	 * The delivery latch. Posting and parking both try to pump, and the
	 * pump runs with the lock dropped; this keeps exactly one pump in
	 * flight so an event cannot be handed to two waiters.
	 */
	int delivering;

	core_notify_claim_fn   claim;
	core_notify_deliver_fn deliver;
	core_notify_abort_fn   abort;
	void                  *ctx;
} core_notify;

void core_notify_init(core_notify *n, core_notify_claim_fn claim,
                      core_notify_deliver_fn deliver,
                      core_notify_abort_fn abort, void *ctx);

/* Append an event and deliver what can be delivered. */
void core_notify_post(core_notify *n, u32 type, u32 arg1, u32 arg2);

/* Park a waiter. Returns 1 if it was delivered to before returning. */
int  core_notify_wait(core_notify *n, core_notify_waiter *w);

/* Withdraw a waiter that is being cancelled. Returns 1 if it was still
 * queued, 0 if delivery had already taken it. */
int  core_notify_cancel(core_notify *n, core_notify_waiter *w);

/* Abort every parked waiter. The last adapter going away does this, and so
 * does closing the control device. */
void core_notify_flush(core_notify *n);

/* ======================================================================
 * THE SDK COMMAND-BLOCK CHANNEL
 *
 * The control device's OTHER user-mode surface. Alongside its eleven IOCTLs
 * it answers IRP_MJ_READ and IRP_MJ_WRITE, and what flows over those is a
 * 64-byte block of N64 JOYBUS COMMANDS - the simple interface the vendor SDK
 * documents, as against the IOCTLs the configurator itself uses.
 *
 * A block is a sequence of variable-length entries:
 *
 *     +------+------+--------------------+------------------+
 *     | 0x00 | 0x01 | 0x02 .. 0x01+CMD   | .. + REPLY       |
 *     +======+======+====================+==================+
 *     | CMD  | RPLY | the command bytes  | space for the    |
 *     | len  | len  | to put on the bus  | controller reply |
 *     +------+------+--------------------+------------------+
 *
 * with three length bytes that are not lengths at all:
 *
 *     0x00   padding. Skip one byte AND ADVANCE THE DEVICE INDEX.
 *     0xFE   end of block.
 *     0xFF   skip one byte, device index unchanged.
 *
 * Each real entry addresses the Nth adapter, N counting entries so far, so
 * position in the block is what selects the controller - a block can poll
 * four adapters in one transfer. Bit 0x80 in the reply-length byte is how a
 * failed entry is reported back.
 *
 * TWO PASSES, WHICH IS THE WHOLE TRICK. A write runs pass 0 and a read runs
 * pass 1, over the same block. Pass 1 handles the two entries the driver can
 * answer from state it already has; pass 0 handles everything else, on the
 * wire. So the common case - "read the controller" - costs a read call and
 * NO USB TRAFFIC AT ALL, because the answer comes out of the interrupt-poll
 * cache. A game polling at 60Hz never touches the bus.
 *
 * See ../docs/command-block.txt.
 * ====================================================================== */

#define CORE_CMD_BLOCK_BYTES    0x40    /* one block, fixed                */
#define CORE_CMD_GO             0x3F    /* the go flag, last byte of it    */
#define CORE_CMD_SWAP_AT        0x3C    /* the byte-order word             */
#define CORE_CMD_MAX_LEN        0x26    /* largest command or reply        */
#define CORE_CMD_MAX_DEVICES    5       /* entries per block               */
#define CORE_CMD_FAILED         0x80    /* set in the reply-length byte    */
#define CORE_CMD_LEN_MASK       0x3F

/* The entry lengths that are not lengths. */
#define CORE_CMD_PAD            0x00
#define CORE_CMD_END            0xFE
#define CORE_CMD_SKIP           0xFF

/*
 * THE TRANSPORT SEAM, in two halves because the original distinguishes them.
 *
 * cmd_claim takes the adapter's vendor slot and returns non-zero on success;
 * a busy slot means the entry is skipped. cmd_xfer runs one control transfer
 * and returns the status.
 *
 * keep IS THE INTERESTING ARGUMENT. A long-form command is two transfers,
 * a write carrying the command and a read collecting the reply, and nothing
 * may be interleaved between them. keep non-zero on the first says "hold the
 * slot, another transfer follows"; zero on the last releases it. That is
 * exactly the distinction between drv_VendorRequestCompleteKeepSlot
 * (00018d10) and drv_VendorRequestComplete (00018cf0), two functions whose
 * only difference is the value they return.
 */
typedef int (*core_cmd_claim_fn)(void *ctx);
typedef u32 (*core_cmd_xfer_fn)(void *ctx, const u8 *setup, u8 *data,
                                u32 len, int keep);

/* ---- the device registry ---------------------------------------------- */

/*
 * One registered adapter. The entry is owned by the caller and lives in its
 * device extension, so the registry is an intrusive list and has no bound -
 * matching the original, whose list is also unbounded even though the count
 * is reported as a single byte.
 */
typedef struct core_device_entry {
	struct core_device_entry *flink;
	struct core_device_entry *blink;

	u32         handle;     /* what user mode passes back; a PDO pointer
	                         * in the original */
	core_state *cs;
	core_sched *sched;
	int         live;       /* its device interface is enabled */
	int         needs_resubmit;

	/*
	 * THE OS SEAMS FOR THIS ADAPTER. A request that arrives on the
	 * control device names a device by handle and is then executed
	 * against it, so the transport cannot come from the caller's
	 * environment the way it does on the per-device surface - it has to
	 * be reachable from the registry entry.
	 *
	 * All four may be null, in which case the affected requests fail
	 * rather than reaching a device.
	 */
	core_ioctl_vendor_fn vendor;
	core_ioctl_enable_fn enable;
	core_cmd_claim_fn    cmd_claim;
	core_cmd_xfer_fn     cmd_xfer;
	void                *os_ctx;
} core_device_entry;

typedef struct core_registry {
	core_device_entry devices;      /* list head sentinel */
	s32 count;
	s32 live_count;
	u32 generation;

	/*
	 * THE BUTTON MAP IS DRIVER-WIDE in the original: one drv_ButtonMap
	 * array, shared by every adapter. Setting it here writes through to
	 * every registered device, because core_decode_buttons reads a
	 * per-device copy.
	 */
	u8 button_map[CORE_RAW_BUTTON_BITS];

	/* The virtual-joystick switch, function 0x816. */
	int reports_enabled;

	core_notify notify;
} core_registry;

void core_registry_init(core_registry *reg);
void core_registry_add(core_registry *reg, core_device_entry *dev);
void core_registry_remove(core_registry *reg, core_device_entry *dev);

/* Mark an adapter's interface up or down. Bumps the generation counter and
 * the live count, and posts the interface-changed event. */
void core_registry_set_live(core_registry *reg, core_device_entry *dev,
                            int live);

/*
 * Dispatch one control-device request. env supplies the clock and the seams
 * for whichever adapter a forwarded request names; reg is the registry.
 *
 * A waiter is supplied only for CORE_CTL_WAIT_NOTIFY and may be null, in
 * which case that one function answers CORE_ST_INVALID_PARAM.
 */
u32 core_ctl_dispatch(core_registry *reg, const core_ioctl *req,
                      core_notify_waiter *waiter, u64 now_100ns, u32 *info);

/* ---- the command-block channel ---------------------------------------- */

/*
 * The channel's own state, one instance per control device.
 *
 * block IS THE DRIVER'S COPY, not the caller's. A write stores into it and a
 * read hands it back, so the two directions share one buffer and a client
 * can write a block of commands and then read the answers out of it.
 *
 * swap_bytes REMEMBERS A BIG-ENDIAN CLIENT. A write whose dword at
 * CORE_CMD_SWAP_AT is 1 is byte-swapped on the way in and every subsequent
 * read is byte-swapped on the way out. Note what that word becomes after
 * swapping its own dword: byte 0x3C moves to 0x3F, which is the go flag. A
 * big-endian client therefore cannot write a block WITHOUT asking for it to
 * run. That is the original's behaviour, not an accident of this port.
 */
typedef struct core_cmd_channel {
	u8  block[CORE_CMD_BLOCK_BYTES];
	int swap_bytes;
} core_cmd_channel;

void core_cmd_channel_init(core_cmd_channel *ch);

/*
 * Walk one block, executing the entries this pass owns.
 *
 * pass 1 answers from cached state: the controller-state read (command 0x01
 * reply 4) entirely, and the identify (command 0x01 reply 3) by running it
 * and then updating the emulated Pak state from the result. pass 0 runs
 * everything else through the transport.
 *
 * Failures are marked in place with CORE_CMD_FAILED, and there are TWO KINDS
 * of them: a structural error - a length out of range, too many entries, an
 * entry running off the end - marks the entry and STOPS the walk, while a
 * missing device or an over-long reply marks it and CARRIES ON. The original
 * distinguishes these by whether the failure path returns or falls through,
 * and a client can tell them apart by whether later entries were answered.
 */
void core_cmd_process(core_registry *reg, u8 *block, int pass, u64 now_100ns);

/*
 * Execute one entry against one adapter. entry[0] is the command length and
 * entry[1] the reply length, already masked.
 *
 * Three paths, and only one of them reaches the bus - see the comment on the
 * definition for what the driver emulates and why.
 */
void core_cmd_exec(core_device_entry *dev, u8 *entry);

/*
 * The IRP_MJ_READ and IRP_MJ_WRITE halves of the channel.
 *
 * buf is the caller's buffer and len its length, both directions. *info
 * receives the byte count for IoStatus.Information.
 *
 * A one-byte READ is the live adapter count and is the ONE request allowed
 * when no adapter is present, so a client can poll for arrival. Everything
 * else needs a device and a length of exactly CORE_CMD_BLOCK_BYTES.
 */
u32 core_cmd_read(core_registry *reg, core_cmd_channel *ch, u8 *buf, u32 len,
                  u64 now_100ns, u32 *info);
u32 core_cmd_write(core_registry *reg, core_cmd_channel *ch, u8 *buf, u32 len,
                   u64 now_100ns, u32 *info);

#endif /* ADAPTOID_IOCTL_H */
