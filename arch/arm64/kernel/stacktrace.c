// SPDX-License-Identifier: GPL-2.0-only
/*
 * Stack tracing support
 *
 * Copyright (C) 2012 ARM Ltd.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/ftrace.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/task_stack.h>
#include <linux/stacktrace.h>

#include <asm/irq.h>
#include <asm/pointer_auth.h>
#include <asm/stack_pointer.h>
#include <asm/stacktrace.h>

/*
 * AArch64 PCS assigns the frame pointer to x29.
 *
 * A simple function prologue looks like this:
 * 	sub	sp, sp, #0x10
 *   	stp	x29, x30, [sp]
 *	mov	x29, sp
 *
 * A simple function epilogue looks like this:
 *	mov	sp, x29
 *	ldp	x29, x30, [sp]
 *	add	sp, sp, #0x10
 */


static void unwind_init_common(struct unwind_state *state,
			       struct task_struct *task)
{
	state->task = task;
#ifdef CONFIG_KRETPROBES
	state->kr_cur = NULL;
#endif

	/*
	 * Prime the first unwind.
	 *
	 * In unwind_next() we'll check that the FP points to a valid stack,
	 * which can't be STACK_TYPE_UNKNOWN, and the first unwind will be
	 * treated as a transition to whichever stack that happens to be. The
	 * prev_fp value won't be used, but we set it to 0 such that it is
	 * definitely not an accessible stack address.
	 */
	bitmap_zero(state->stacks_done, __NR_STACK_TYPES);
	state->prev_fp = 0;
	state->prev_type = STACK_TYPE_UNKNOWN;
}
NOKPROBE_SYMBOL(unwind_init_common);

/*
 * TODO: document requirements here.
 */
static inline void unwind_init_from_regs(struct unwind_state *state,
					 struct task_struct *task,
					 struct pt_regs *regs)
{
	unwind_init_common(state, task);

	state->fp = regs->regs[29];
	state->pc = regs->pc;
}

/*
 * TODO: document requirements here.
 *
 * Note: this is always inlined, and we expect our caller to be a noinline
 * function, such that this starts from our caller's caller.
 */
static __always_inline void unwind_init_from_current(struct unwind_state *state,
						     struct task_struct *task)
{
	unwind_init_common(state, task);

	state->fp = (unsigned long)__builtin_frame_address(1);
	state->pc = (unsigned long)__builtin_return_address(0);
}

/*
 * TODO: document requirements here.
 *
 * The caller guarantees that the task is not running.
 */
static inline void unwind_init_from_task(struct unwind_state *state,
					 struct task_struct *task)
{
	unwind_init_common(state, task);

	state->fp = thread_saved_fp(task);
	state->pc = thread_saved_pc(task);
}

/*
 * Unwind from one frame record (A) to the next frame record (B).
 *
 * We terminate early if the location of B indicates a malformed chain of frame
 * records (e.g. a cycle), determined based on the location and fp value of A
 * and the location (but not the fp value) of B.
 */
static int notrace unwind_next(struct unwind_state *state)
{
	unsigned long fp = state->fp;
	struct stack_info info;
	struct task_struct *tsk = state->task;

	/* Final frame; nothing to unwind */
	if (fp == (unsigned long)task_pt_regs(tsk)->stackframe)
		return -ENOENT;

	if (fp & 0x7)
		return -EINVAL;

	if (!on_accessible_stack(tsk, fp, 16, &info))
		return -EINVAL;

	if (test_bit(info.type, state->stacks_done))
		return -EINVAL;

	/*
	 * As stacks grow downward, any valid record on the same stack must be
	 * at a strictly higher address than the prior record.
	 *
	 * Stacks can nest in several valid orders, e.g.
	 *
	 * TASK -> IRQ -> OVERFLOW -> SDEI_NORMAL
	 * TASK -> SDEI_NORMAL -> SDEI_CRITICAL -> OVERFLOW
	 *
	 * ... but the nesting itself is strict. Once we transition from one
	 * stack to another, it's never valid to unwind back to that first
	 * stack.
	 */
	if (info.type == state->prev_type) {
		if (fp <= state->prev_fp)
			return -EINVAL;
	} else {
		set_bit(state->prev_type, state->stacks_done);
	}

	/*
	 * Record this frame record's values and location. The prev_fp and
	 * prev_type are only meaningful to the next unwind_next() invocation.
	 */
	state->fp = READ_ONCE_NOCHECK(*(unsigned long *)(fp));
	state->pc = READ_ONCE_NOCHECK(*(unsigned long *)(fp + 8));
	state->prev_fp = fp;
	state->prev_type = info.type;

	state->pc = ptrauth_strip_insn_pac(state->pc);

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
	if (tsk->ret_stack &&
		(state->pc == (unsigned long)return_to_handler)) {
		unsigned long orig_pc;
		/*
		 * This is a case where function graph tracer has
		 * modified a return address (LR) in a stack frame
		 * to hook a function return.
		 * So replace it to an original value.
		 */
		orig_pc = ftrace_graph_ret_addr(tsk, NULL, state->pc,
						(void *)state->fp);
		if (WARN_ON_ONCE(state->pc == orig_pc))
			return -EINVAL;
		state->pc = orig_pc;
	}
#endif /* CONFIG_FUNCTION_GRAPH_TRACER */
#ifdef CONFIG_KRETPROBES
	if (is_kretprobe_trampoline(state->pc))
		state->pc = kretprobe_find_ret_addr(tsk, (void *)state->fp, &state->kr_cur);
#endif

	return 0;
}
NOKPROBE_SYMBOL(unwind_next);

static void notrace unwind(struct unwind_state *state,
			   stack_trace_consume_fn consume_entry, void *cookie)
{
	while (1) {
		int ret;

		if (!consume_entry(cookie, state->pc))
			break;
		ret = unwind_next(state);
		if (ret < 0)
			break;
	}
}
NOKPROBE_SYMBOL(unwind);

static bool dump_backtrace_entry(void *arg, unsigned long where)
{
	char *loglvl = arg;
	printk("%s %pSb\n", loglvl, (void *)where);
	return true;
}

void dump_backtrace(struct pt_regs *regs, struct task_struct *tsk,
		    const char *loglvl)
{
	pr_debug("%s(regs = %p tsk = %p)\n", __func__, regs, tsk);

	if (regs && user_mode(regs))
		return;

	if (!tsk)
		tsk = current;

	if (!try_get_task_stack(tsk))
		return;

	printk("%sCall trace:\n", loglvl);
	arch_stack_walk(dump_backtrace_entry, (void *)loglvl, tsk, regs);

	put_task_stack(tsk);
}

void show_stack(struct task_struct *tsk, unsigned long *sp, const char *loglvl)
{
	dump_backtrace(NULL, tsk, loglvl);
	barrier();
}

noinline notrace void arch_stack_walk(stack_trace_consume_fn consume_entry,
			      void *cookie, struct task_struct *task,
			      struct pt_regs *regs)
{
	struct unwind_state state;

	if (regs)
		unwind_init_from_regs(&state, task, regs);
	else if (task == current)
		unwind_init_from_current(&state, task);
	else
		unwind_init_from_task(&state, task);

	unwind(&state, consume_entry, cookie);
}
