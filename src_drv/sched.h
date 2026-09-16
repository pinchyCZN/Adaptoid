/*
 * sched.h - the script thread scheduler for wishk300.
 *
 * One scheduling pass runs every thread whose wake time has arrived, drains
 * the events those threads produced, and arms a timer for the next one.
 * Specified from drv_ScriptScheduler (00017980), drv_ScriptThreadQueue
 * (00017860), drv_ScriptThreadAlloc (000177c0), drv_ScriptSchedulerDpc
 * (00017910) and the load/teardown half of drv_ScriptLoad (00017310).
 *
 * Like core.c and script.c this file is OS-free. Three things it cannot do
 * without help are seams: allocating memory, arming a timer, and delivering
 * an event. See ../docs/script-bytecode.txt and ../docs/driver-structures.txt.
 */
#ifndef ADAPTOID_SCHED_H
#define ADAPTOID_SCHED_H

#include "script.h"

/*
 * THE INSTRUCTION BUDGET IS A TOKEN BUCKET, shared by every thread on the
 * device. Each pass credits it with the real time that has elapsed since the
 * last pass, in units of 100 * 100ns = 10us, and saturates at
 * CORE_SCRIPT_BUDGET. The interpreter spends one token per instruction.
 *
 * The cap therefore equals exactly one second of accrual, which makes this a
 * 100000 instruction per second rate limit with a one second burst.
 *
 * WHAT HAPPENS WHEN IT RUNS OUT IS NOT A PAUSE. core_script_run reports
 * CORE_SCRIPT_BUDGET_OUT, which is a fault like any other: the thread is
 * retired into the post-mortem slot and an event of type CORE_EVENT_FAULT is
 * posted. It is not rescheduled. The budget is a kill switch for a runaway
 * script, not a throttle, and because the bucket is per device rather than
 * per thread a greedy thread can empty it and take its neighbours down in
 * the same pass.
 */
#define CORE_SCHED_TICK_100NS   100

/* drv_ScriptThreadAlloc clamps every request up to this many words. */
#define CORE_SCHED_MIN_STACK    8

/*
 * Event types, established in drv_DispatchEvents (00015e70) and named from
 * the compiler's own symbol table in wishd201.exe. Types 0..2 become real HID
 * input inside the driver; everything else is a user-mode notification.
 */
#define CORE_EVENT_KEY          0     /* _key(usage, down)             */
#define CORE_EVENT_MOUSE_BUTTON 1     /* _mouse_button(button, down)   */
#define CORE_EVENT_MOUSE_REL    2     /* _mouse_relative(dx, dy)       */
#define CORE_EVENT_MOUSE_ABS    3     /* _mouse_absolute(x, y)         */
#define CORE_EVENT_FAULT        0x42  /* arg1 is the interpreter status */
#define CORE_EVENT_DEBUG        0x4D  /* _debug(a, b)                  */
#define CORE_EVENT_INTERFACE    99    /* interface arrived or departed */

/*
 * THE EVENT BUFFER IS BOUNDED HERE AND WAS NOT IN THE ORIGINAL.
 *
 * drv_QueueEvent carries the guard - when its count exceeds 99 it recycles
 * the oldest node instead of allocating - but nothing ever increments that
 * count. Both sites that create an event list pass either a null pointer
 * (drv_NotifyInterfaceChange) or a local that stays zero
 * (drv_ScriptScheduler), so the branch is dead in the shipped driver and the
 * list can grow without limit.
 *
 * This is the original's own policy, activated: a fixed ring, and a full ring
 * drops its oldest entry. events_dropped counts what that cost, because a
 * silent drop is worse than a bounded one.
 */
#define CORE_SCHED_EVENTS       100

/*
 * Handler slots in the globals array, the input binding in sched.c. Slots
 * 0x00..0x0F are one per raw button bit, slot n being raw button index n.
 */
#define CORE_SCHED_SLOT_BUTTONS 0x10  /* any button changed  */
#define CORE_SCHED_SLOT_STICK   0x11  /* the stick moved     */
#define CORE_SCHED_SLOT_PRE     0x12  /* anything, queued first */
#define CORE_SCHED_SLOT_POST    0x13  /* anything, queued last  */

typedef struct core_sched_event {
	u32 type;
	u32 arg1;
	u32 arg2;
} core_sched_event;

/*
 * A schedulable thread. The operand stack travels with the thread; only the
 * thread that is actually running has its stack unpacked into the shared
 * 200-word local region, which is why two threads in the same function share
 * that function's locals but not their saved stacks.
 *
 * NOT binary compatible with the original's 0x24-byte header. That header
 * used two 32-bit halves for the wake time and put the stack immediately
 * after itself; this uses a u64 and a pointer. The layout is private, the
 * behaviour is not.
 */
typedef struct core_sched_thread {
	struct core_sched_thread *flink;
	struct core_sched_thread *blink;

	u32  thread_id;
	s32  pc;
	u32  saved_acc;         /* Thread+0x10, the saved accumulator     */
	s32  saved_depth;       /* Thread+0x14, saved stack depth in words */
	u64  wake_time;         /* 100ns units                            */

	s32  stack_size;        /* capacity in words, >= saved_depth      */
	u32 *stack;             /* carved out of the same allocation      */
} core_sched_thread;

/* ---- the three seams --------------------------------------------------- */

/* Zeroing is NOT required; the scheduler initialises what it uses. */
typedef void *(*core_sched_alloc_fn)(void *ctx, u32 bytes);
typedef void  (*core_sched_free_fn)(void *ctx, void *block);

