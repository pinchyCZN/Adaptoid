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

/* The adapter reports controller state in five raw bytes; see
 * ../docs/usb-transport.txt. */
#define CORE_RAW_PACKET_BYTES   5

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

/* Input: one raw packet off the interrupt pipe. */
void core_on_raw_packet(core_state *cs, const u8 *raw);

/* Time: advance the engine. now_100ns is monotonic, in 100ns units. */
void core_tick(core_state *cs, u64 now_100ns);

/*
 * Controller Pak CRCs. Pure functions, no state - the natural first real unit
 * test for the harness. Named after drv_N64PakAddrCrc5 / drv_N64PakDataCrc8.
 */
u8 core_pak_addr_crc5(u16 address);
u8 core_pak_data_crc8(const u8 *data, u32 len);

#endif /* ADAPTOID_CORE_H */
