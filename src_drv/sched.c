/*
 * sched.c - the script thread scheduler.
 *
 * See sched.h for the model. The originals are drv_ScriptScheduler
 * (00017980), drv_ScriptThreadQueue (00017860), drv_ScriptThreadAlloc
 * (000177c0) and the load/teardown half of drv_ScriptLoad (00017310).
 */

#include "sched.h"

/* Defined below; core_sched_init installs it. */
int core_sched_native(void *ctx, core_script *vm, u32 id);

/* ---- list primitives --------------------------------------------------- */

static void list_init(core_sched_thread *head)
{
	head->flink = head;
	head->blink = head;
}

static int list_empty(const core_sched_thread *head)
{
	return head->flink == head;
}

static void list_insert_before(core_sched_thread *at, core_sched_thread *t)
{
	t->flink        = at;
	t->blink        = at->blink;
	at->blink->flink = t;
	at->blink        = t;
}

static void list_unlink(core_sched_thread *t)
{
	t->blink->flink = t->flink;
	t->flink->blink = t->blink;
	t->flink = 0;
	t->blink = 0;
}

/* ---- memory ------------------------------------------------------------ */

static void *sched_alloc(core_sched *s, u32 bytes)
{
	if (s->alloc == 0) {
		return 0;
	}
	return s->alloc(s->mem_ctx, bytes);
}

static void sched_free(core_sched *s, void *block)
{
	if (block != 0 && s->release != 0) {
		s->release(s->mem_ctx, block);
	}
}

/* ---- events ------------------------------------------------------------ */

void core_sched_post_event(core_sched *s, u32 type, u32 arg1, u32 arg2)
{
	s32 slot;

	if (s == 0) {
		return;
	}
	if (s->event_count >= CORE_SCHED_EVENTS) {
		/* Full: drop the oldest, which is what the original's own
		 * recycle branch does with the head node. */
		s->event_head = (s->event_head + 1) % CORE_SCHED_EVENTS;
		s->event_count--;
		s->events_dropped++;
	}
	slot = (s->event_head + s->event_count) % CORE_SCHED_EVENTS;
	s->events[slot].type = type;
	s->events[slot].arg1 = arg1;
	s->events[slot].arg2 = arg2;
	s->event_count++;
}

static void sched_drain_events(core_sched *s)
{
	while (s->event_count > 0) {
		core_sched_event e = s->events[s->event_head];

		s->event_head = (s->event_head + 1) % CORE_SCHED_EVENTS;
		s->event_count--;
		if (s->emit != 0) {
			s->emit(s->emit_ctx, e.type, e.arg1, e.arg2);
		}
	}
}

/* ---- setup ------------------------------------------------------------- */

void core_sched_init(core_sched *s, core_sched_alloc_fn alloc,
                     core_sched_free_fn release, void *mem_ctx)
{
	s32 i;

	if (s == 0) {
		return;
	}
	/* Deliberately field by field rather than a memset: core has no CRT. */
	s->vm.code       = 0;
	s->vm.code_count = 0;
	s->vm.vars       = 0;
	s->vm.var_count  = 0;
	s->vm.budget     = 0;
	s->vm.pc         = 0;
	s->vm.acc        = 0;
	s->vm.sp         = 0;
	s->vm.native     = 0;
	s->vm.native_ctx = 0;
	s->code          = 0;

	list_init(&s->ready);
	list_init(&s->freepool);
	s->thread_count   = 0;
	s->next_thread_id = 0;

	s->budget     = 0;
	s->sched_time = 0;

	s->running       = 0;
	s->pending_valid = 0;
	s->pending_time  = 0;

	s->cs           = 0;
	s->load_time    = 0;
	s->current      = 0;
	s->fault_thread = 0;
	s->fault_vars   = 0;
	s->fault_status = 0;

	for (i = 0; i < CORE_SCHED_EVENTS; i++) {
		s->events[i].type = 0;
		s->events[i].arg1 = 0;
		s->events[i].arg2 = 0;
	}
	s->event_head     = 0;
	s->event_count    = 0;
	s->events_dropped = 0;

	for (i = 0; i < 4; i++) {
		s->cur_input[i]  = 0;
		s->prev_input[i] = 0;
	}
	s->device_tag = 0;

	s->alloc     = alloc;
	s->release   = release;
	s->mem_ctx   = mem_ctx;
	s->arm       = 0;
	s->arm_ctx   = 0;
	s->emit      = 0;
	s->emit_ctx  = 0;

	/* The builtin library is on by default; a caller that wants something
	 * else installs it with core_sched_set_native. */
	core_script_set_native(&s->vm, core_sched_native, s);
}

