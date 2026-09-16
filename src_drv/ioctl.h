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

#endif /* ADAPTOID_IOCTL_H */
