/*
 * script.c - the script bytecode interpreter.
 *
 * See script.h for the machine model. Semantics come from
 * ../docs/script-bytecode.txt sections 5 and 6, which were read out of
 * drv_ScriptExecute at 00017b90 and independently confirmed against the
 * configurator's own 43-entry disassembler table.
 */

#include "script.h"

int core_script_var_index(u32 *operand, s32 var_count)
{
	u32 v;

	if (operand == 0) {
		return 0;
	}
	v = *operand;

	if ((v & CORE_TAG_MASK) == CORE_TAG_GLOBAL) {
		if ((s32)(v & CORE_TAG_VALUE) >= var_count) {
			return 0;
		}
		*operand = v & CORE_TAG_VALUE;
		return 1;
	}
	if ((v & CORE_TAG_MASK) == CORE_TAG_LOCAL) {
		if ((v & CORE_TAG_VALUE) > (u32)(CORE_SCRIPT_LOCALS - 1)) {
			return 0;
		}
		*operand = (v & CORE_TAG_VALUE) + (u32)var_count;
		return 1;
	}
	return 0;                   /* untagged, or a tag that is not a variable */
}

void core_script_init(core_script *vm, const u32 *code, s32 code_count,
                      u32 *vars, s32 var_count)
{
	if (vm == 0) {
		return;
	}
	vm->code       = code;
	vm->code_count = code_count;
	vm->vars       = vars;
	vm->var_count  = var_count;
	vm->budget     = CORE_SCRIPT_BUDGET;
	vm->pc         = 0;
	vm->acc        = 0;
	vm->sp         = 0;
	vm->native     = 0;
	vm->native_ctx = 0;
}

void core_script_set_native(core_script *vm, core_script_native_fn fn,
                            void *ctx)
{
	if (vm != 0) {
		vm->native     = fn;
		vm->native_ctx = ctx;
	}
}

/* The operand stack lives above the globals; slot i is vars[var_count + i]. */
#define STK(vm, i)  ((vm)->vars[(vm)->var_count + (i)])