void core_sched_set_core(core_sched *s, core_state *cs)
{
	if (s != 0) {
		s->cs = cs;
	}
}

void core_sched_set_arm(core_sched *s, core_sched_arm_fn fn, void *ctx)
{
	if (s != 0) {
		s->arm     = fn;
		s->arm_ctx = ctx;
	}
}

void core_sched_set_event_sink(core_sched *s, core_sched_event_fn fn,
                               void *ctx)
{
	if (s != 0) {
		s->emit     = fn;
		s->emit_ctx = ctx;
	}
}

void core_sched_set_native(core_sched *s, core_script_native_fn fn, void *ctx)
{
	if (s != 0) {
		core_script_set_native(&s->vm, fn, ctx);
	}
}

/* ---- thread nodes ------------------------------------------------------ */

core_sched_thread *core_sched_thread_alloc(core_sched *s, s32 stack_size)
{
	core_sched_thread *t = 0;

	if (s == 0) {
		return 0;
	}
	if (stack_size < CORE_SCHED_MIN_STACK) {
		stack_size = CORE_SCHED_MIN_STACK;
	}
	/*
	 * A thread's stack is copied into the shared 200-word local region
	 * wholesale, so a node bigger than that region could never be restored.
	 * The original cannot produce one - every stack_size it uses is either
	 * a small literal or a depth it has already clamped to 200 - but the
	 * clamp is cheap and makes that an invariant rather than a coincidence.
	 */
	if (stack_size > CORE_SCRIPT_LOCALS) {
		stack_size = CORE_SCRIPT_LOCALS;
	}

	if (!list_empty(&s->freepool)) {
		t = s->freepool.flink;
		list_unlink(t);
		if (t->stack_size < stack_size) {
			/* Too small to reuse. Put it back and allocate fresh; the
			 * original does exactly this rather than growing the node. */
			list_insert_before(s->freepool.flink, t);
			t = 0;
		}
	}

	if (t == 0) {
		u32 bytes = (u32)sizeof(core_sched_thread) + (u32)stack_size * 4u;

		t = (core_sched_thread *)sched_alloc(s, bytes);
		if (t == 0) {
			return 0;
		}
		t->stack      = (u32 *)(t + 1);
		t->stack_size = stack_size;
	}

	s->next_thread_id++;
	t->thread_id   = s->next_thread_id;
	t->pc          = 0;
	t->saved_acc   = 0;
	t->saved_depth = 0;
	t->wake_time   = 0;
	t->flink       = 0;
	t->blink       = 0;
	return t;
}

static void thread_release(core_sched *s, core_sched_thread *t)
{
	/* HEAD insert, as ExfInterlockedInsertHeadList does: the node just
	 * retired is the next one reused. */
	list_insert_before(s->freepool.flink, t);
}

void core_sched_queue(core_sched *s, core_sched_thread *t)
{
	core_sched_thread *at;

	if (s == 0 || t == 0) {
		return;
	}
	for (at = s->ready.flink; at != &s->ready; at = at->flink) {
		if (t->wake_time < at->wake_time) {
			break;
		}
	}
	/* Insert before the first thread due strictly later, so threads with
	 * equal wake times run in the order they were queued. Falling off the
	 * end lands on the sentinel, which is the tail append. */
	list_insert_before(at, t);
	s->thread_count++;
}

/* ---- load and teardown ------------------------------------------------- */

static void free_list(core_sched *s, core_sched_thread *head)
{
	while (!list_empty(head)) {
		core_sched_thread *t = head->flink;

		list_unlink(t);
		sched_free(s, t);
	}
}

void core_sched_unload(core_sched *s)
{
	if (s == 0) {
		return;
	}
	if (s->arm != 0) {
		/* Cancelling is arming for "never"; the OS layer reads a wake time
		 * of 0 as a cancel. drv_ScriptLoad calls KeCancelTimer here. */
		s->arm(s->arm_ctx, 0);
	}

	sched_free(s, s->code);
	s->code          = 0;
	s->vm.code       = 0;
	s->vm.code_count = 0;

	sched_free(s, s->vm.vars);
	s->vm.vars      = 0;
	s->vm.var_count = 0;

	free_list(s, &s->ready);
	free_list(s, &s->freepool);

	sched_free(s, s->fault_thread);
	sched_free(s, s->fault_vars);
	s->fault_thread = 0;
	s->fault_vars   = 0;
	s->fault_status = 0;

	s->thread_count   = 0;
	s->next_thread_id = 0;
	s->pending_valid  = 0;
	s->pending_time   = 0;
	s->running        = 0;
	s->current        = 0;
	s->budget         = 0;
	s->sched_time     = 0;

	s->event_head  = 0;
	s->event_count = 0;
}

