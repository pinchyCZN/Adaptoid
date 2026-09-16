/*
 * script.h - the script bytecode interpreter for wishk300.
 *
 * The Adaptoid's scripting language compiles to a bytecode that the DRIVER
 * executes, which is how a script can turn a controller button into a real
 * keystroke: the interpreter runs below the application layer, so what it
 * produces is genuine HID input rather than injected events.
 *
 * A stack machine with a register accumulator. Instruction set, machine
 * state and status codes are specified in ../docs/script-bytecode.txt
 * sections 5 and 6, which were read out of drv_ScriptExecute and confirmed
 * against the configurator's own disassembler table.
 *
 * Like core.c this file is OS-free: no Windows headers, no kernel calls.
 */
#ifndef ADAPTOID_SCRIPT_H
#define ADAPTOID_SCRIPT_H

#include "core.h"

/*
 * MEMORY MODEL.
 *
 * One flat array holds both the globals and the 200-entry region above them
 * that serves as BOTH the operand stack and the script locals. That sharing
 * is not an implementation choice here - drv_ScriptCheckVarIndex resolves a
 * local as Vars[VarCount + index], and the stack base is the same address.
 *
 * A consequence worth knowing: the 200 local slots are PER DEVICE, not per
 * thread, so two threads running the same function share its locals.
 */
#define CORE_SCRIPT_LOCALS      200

/* The runaway guard: one quantum may run this many instructions. */
#define CORE_SCRIPT_BUDGET      100000

/* Status codes, ../docs/script-bytecode.txt section 6.4. */
#define CORE_SCRIPT_RUNNING     0
#define CORE_SCRIPT_TERMINATED  1
#define CORE_SCRIPT_BAD_VAR     2
#define CORE_SCRIPT_UNDERFLOW   3
#define CORE_SCRIPT_OVERFLOW    4
#define CORE_SCRIPT_DIV_ZERO    5
#define CORE_SCRIPT_RANGE       6
#define CORE_SCRIPT_ILLEGAL     7
#define CORE_SCRIPT_BAD_NATIVE  8
#define CORE_SCRIPT_SLEEPING    9
#define CORE_SCRIPT_BUDGET_OUT  10
/*
 * 11 is not produced by the interpreter. drv_ScriptExecute uses it for a
 * failed reallocation of the thread node, where the node has already been
 * freed, so the scheduler must not touch the thread again. It is the one
 * status that is not a script fault.
 */
#define CORE_SCRIPT_NO_MEMORY   11

/*
 * Pointer tags. A value that denotes an address carries one in its high
 * nibble, and the interpreter dispatches on it.
 */
#define CORE_TAG_MASK           0xF0000000u
#define CORE_TAG_VALUE          0x0FFFFFFFu
#define CORE_TAG_CODE           0x10000000u   /* a script function      */
#define CORE_TAG_GLOBAL         0x20000000u   /* global variable slot   */
#define CORE_TAG_LOCAL          0x30000000u   /* stack local            */
#define CORE_TAG_NATIVE         0x40000000u   /* native builtin id      */

/* Opcodes. Bit 0x100 means one operand word follows. */
#define CORE_OP_OPERAND         0x100u

#define CORE_OP_MEM             0x020   /* acc = vars[acc]              */
#define CORE_OP_MEMPP           0x021   /* post-increment               */
#define CORE_OP_MEMMM           0x022   /* post-decrement               */
#define CORE_OP_PPMEM           0x023   /* pre-increment                */
#define CORE_OP_MMMEM           0x024   /* pre-decrement                */
#define CORE_OP_NOT             0x040
#define CORE_OP_BNOT            0x041
#define CORE_OP_NEG             0x042
#define CORE_OP_CALLA           0x080   /* call through acc             */
#define CORE_OP_RET             0x082
#define CORE_OP_PUSH            0x093
#define CORE_OP_SWAP            0x095
#define CORE_OP_LOAD            0x110   /* acc = immediate              */
#define CORE_OP_B               0x170
#define CORE_OP_BT              0x171
#define CORE_OP_BF              0x172
#define CORE_OP_CALL            0x181   /* call immediate               */
#define CORE_OP_STACK           0x190   /* stack pointer += operand     */
#define CORE_OP_BAIL            0x191   /* the unwind form of the above */
#define CORE_OP_LOC             0x192   /* acc = address of a local     */
#define CORE_OP_STORS           0x230
#define CORE_OP_STORA           0x231
#define CORE_OP_LSHFT           0x250
#define CORE_OP_RSHFT           0x251
#define CORE_OP_ADD             0x252
#define CORE_OP_SUB             0x253
#define CORE_OP_MUL             0x254
#define CORE_OP_DIV             0x255
#define CORE_OP_MOD             0x256
#define CORE_OP_BAND            0x257
#define CORE_OP_BOR             0x258
#define CORE_OP_BXOR            0x259
#define CORE_OP_EQ              0x260
#define CORE_OP_NE              0x261
#define CORE_OP_GT              0x262
#define CORE_OP_LT              0x263
#define CORE_OP_GE              0x264
#define CORE_OP_LE              0x265
#define CORE_OP_AND             0x266
#define CORE_OP_OR              0x267
#define CORE_OP_POP             0x294

struct core_script;

/*
 * THE NATIVE SEAM. A call whose target carries CORE_TAG_NATIVE lands here;
 * the builtin library is what lets a script press keys and move the stick.
 * Return a status code - CORE_SCRIPT_RUNNING to carry on, CORE_SCRIPT_SLEEPING
 * to yield, CORE_SCRIPT_BAD_NATIVE for an id you do not implement.
 *
 * With no handler installed every native call reports CORE_SCRIPT_BAD_NATIVE,
 * which is the right answer for an interpreter that has no library yet.
 */
typedef int (*core_script_native_fn)(void *ctx, struct core_script *vm,
                                     u32 id);

typedef struct core_script {
	const u32 *code;
	s32        code_count;      /* in words                          */

	u32       *vars;            /* var_count + CORE_SCRIPT_LOCALS    */
	s32        var_count;       /* the globals                       */

	s32        budget;          /* instructions left this quantum    */

	/* Thread state. These are what a scheduler saves and restores. */
	s32        pc;              /* word index into code              */
	u32        acc;
	s32        sp;              /* depth into the local region       */

	core_script_native_fn native;
	void      *native_ctx;
} core_script;

/*
 * Point the machine at some code and storage. vars must have room for
 * var_count + CORE_SCRIPT_LOCALS words; only the globals need initialising.
 */
/*
 * NOT CALLED BY THE DRIVER. core_sched_init initialises the interpreter
 * field by field as part of bringing a scheduler up, so this is the entry
 * point for a caller that wants a bare interpreter with no threads around
 * it - which is exactly what the interpreter's own tests want.
 */
void core_script_init(core_script *vm, const u32 *code, s32 code_count,
                      u32 *vars, s32 var_count);

void core_script_set_native(core_script *vm, core_script_native_fn fn,
                            void *ctx);

/*
 * Run until the machine stops, and return why. Resuming is just calling it
 * again - the state that matters lives in the struct.
 */
int core_script_run(core_script *vm);

/*
 * Resolve a tagged operand to an index into vars, or return 0 to reject it.
 * Exposed because it is the single check that makes an unvalidated var_count
 * safe: nothing indexes vars without passing through here.
 */
int core_script_var_index(u32 *operand, s32 var_count);

#endif /* ADAPTOID_SCRIPT_H */
