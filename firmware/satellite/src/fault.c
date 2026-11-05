/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * A fatal error (stack guard hit, hard fault, failed assert) reboots the
 * satellite instead of halting it. A halted satellite leaves its output pins
 * where they were, and its console with it: the log only goes out over USB,
 * so the fault dump never reaches anyone. The reason, the thread and the PC
 * survive the reboot in RAM; `sat fault` shows them, and the boot log too.
 */

#include <string.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#include "satellite.h"

LOG_MODULE_REGISTER(fault, LOG_LEVEL_INF);

#define FAULT_MAGIC 0x46415554UL

struct fault_record {
	uint32_t magic;
	uint32_t reason;
	uint32_t pc;
	char thread[16];
	uint32_t check;
};

static __noinit struct fault_record noinit_fault;
/* The record from before this boot, if there was one. */
static struct fault_record last_fault;

static uint32_t record_check(const struct fault_record *r)
{
	uint32_t c = r->magic ^ r->reason ^ r->pc;

	for (size_t i = 0; i < sizeof(r->thread); i++) {
		c = (c << 5 | c >> 27) ^ (uint8_t)r->thread[i];
	}

	return c;
}

#ifndef CONFIG_ARCH_POSIX
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	const char *name = k_thread_name_get(k_current_get());

	noinit_fault.magic = FAULT_MAGIC;
	noinit_fault.reason = reason;
#ifdef CONFIG_ARM
	noinit_fault.pc = esf ? esf->basic.pc : 0;
#else
	noinit_fault.pc = 0;
#endif
	memset(noinit_fault.thread, 0, sizeof(noinit_fault.thread));
	strncpy(noinit_fault.thread, name ? name : "?", sizeof(noinit_fault.thread) - 1);
	noinit_fault.check = record_check(&noinit_fault);

	LOG_PANIC();
	sys_reboot(SYS_REBOOT_COLD);
}
#endif

static const char *reason_name(uint32_t reason)
{
	switch (reason) {
	case K_ERR_CPU_EXCEPTION:
		return "CPU exception";
	case K_ERR_SPURIOUS_IRQ:
		return "spurious IRQ";
	case K_ERR_STACK_CHK_FAIL:
		return "stack overflow";
	case K_ERR_KERNEL_OOPS:
		return "kernel oops";
	case K_ERR_KERNEL_PANIC:
		return "kernel panic";
	default:
		return "fatal error";
	}
}

static int fault_init(void)
{
	if (noinit_fault.magic == FAULT_MAGIC && noinit_fault.check == record_check(&noinit_fault)) {
		last_fault = noinit_fault;
		LOG_ERR("rebooted after %s (%u) in thread %s, pc 0x%08x",
			reason_name(last_fault.reason), last_fault.reason, last_fault.thread,
			last_fault.pc);
	}
	memset(&noinit_fault, 0, sizeof(noinit_fault));

	return 0;
}

SYS_INIT(fault_init, APPLICATION, 0);

void fault_print(const struct shell *sh)
{
	if (last_fault.magic != FAULT_MAGIC) {
		shell_print(sh, "no fault before this boot");
		return;
	}

	shell_print(sh, "%s (%u) in thread %s, pc 0x%08x", reason_name(last_fault.reason),
		    last_fault.reason, last_fault.thread, last_fault.pc);
}