int core_sched_load(core_sched *s, const u32 *code, s32 code_count,
                    s32 var_count, u64 now)
{
	core_sched_thread *t;
	s32 i;

	if (s == 0) {
		return 0;
	}
	core_sched_unload(s);

	if (code == 0 || code_count <= 0 || var_count <= 0) {
		return 0;
	}

	s->code = (u32 *)sched_alloc(s, (u32)code_count * 4u);
	if (s->code == 0) {
		return 0;
	}
	/*
	 * The globals and the 200-word local region are ONE allocation, because
	 * a local is addressed as vars[var_count + index]. The original's 0x328
	 * of slack is that region. Only the globals are zeroed.
	 */
	s->vm.vars = (u32 *)sched_alloc(s,
	                     ((u32)var_count + CORE_SCRIPT_LOCALS) * 4u);
	if (s->vm.vars == 0) {
		sched_free(s, s->code);
		s->code = 0;
		return 0;
	}

	for (i = 0; i < code_count; i++) {
		s->code[i] = code[i];
	}
	for (i = 0; i < var_count; i++) {
		s->vm.vars[i] = 0;
	}

	s->vm.code       = s->code;
	s->vm.code_count = code_count;
	s->vm.var_count  = var_count;

	s->sched_time = now;
	s->load_time  = now;
	s->budget     = CORE_SCRIPT_BUDGET;

	t = core_sched_thread_alloc(s, 0);
	if (t == 0) {
		core_sched_unload(s);
		return 0;
	}
	t->pc        = 0;
	t->wake_time = now;
	core_sched_queue(s, t);

	core_sched_run(s, now);
	return 1;
}

/* ---- running one thread ------------------------------------------------ */

/*
 * Returns the interpreter status. On CORE_SCRIPT_NO_MEMORY the node has
 * already been freed and *pt is left dangling for the caller to ignore.
 */
static int sched_execute(core_sched *s, core_sched_thread **pt)
{
	core_sched_thread *t = *pt;
	s32 i, n, depth;
	int status;

	/*
	 * Unpack the whole node, not just the live depth. The original copies
	 * stack_size words in but saves only saved_depth words out, so the
	 * slots above the depth hold whatever the node last held. Nothing reads
	 * them - the interpreter only touches [0, sp) - but reproducing the
	 * width keeps a reused node behaving identically.
	 */
	n = t->stack_size;
	if (n > CORE_SCRIPT_LOCALS) {
		n = CORE_SCRIPT_LOCALS;
	}
	for (i = 0; i < n; i++) {
		s->vm.vars[s->vm.var_count + i] = t->stack[i];
	}

	s->vm.pc     = t->pc;
	s->vm.acc    = t->saved_acc;
	s->vm.sp     = t->saved_depth;
	s->vm.budget = s->budget;

	s->current = t;
	status = core_script_run(&s->vm);
	s->current = 0;

	/* The budget is device-wide, so whatever this thread spent is gone for
	 * every other thread in the pass too. */
	s->budget = s->vm.budget;

	if (status == CORE_SCRIPT_TERMINATED) {
		return status;
	}

	depth = s->vm.sp;
	if (depth > CORE_SCRIPT_LOCALS) {
		depth = CORE_SCRIPT_LOCALS;
	}
	if (depth < 0) {
		depth = 0;
	}

	if (t->stack_size < depth) {
		/* The stack outgrew the node. Move to a bigger one, carrying the
		 * identity and the wake time; pc and acc are written below. */
		core_sched_thread *bigger;
		u32 bytes = (u32)sizeof(core_sched_thread) + (u32)depth * 4u;

		bigger = (core_sched_thread *)sched_alloc(s, bytes);
		if (bigger == 0) {
			sched_free(s, t);
			*pt = 0;
			return CORE_SCRIPT_NO_MEMORY;
		}
		bigger->stack      = (u32 *)(bigger + 1);
		bigger->stack_size = depth;
		bigger->thread_id  = t->thread_id;
		bigger->wake_time  = t->wake_time;
		bigger->flink      = 0;
		bigger->blink      = 0;
		sched_free(s, t);
		t   = bigger;
		*pt = bigger;
	}

	t->pc          = s->vm.pc;
	t->saved_acc   = s->vm.acc;
	t->saved_depth = depth;
	for (i = 0; i < depth; i++) {
		t->stack[i] = s->vm.vars[s->vm.var_count + i];
	}
	return status;
}