int core_script_run(core_script *vm)
{
	int status = CORE_SCRIPT_RUNNING;

	if (vm == 0 || vm->code == 0 || vm->vars == 0) {
		return CORE_SCRIPT_RANGE;
	}
	if (vm->pc < 0) {
		return CORE_SCRIPT_RANGE;
	}

	while (status == CORE_SCRIPT_RUNNING) {
		u32 op, operand;
		s32 at;

		/*
		 * The budget is spent BEFORE the instruction runs, and the test
		 * is against 1 rather than 0, so a machine started with a budget
		 * of 1 executes nothing. That is the runaway-script guard and the
		 * reason an infinite loop in a script cannot hang the driver.
		 */
		vm->budget--;
		if (vm->budget < 1) {
			status = CORE_SCRIPT_BUDGET_OUT;
			break;
		}
		if (vm->pc >= vm->code_count) {
			status = CORE_SCRIPT_RANGE;
			break;
		}

		at      = vm->pc;
		op      = vm->code[at];
		operand = 0;
		if ((op & CORE_OP_OPERAND) && at + 1 < vm->code_count) {
			operand = vm->code[at + 1];
		}
		vm->pc = at + ((op & CORE_OP_OPERAND) ? 2 : 1);

		switch (op) {

		/* ---- variables ------------------------------------------- */
		case CORE_OP_MEM:
		case CORE_OP_MEMPP:
		case CORE_OP_MEMMM:
		case CORE_OP_PPMEM:
		case CORE_OP_MMMEM: {
			u32 idx = vm->acc;

			if (!core_script_var_index(&idx, vm->var_count)) {
				status = CORE_SCRIPT_BAD_VAR;
				break;
			}
			switch (op) {
			case CORE_OP_MEM:
				vm->acc = vm->vars[idx];
				break;
			case CORE_OP_MEMPP:
				vm->acc = vm->vars[idx];
				vm->vars[idx] = vm->vars[idx] + 1;
				break;
			case CORE_OP_MEMMM:
				vm->acc = vm->vars[idx];
				vm->vars[idx] = vm->vars[idx] - 1;
				break;
			case CORE_OP_PPMEM:
				vm->vars[idx] = vm->vars[idx] + 1;
				vm->acc = vm->vars[idx];
				break;
			default:
				vm->vars[idx] = vm->vars[idx] - 1;
				vm->acc = vm->vars[idx];
				break;
			}
			break;
		}

		/* ---- unary ----------------------------------------------- */
		case CORE_OP_NOT:  vm->acc = (vm->acc == 0);    break;
		case CORE_OP_BNOT: vm->acc = ~vm->acc;          break;
		case CORE_OP_NEG:  vm->acc = (u32)(-(s32)vm->acc); break;

		/* ---- stack ----------------------------------------------- */
		case CORE_OP_PUSH:
			if (vm->sp >= CORE_SCRIPT_LOCALS) {
				status = CORE_SCRIPT_OVERFLOW;
				break;
			}
			STK(vm, vm->sp) = vm->acc;
			vm->sp++;
			break;

		case CORE_OP_POP:
			if (vm->sp <= 0) {
				status = CORE_SCRIPT_UNDERFLOW;
				break;
			}
			vm->sp--;
			vm->acc = STK(vm, vm->sp);
			break;

		case CORE_OP_SWAP: {
			u32 t;

			if (vm->sp <= 0) {
				status = CORE_SCRIPT_UNDERFLOW;
				break;
			}
			t = STK(vm, vm->sp - 1);
			STK(vm, vm->sp - 1) = vm->acc;
			vm->acc = t;
			break;
		}

		case CORE_OP_STACK:
		case CORE_OP_BAIL:
			/* The accumulator is NOT disturbed by a stack adjust. */
			vm->sp += (s32)operand;
			if (vm->sp > CORE_SCRIPT_LOCALS) {
				status = CORE_SCRIPT_OVERFLOW;
			}
			if (vm->sp < 0) {
				status = CORE_SCRIPT_UNDERFLOW;
			}
			break;

		case CORE_OP_LOC:
			vm->acc = ((u32)vm->sp + operand) | CORE_TAG_LOCAL;
			break;

		/* ---- immediates and branches ----------------------------- */
		case CORE_OP_LOAD:
			vm->acc = operand;
			break;

		case CORE_OP_B:
			vm->pc = at + 2 + (s32)operand;
			if (vm->pc < 0) {
				status = CORE_SCRIPT_RANGE;
			}
			break;

		case CORE_OP_BT:
			if (vm->acc != 0) {
				vm->pc = at + 2 + (s32)operand;
				if (vm->pc < 0) {
					status = CORE_SCRIPT_RANGE;
				}
			}
			break;

		case CORE_OP_BF:
			if (vm->acc == 0) {
				vm->pc = at + 2 + (s32)operand;
				if (vm->pc < 0) {
					status = CORE_SCRIPT_RANGE;
				}
			}
			break;

		/* ---- calls and return ------------------------------------ */
		case CORE_OP_CALLA:
		case CORE_OP_CALL: {
			u32 target = (op == CORE_OP_CALLA) ? vm->acc : operand;
			u32 tag    = target & CORE_TAG_MASK;

			if (tag == 0) {
				/*
				 * An untagged call target does NOTHING in the original -
				 * no jump, no fault, not even a disturbed accumulator.
				 * Reproduced rather than tightened, because a script that
				 * calls an uninitialised variable currently just carries
				 * on and turning that into a fault would be a behaviour
				 * change, not a fix.
				 */
				break;
			}
			if (tag == CORE_TAG_CODE) {
				s32 dest = (s32)(target & CORE_TAG_VALUE);

				/*
				 * DIVERGENCE, and it is a real defect in the original.
				 * For opcode 0x181 the bounds check tests the ACCUMULATOR
				 * against the code length instead of the call target, so
				 * a call can jump clean out of the code array whenever the
				 * accumulator happens to be small. Opcode 0x080 checks the
				 * target correctly. Both check the target here.
				 */
				if (dest >= vm->code_count) {
					status = CORE_SCRIPT_RANGE;
					break;
				}
				if (vm->sp >= CORE_SCRIPT_LOCALS) {
					status = CORE_SCRIPT_OVERFLOW;
					break;
				}
				STK(vm, vm->sp) = (u32)vm->pc;   /* the return address */
				vm->sp++;
				vm->pc = dest;
				break;
			}
			if (tag == CORE_TAG_NATIVE) {
				if (vm->native == 0) {
					status = CORE_SCRIPT_BAD_NATIVE;
					break;
				}
				status = vm->native(vm->native_ctx, vm, target);
				break;
			}
			status = CORE_SCRIPT_RANGE;
			break;
		}

		case CORE_OP_RET:
			/*
			 * Returning with an empty stack TERMINATES the thread. That is
			 * how a script's entry function ends: there is no separate
			 * halt instruction.
			 */
			if (vm->sp <= 0) {
				status = CORE_SCRIPT_TERMINATED;
				break;
			}
			vm->sp--;
			vm->pc = (s32)STK(vm, vm->sp);
			if (vm->pc < 0) {
				status = CORE_SCRIPT_RANGE;
			}
			break;

		/* ---- stores ---------------------------------------------- */
		case CORE_OP_STORS: {
			u32 idx;

			if (vm->sp <= 0) {
				status = CORE_SCRIPT_UNDERFLOW;
				break;
			}
			vm->sp--;
			idx = vm->acc;
			if (!core_script_var_index(&idx, vm->var_count)) {
				status = CORE_SCRIPT_BAD_VAR;
				break;
			}
			vm->vars[idx] = STK(vm, vm->sp);
			break;
		}

		case CORE_OP_STORA: {
			u32 idx;

			if (vm->sp <= 0) {
				status = CORE_SCRIPT_UNDERFLOW;
				break;
			}
			vm->sp--;
			idx = STK(vm, vm->sp);
			if (!core_script_var_index(&idx, vm->var_count)) {
				status = CORE_SCRIPT_BAD_VAR;
				break;
			}
			vm->vars[idx] = vm->acc;
			break;
		}

		/* ---- binary --------------------------------------------- */
		default: {
			u32 lhs;

			/*
			 * Division checks its divisor BEFORE the stack, so dividing by
			 * zero on an empty stack reports the divide rather than the
			 * underflow. Small thing, but it is what the original does.
			 */
			if ((op == CORE_OP_DIV || op == CORE_OP_MOD) && vm->acc == 0) {
				status = CORE_SCRIPT_DIV_ZERO;
				break;
			}
			if (op < CORE_OP_LSHFT || op > CORE_OP_OR) {
				status = CORE_SCRIPT_ILLEGAL;
				break;
			}
			if (vm->sp <= 0) {
				status = CORE_SCRIPT_UNDERFLOW;
				break;
			}
			vm->sp--;
			lhs = STK(vm, vm->sp);

			/*
			 * A ZERO DIVISOR IS NOT THE ONLY OPERAND PAIR A DIVIDE
			 * CANNOT COMPUTE. INT_MIN / -1 has no representable
			 * result - the true quotient is +2147483648, one past
			 * what a signed 32-bit value holds - and x86 reports
			 * that overflow through the SAME #DE trap it uses for
			 * division by zero. MOD TRAPS IDENTICALLY, because one
			 * idiv produces both the quotient and the remainder.
			 *
			 * WITHOUT THIS CHECK IT IS A BUGCHECK, NOT A SCRIPT
			 * FAULT. The interpreter runs from the scheduler DPC at
			 * DISPATCH_LEVEL, where an unhandled #DE is fatal to the
			 * machine. Measured on the live driver from an ordinary
			 * text script: bugcheck 1E, KMODE_EXCEPTION_NOT_HANDLED,
			 * exception c0000095 STATUS_INTEGER_OVERFLOW, at the
			 * idiv this guard protects.
			 *
			 * The operands are compared as bit patterns rather than
			 * as signed values, because forming the constant INT_MIN
			 * by casting is exactly the conversion being guarded.
			 *
			 * It reports DIV_ZERO rather than a status of its own.
			 * The two are one condition from a script's point of
			 * view - a divide whose result does not exist - and the
			 * client already has a string for this one.
			 */
			if ((op == CORE_OP_DIV || op == CORE_OP_MOD) &&
				lhs == 0x80000000u && vm->acc == 0xFFFFFFFFu) {
				status = CORE_SCRIPT_DIV_ZERO;
				break;
			}

			switch (op) {
			case CORE_OP_LSHFT: vm->acc = lhs << (vm->acc & 0x1f);        break;
			case CORE_OP_RSHFT:
				vm->acc = (u32)((s32)lhs >> (vm->acc & 0x1f));
				break;
			case CORE_OP_ADD:   vm->acc = lhs + vm->acc;                  break;
			case CORE_OP_SUB:   vm->acc = lhs - vm->acc;                  break;
			case CORE_OP_MUL:   vm->acc = lhs * vm->acc;                  break;
			case CORE_OP_DIV:   vm->acc = (u32)((s32)lhs / (s32)vm->acc); break;
			case CORE_OP_MOD:   vm->acc = (u32)((s32)lhs % (s32)vm->acc); break;
			case CORE_OP_BAND:  vm->acc = lhs & vm->acc;                  break;
			case CORE_OP_BOR:   vm->acc = lhs | vm->acc;                  break;
			case CORE_OP_BXOR:  vm->acc = lhs ^ vm->acc;                  break;
			case CORE_OP_EQ:    vm->acc = (lhs == vm->acc);               break;
			case CORE_OP_NE:    vm->acc = (lhs != vm->acc);               break;
			case CORE_OP_GT:    vm->acc = ((s32)lhs >  (s32)vm->acc);     break;
			case CORE_OP_LT:    vm->acc = ((s32)lhs <  (s32)vm->acc);     break;
			case CORE_OP_GE:    vm->acc = ((s32)lhs >= (s32)vm->acc);     break;
			case CORE_OP_LE:    vm->acc = ((s32)lhs <= (s32)vm->acc);     break;
			case CORE_OP_AND:   vm->acc = (lhs != 0 && vm->acc != 0);     break;
			default:            vm->acc = (lhs != 0 || vm->acc != 0);     break;
			}
			break;
		}
		}
	}
	return status;
}