/*
 * Arm a one-shot timer to fire at wake_time. Called at the end of a pass
 * that leaves threads queued.
 *
 * The original passes KeSetTimer a due time of (now - wake), which is
 * negative and therefore a RELATIVE delay of exactly (wake - now) 100ns
 * units. An absolute wake time is handed over here instead and the OS layer
 * does that conversion, because the sign trick is a KeSetTimer convention
 * rather than anything the scheduler means.
 */
typedef void (*core_sched_arm_fn)(void *ctx, u64 wake_time);

/* Deliver one drained event. Types 0..2 are HID input; the rest go to the
 * user-mode notification queue. */
typedef void (*core_sched_event_fn)(void *ctx, u32 type, u32 arg1, u32 arg2);

typedef struct core_sched {
	/*
	 * The interpreter. code, vars and the native seam live here for the
	 * lifetime of a load; pc, acc, sp and budget are swapped in and out
	 * around every thread, exactly as drv_ScriptExecute does with its
	 * locals.
	 */
	core_script vm;
	u32        *code;           /* our copy, freed on unload */

	core_sched_thread ready;    /* list head sentinel, sorted by wake time */
	core_sched_thread freepool; /* list head sentinel                      */
	s32 thread_count;
	u32 next_thread_id;

	s32 budget;
	u64 sched_time;

	/*
	 * The thread currently inside the interpreter, or 0. This is how a
	 * native builtin reaches the thread that called it: the seam hands a
	 * native only the core_script, so _sleep sets current->wake_time and
	 * returns CORE_SCRIPT_SLEEPING. The original passes the thread as an
	 * argument to drv_ScriptNativeCall instead; this keeps script.c from
	 * having to know that threads exist.
	 */
	core_sched_thread *current;

	/* Re-entrancy latch. See core_sched_run. */
	int running;
	int pending_valid;
	u64 pending_time;

	/*
	 * The post-mortem slot. A faulted thread is neither freed nor
	 * rescheduled: it is parked here with a snapshot of the globals, and
	 * the previous occupant is released. One slot, device-wide.
	 */
	core_sched_thread *fault_thread;
	u32               *fault_vars;
	int                fault_status;

	core_sched_event events[CORE_SCHED_EVENTS];
	s32 event_head;
	s32 event_count;
	u32 events_dropped;

	/*
	 * The packed input report and the one before it, four bytes each:
	 * X, Y, and the two button bytes in wire order. core_sched_on_input
	 * maintains the pair. The original keeps it in the device extension,
	 * written by drv_BuildJoystickReport.
	 */
	u8 cur_input[4];
	u8 prev_input[4];

	/* Passed as arg2 of a fault event; the original passes its PDO. */
	u32 device_tag;

	core_sched_alloc_fn alloc;
	core_sched_free_fn  release;
	void               *mem_ctx;

	core_sched_arm_fn   arm;
	void               *arm_ctx;

	core_sched_event_fn emit;
	void               *emit_ctx;
} core_sched;

void core_sched_init(core_sched *s, core_sched_alloc_fn alloc,
                     core_sched_free_fn release, void *mem_ctx);

void core_sched_set_arm(core_sched *s, core_sched_arm_fn fn, void *ctx);
void core_sched_set_event_sink(core_sched *s, core_sched_event_fn fn,
                               void *ctx);
void core_sched_set_native(core_sched *s, core_script_native_fn fn,
                           void *ctx);

/*
 * Replace the running script. Tears down unconditionally - timer, code,
 * vars, both thread lists, the post-mortem slot and all scheduler state -
 * then loads if both counts are non-zero. A call with code_count 0 is
 * therefore the unload, which is how the original stops a script when the
 * device goes away.
 *
 * On success thread 0 is created at pc 0 and a pass is run immediately.
 * Returns 1 on success, 0 if an allocation failed or the counts were zero.
 */
int  core_sched_load(core_sched *s, const u32 *code, s32 code_count,
                     s32 var_count, u64 now);
void core_sched_unload(core_sched *s);

/*
 * Get a thread node, from the free pool when one is big enough. stack_size
 * is in words and is clamped up to CORE_SCHED_MIN_STACK; a pooled node may
 * be larger than asked for. Returns 0 if the allocation failed.
 *
 * The caller may write arguments into t->stack before queueing. Note that
 * saved_depth starts at 0, so those words sit below the operand stack and
 * the first push overwrites the first of them - see the note in sched.c.
 */
core_sched_thread *core_sched_thread_alloc(core_sched *s, s32 stack_size);

/* Insert into the ready list before the first thread due strictly later,
 * so equal wake times keep their arrival order. */
void core_sched_queue(core_sched *s, core_sched_thread *t);

/*
 * Run a pass at time now. Never recurses and never blocks: a call arriving
 * while a pass is in flight records its timestamp and returns, and the pass
 * in flight picks it up and goes round again.
 */
void core_sched_run(core_sched *s, u64 now);

/* Append an event, dropping the oldest if the ring is full. */
void core_sched_post_event(core_sched *s, u32 type, u32 arg1, u32 arg2);

/*
 * Feed one accepted controller packet, the raw five bytes as they arrive.
 * Queues a thread for every bound handler whose input changed and runs a
 * pass. Call it once per packet, after core_decode has accepted it -
 * drv_BuildJoystickReport calls the original at exactly that point.
 *
 * Returns 0 when no script is loaded and 1 otherwise, which is what the
 * original returns; note that includes the case where nothing changed and
 * no thread was queued.
 */
int core_sched_on_input(core_sched *s, const u8 *raw, u64 now);

#endif /* ADAPTOID_SCHED_H */