/*
 * Retire a faulted thread. It is neither freed nor rescheduled: it is parked
 * in the single post-mortem slot together with a snapshot of the globals,
 * and whatever was parked there before is released. A fault event carries
 * the status out to user mode.
 */
static void sched_fault(core_sched *s, core_sched_thread *t, int status)
{
	core_sched_thread *old_thread = s->fault_thread;
	u32               *old_vars   = s->fault_vars;
	u32               *snapshot;
	s32 i;

	snapshot = (u32 *)sched_alloc(s, (u32)s->vm.var_count * 4u);
	if (snapshot != 0) {
		for (i = 0; i < s->vm.var_count; i++) {
			snapshot[i] = s->vm.vars[i];
		}
	}
	/* A failed snapshot is not a failed fault - the original reports the
	 * fault either way and simply parks a null. */
	s->fault_thread = t;
	s->fault_vars   = snapshot;
	s->fault_status = status;

	sched_free(s, old_thread);
	sched_free(s, old_vars);

	core_sched_post_event(s, CORE_EVENT_FAULT, (u32)status, s->device_tag);
}

/*
 * THE NATIVE BUILTIN LIBRARY - the scripting language's standard library.
 *
 * drv_ScriptNativeCall (000186a0) is the original. The names are established
 * rather than inferred: wishd201.exe predeclares them in cfg_PredeclareSymbols
 * from a table at 00421350, and read in ascending id order that table lines up
 * one for one with the dispatch below.
 *
 * THE ARGUMENT CONVENTION. A native reads its arguments from the slots ABOVE
 * the operand stack pointer: argument k is stack slot sp + k, one based. The
 * caller puts them there with opcode 0x192, which yields the address of local
 * (depth + operand), and never pushes them, so sp is unchanged by the call and
 * the caller does not clean up.
 *
 * That also settles how a SCRIPT function receives arguments, which an earlier
 * pass left open. Opcode 0x181 pushes the return address at slot sp before
 * jumping, so the callee runs at depth sp+1 and its own local 0 is slot sp+1 -
 * the same slot the caller wrote argument 1 into. The two conventions are one
 * convention. A thread created by the input binding starts at depth 0 with its
 * arguments already at slots 0, 1, ... for the same reason: there is no return
 * address to push.
 *
 * A native returns its value by writing the accumulator, and yields or stops
 * by returning a status other than CORE_SCRIPT_RUNNING.
 */


/* A millisecond in 100ns units. */
#define CORE_MS_100NS           10000

/*
 * Argument k, one based, from the slots above the stack pointer.
 *
 * BOUNDS-CHECKED, and the original is not. Its Vars allocation carries eight
 * bytes past the 200 locals - two spare words, which is exactly enough for a
 * two-argument call made at depth 199, so the slack is argument headroom and
 * not padding. It is one word short: a call at depth 200, which the interpreter
 * permits because only a depth ABOVE 200 faults, reads argument 2 from four
 * bytes past the end of the allocation. A script cannot put a value there -
 * core_script_var_index rejects a local index above 199, so opcode 0x192 cannot
 * address it - which makes the over-read reachable but never useful. Returning
 * zero is both safe and indistinguishable for any script that could have
 * written the argument.
 */
static u32 native_arg(core_script *vm, s32 k)
{
	s32 slot = vm->sp + k;

	if (slot < 0 || slot >= CORE_SCRIPT_LOCALS) {
		return 0;
	}
	return vm->vars[vm->var_count + slot];
}

static s32 clamp_s32(s32 v, s32 lo, s32 hi)
{
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return v;
}

/* Find a thread on the ready list by id, or 0. */
static core_sched_thread *find_thread(core_sched *s, u32 id)
{
	core_sched_thread *t;

	for (t = s->ready.flink; t != &s->ready; t = t->flink) {
		if (t->thread_id == id) {
			return t;
		}
	}
	return 0;
}

/*
 * _fork. The child resumes at the same pc with a copy of the parent's live
 * stack, so the parent sees the child's id in the accumulator and the child
 * sees zero.
 *
 * DIVERGENCE, and it fixes a real over-read. The original copies the whole
 * 0x24-byte header from parent to child with one rep movsd at 00018a15 and
 * then restores only ThreadId and SavedAcc - so the child keeps the PARENT's
 * StackSize while owning a node that drv_ScriptThreadAlloc sized for the
 * parent's current DEPTH. A thread whose stack was once deep and is now
 * shallow therefore forks a child whose StackSize exceeds its allocation, and
 * the next drv_ScriptExecute restores that many words from the short node,
 * reading up to 768 bytes of pool past its end into the script's own locals.
 * Here the fields are copied one at a time and the child keeps the stack_size
 * of the node it actually got. See ../docs/known-defects.txt.
 */
static int native_fork(core_sched *s, core_script *vm)
{
	core_sched_thread *parent = s->current;
	core_sched_thread *child;
	s32 depth = vm->sp;
	s32 i;

	if (s->thread_count > CORE_SCHED_MAX_THREADS - 1) {
		vm->acc = (u32)-1;
		return CORE_SCRIPT_RUNNING;
	}
	if (depth < 0) {
		depth = 0;
	}
	child = core_sched_thread_alloc(s, depth);
	if (child == 0) {
		vm->acc = (u32)-1;
		return CORE_SCRIPT_RUNNING;
	}

	/* Everything the header copy carries, EXCEPT stack_size and the list
	 * links, and with thread_id and saved_acc as the original restores them. */
	child->pc          = vm->pc;
	child->saved_depth = depth;
	child->wake_time   = parent->wake_time;
	child->saved_acc   = 0;

	for (i = 0; i < depth && i < child->stack_size; i++) {
		child->stack[i] = vm->vars[vm->var_count + i];
	}

	/* The parent's own saved depth is written here too; it is already this
	 * value, but the original stores it and so does this. */
	parent->saved_depth = depth;

	vm->acc = child->thread_id;
	core_sched_queue(s, child);
	return CORE_SCRIPT_RUNNING;
}

int core_sched_native(void *ctx, core_script *vm, u32 id)
{
	core_sched *s = (core_sched *)ctx;
	core_state *cs;
	u32 a1, a2;

	if (s == 0 || vm == 0 || s->current == 0) {
		return CORE_SCRIPT_BAD_NATIVE;
	}
	cs = s->cs;
	a1 = native_arg(vm, 1);
	a2 = native_arg(vm, 2);

	switch (id) {

	case CORE_FN_BUTTON: {
		/* n is 1..16, a HID button number, not a raw bit index. Out of
		 * range is silently ignored, not a fault. */
		s32 n = (s32)a1;

		if (cs != 0 && n > 0 && n < 17) {
			u16 bit = (u16)(1u << (n - 1));

			if (a2 == 0) {
				cs->buttons = (u16)(cs->buttons & (u16)~bit);
			} else {
				cs->buttons = (u16)(cs->buttons | bit);
			}
		}
		return CORE_SCRIPT_RUNNING;
	}

	case CORE_FN_STICK:
	case CORE_FN_STICK_REL:
		if (cs != 0) {
			s32 x = (s32)a1;
			s32 y = (s32)a2;

			if (id == CORE_FN_STICK_REL) {
				x += cs->stick_x;
				y += cs->stick_y;
			}
			cs->stick_x = (s16)clamp_s32(x, -CORE_STICK_LIMIT,
			                                 CORE_STICK_LIMIT);
			cs->stick_y = (s16)clamp_s32(y, -CORE_STICK_LIMIT,
			                                 CORE_STICK_LIMIT);
		}
		return CORE_SCRIPT_RUNNING;

	case CORE_FN_KEY:
		core_sched_post_event(s, CORE_EVENT_KEY, a1, a2);
		return CORE_SCRIPT_RUNNING;

	case CORE_FN_MOUSE_BUTTON:
		core_sched_post_event(s, CORE_EVENT_MOUSE_BUTTON, a1, a2);
		return CORE_SCRIPT_RUNNING;

	case CORE_FN_MOUSE_REL:
		core_sched_post_event(s, CORE_EVENT_MOUSE_REL, a1, a2);
		return CORE_SCRIPT_RUNNING;

	case CORE_FN_MOUSE_ABS:
		/* An absolute 16-bit pointer position, 0x8000 the centre. The
		 * relative mouse collection cannot express this, which is why it
		 * goes to user mode instead of becoming a HID report. */
		core_sched_post_event(s, CORE_EVENT_MOUSE_ABS,
		                      (u32)clamp_s32((s32)a1, 0, 0xFFFF),
		                      (u32)clamp_s32((s32)a2, 0, 0xFFFF));
		return CORE_SCRIPT_RUNNING;

	case CORE_FN_DEBUG:
		core_sched_post_event(s, CORE_EVENT_DEBUG, a1, a2);
		return CORE_SCRIPT_RUNNING;

	case CORE_FN_EXIT:
		return CORE_SCRIPT_TERMINATED;

	case CORE_FN_TIME:
		/*
		 * Milliseconds since the script loaded - measured from this
		 * THREAD'S WAKE TIME, not from the clock. Two threads in the same
		 * pass can therefore see different times, and a thread sees the
		 * time it was scheduled for rather than the time it ran.
		 */
		vm->acc = (u32)(s32)((s64)(s->current->wake_time - s->load_time)
		                     / CORE_MS_100NS);
		return CORE_SCRIPT_RUNNING;

	case CORE_FN_SLEEP:
		/*
		 * Relative to this thread's CURRENT wake time, not to now, so a
		 * loop of sleeps does not drift by however long each pass took.
		 * The original multiplies in 32 bits, so a sleep beyond about
		 * 214748 ms wraps; reproduced, because a script relying on that
		 * is already broken and widening it would change behaviour.
		 */
		s->current->wake_time += (u64)(s64)(s32)((s32)a1 * CORE_MS_100NS);
		vm->acc = 1;
		return CORE_SCRIPT_SLEEPING;

	case CORE_FN_WAKE: {
		/*
		 * Reschedule another thread for this thread's wake time and clear
		 * its accumulator, which is how it learns it was woken early: a
		 * _sleep that runs to completion leaves 1 there.
		 *
		 * DIVERGENCE: the thread is requeued so the ready list stays
		 * sorted. The original writes the wake time in place and leaves
		 * the node where it was, which breaks the sort the scheduler
		 * relies on - see ../docs/known-defects.txt.
		 */
		core_sched_thread *t = find_thread(s, a1);

		if (t != 0) {
			t->wake_time = s->current->wake_time;
			t->saved_acc = 0;
			list_unlink(t);
			s->thread_count--;
			core_sched_queue(s, t);
		}
		return CORE_SCRIPT_RUNNING;
	}

	case CORE_FN_KILL: {
		core_sched_thread *t;

		if (s->current->thread_id == a1) {
			return CORE_SCRIPT_TERMINATED;   /* killing yourself is _exit */
		}
		t = find_thread(s, a1);
		if (t != 0) {
			list_unlink(t);
			/* DIVERGENCE: the count is decremented. The original does not,
			 * so every kill permanently inflates it and thirty fork-and-kill
			 * pairs disable _fork for good. See ../docs/known-defects.txt. */
			s->thread_count--;
			thread_release(s, t);
		}
		return CORE_SCRIPT_RUNNING;
	}

	case CORE_FN_FORK:
		return native_fork(s, vm);

	case CORE_FN_GETPID:
		vm->acc = s->current->thread_id;
		return CORE_SCRIPT_RUNNING;

	case CORE_FN_STICK_SWAP:
	case CORE_FN_SET_RUMBLE:
		/*
		 * ACCEPTED AND IGNORED, deliberately. Both get their own case in
		 * the original and both do nothing - no return value, no status.
		 * That is distinct from an unknown id, which faults with status 8.
		 * The compiler accepts a call to either and the driver discards it
		 * without complaint. _set_rumble reads as force feedback that moved
		 * out to the separate rumble driver.
		 */
		return CORE_SCRIPT_RUNNING;

	default:
		return CORE_SCRIPT_BAD_NATIVE;
	}
}

/*
 * INPUT BINDING - what creates threads in the first place.
 *
 * Sixteen globals at the bottom of the variable array, plus four above them,
 * hold tagged code addresses. When the controller state changes, each one
 * that is bound gets a thread, with its arguments already on the thread's
 * stack, and a pass is run. drv_ScriptDispatchInput (00017550) is the
 * original; drv_BuildJoystickReport calls it once per accepted packet.
 *
 *   slot        fires when            arguments pushed
 *   ---------   -------------------   --------------------------------
 *   0x00..0x0F  that button changed   1: the button's new state, 0 or 1
 *   0x10        any button changed    2: cur button word, prev, signed
 *   0x11        the stick moved       4: cur X, cur Y, prev X, prev Y
 *   0x12        anything changed      2: cur packed dword, prev
 *   0x13        anything changed      2: cur packed dword, prev
 *
 * 0x12 AND 0x13 HAVE THE SAME TRIGGER AND THE SAME ARGUMENTS. What separates
 * them is position: 0x12 is queued before every other handler and 0x13 after,
 * and since all of them are queued with the same wake time and the ready list
 * is FIFO among equal wake times, that is also the order they run in. They
 * bracket the pass.
 *
 * THE PACKED REPORT is four bytes - X, Y, and the two button bytes in the
 * order they arrive on the wire:
 *
 *     packed[0] = raw[0]   stick X, signed
 *     packed[1] = raw[1]   stick Y, signed
 *     packed[2] = raw[3]   the first button byte
 *     packed[3] = raw[4]   the second
 *
 * The status byte raw[2] is dropped. Read as little-endian 16-bit values,
 * packed[0..1] is the stick word and packed[2..3] is the button word, and
 * the two are compared against the previous packet to decide what changed.
 *
 * The button word is bit-for-bit the same word core_decode_buttons builds, so
 * SLOT n IS RAW BUTTON INDEX n: slot 0 is bit 15, slot 15 is bit 0. That
 * correspondence is worth stating because it is not obvious from either side
 * on its own - core.c builds (raw[4] << 8) | raw[3] and this reads bytes
 * [2],[3] little-endian, which come to the same value.
 *
 * The original keeps the pair in its device extension, maintained by the
 * report builder. Here the scheduler owns it, because core.c must not know
 * that scripts exist. Same two values, different owner.
 */

static u32 in_dword(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}

static s32 in_buttons(const u8 *p)
{
	return (s32)(s16)((u16)p[2] | ((u16)p[3] << 8));
}

static s32 in_stick(const u8 *p)
{
	return (s32)(s16)((u16)p[0] | ((u16)p[1] << 8));
}

/*
 * Queue one handler thread if that slot holds a tagged code address.
 *
 * DIVERGENCE: the slot is bounds-checked against var_count and the original
 * does not check at all. Vars is allocated with 200 words of slack for the
 * locals, so a script declaring fewer than 20 globals makes the original read
 * an uninitialised local as a handler address - inside its own allocation, so
 * not an overrun, but it can spawn a thread at a junk pc. A compiled script
 * always reserves all 20, so this cannot happen in practice; the check makes
 * that a guarantee rather than a convention.
 */
static void sched_spawn_handler(core_sched *s, s32 slot, s32 stack_size,
                                const u32 *args, s32 argc, u64 now)
{
	core_sched_thread *t;
	u32 target;
	s32 i;

	if (slot < 0 || slot >= s->vm.var_count) {
		return;
	}
	target = s->vm.vars[slot];
	if ((target & CORE_TAG_MASK) != CORE_TAG_CODE) {
		return;         /* unbound, or not a code address */
	}
	t = core_sched_thread_alloc(s, stack_size);
	if (t == 0) {
		return;         /* the original drops the handler too */
	}
	t->pc        = (s32)(target & CORE_TAG_VALUE);
	t->wake_time = now;
	for (i = 0; i < argc && i < t->stack_size; i++) {
		t->stack[i] = args[i];
	}
	core_sched_queue(s, t);
}

int core_sched_on_input(core_sched *s, const u8 *raw, u64 now)
{
	u32 args[4];
	s32 btn_delta, stick_delta, cur_btn;
	s32 n;

	if (s == 0 || raw == 0) {
		return 0;
	}
	if (s->vm.code == 0 || s->vm.code_count == 0) {
		/* No script loaded. The packed pair is still advanced so that the
		 * first packet after a load compares against something real. */
		for (n = 0; n < 4; n++) {
			s->prev_input[n] = s->cur_input[n];
		}
		s->cur_input[0] = raw[CORE_RAW_X];
		s->cur_input[1] = raw[CORE_RAW_Y];
		s->cur_input[2] = raw[CORE_RAW_BUTTONS_HI];
		s->cur_input[3] = raw[CORE_RAW_BUTTONS_LO];
		return 0;
	}

	for (n = 0; n < 4; n++) {
		s->prev_input[n] = s->cur_input[n];
	}
	s->cur_input[0] = raw[CORE_RAW_X];
	s->cur_input[1] = raw[CORE_RAW_Y];
	s->cur_input[2] = raw[CORE_RAW_BUTTONS_HI];
	s->cur_input[3] = raw[CORE_RAW_BUTTONS_LO];

	/*
	 * The original sign-extends both halves before the XOR. That changes
	 * the bits above 15 and nothing else, since every test below is either
	 * against zero or against a mask no wider than 0x8000.
	 */
	btn_delta   = in_buttons(s->cur_input) ^ in_buttons(s->prev_input);
	stick_delta = in_stick(s->cur_input)   ^ in_stick(s->prev_input);
	if (btn_delta == 0 && stick_delta == 0) {
		return 1;       /* nothing changed; no pass, no threads */
	}
	cur_btn = in_buttons(s->cur_input);

	/* 1. the pre handler, before anything specific. */
	args[0] = in_dword(s->cur_input);
	args[1] = in_dword(s->prev_input);
	sched_spawn_handler(s, CORE_SCHED_SLOT_PRE, 2, args, 2, now);

	/* 2. the stick. */
	if (stick_delta != 0) {
		args[0] = (u32)(s32)(s8)s->cur_input[0];
		args[1] = (u32)(s32)(s8)s->cur_input[1];
		args[2] = (u32)(s32)(s8)s->prev_input[0];
		args[3] = (u32)(s32)(s8)s->prev_input[1];
		sched_spawn_handler(s, CORE_SCHED_SLOT_STICK, 4, args, 4, now);
	}

	/* 3. the buttons, aggregate first and then one thread per changed bit. */
	if (btn_delta != 0) {
		args[0] = (u32)cur_btn;
		args[1] = (u32)in_buttons(s->prev_input);
		sched_spawn_handler(s, CORE_SCHED_SLOT_BUTTONS, 2, args, 2, now);

		for (n = 0; n < CORE_RAW_BUTTON_BITS; n++) {
			s32 mask = 0x8000 >> n;      /* slot 0 is bit 15 */

			if ((btn_delta & mask) == 0) {
				continue;
			}
			args[0] = (u32)((cur_btn & mask) != 0);
			sched_spawn_handler(s, n, 1, args, 1, now);
		}
	}

	/* 4. the post handler, after everything else is queued. */
	args[0] = in_dword(s->cur_input);
	args[1] = in_dword(s->prev_input);
	sched_spawn_handler(s, CORE_SCHED_SLOT_POST, 2, args, 2, now);

	core_sched_run(s, now);
	return 1;
}

/* ---- the pass ---------------------------------------------------------- */

void core_sched_run(core_sched *s, u64 now)
{
	if (s == 0 || s->vm.code == 0 || s->vm.vars == 0) {
		return;
	}

	/*
	 * The re-entrancy latch. A caller arriving while a pass is in flight
	 * leaves its timestamp behind and returns, so the scheduler never
	 * recurses and never blocks the input path that called it.
	 */
	if (s->running) {
		s->pending_time  = now;
		s->pending_valid = 1;
		return;
	}
	s->running = 1;

	for (;;) {
		s64 elapsed;

		/* Credit the bucket with the real time since the last pass. */
		elapsed = (s64)(now - s->sched_time) / CORE_SCHED_TICK_100NS;
		if (elapsed < 0) {
			elapsed = 0;
		}
		if (elapsed > CORE_SCRIPT_BUDGET) {
			elapsed = CORE_SCRIPT_BUDGET;
		}
		s->budget += (s32)elapsed;
		if (s->budget > CORE_SCRIPT_BUDGET) {
			s->budget = CORE_SCRIPT_BUDGET;
		}
		s->sched_time = now;

		/*
		 * Run every thread that is due. The list is sorted, so the first
		 * thread that is not due ends the sweep. A running thread is on no
		 * list at all: it is unlinked before it runs and only re-linked by
		 * its disposition below.
		 */
		while (!list_empty(&s->ready)) {
			core_sched_thread *t = s->ready.flink;
			int status;

			if (now < t->wake_time) {
				break;
			}
			list_unlink(t);
			s->thread_count--;

			status = sched_execute(s, &t);

			if (status == CORE_SCRIPT_SLEEPING) {
				/* The native that slept set the new wake time. */
				core_sched_queue(s, t);
			} else if (status == CORE_SCRIPT_TERMINATED) {
				thread_release(s, t);
			} else if (status != CORE_SCRIPT_NO_MEMORY) {
				sched_fault(s, t, status);
			}
			/* CORE_SCRIPT_NO_MEMORY: the node is already gone. */
		}

		sched_drain_events(s);

		if (s->pending_valid) {
			now = s->pending_time;
			s->pending_valid = 0;
			s->pending_time  = 0;
			continue;
		}

		s->running = 0;
		if (!list_empty(&s->ready) && s->arm != 0) {
			s->arm(s->arm_ctx, s->ready.flink->wake_time);
		}
		return;
	}
}
