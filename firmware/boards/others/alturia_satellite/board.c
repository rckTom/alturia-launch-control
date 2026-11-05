/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The satellite has no HSE crystal, so USB full-speed runs from HSI48. That
 * is only inside USB's +-0.25 % clock tolerance when the clock recovery
 * system trims HSI48 against the host's start-of-frame packets, and Zephyr's
 * STM32G4 clock driver never enables CRS. Turn it on here.
 *
 * The board also enters the ROM USB DFU bootloader without BOOT0: the `dfu`
 * shell command, or opening the CDC ACM port at 1200 baud and closing it
 * (`stty -F /dev/ttyACM0 1200`), marks a request in RAM that survives a
 * reset and reboots. The early init hook then finds the mark and jumps to
 * system memory while the chip still has its reset clocks and no driver has
 * touched a peripheral, which is the state the bootloader expects.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/sys_io.h>
#include <cmsis_core.h>
#include <stm32_ll_bus.h>
#include <stm32_ll_crs.h>
#include <stm32_ll_system.h>

#define SYSMEM_BASE 0x1FFF0000UL

/* Two words, so RAM noise at power-on cannot pass for a request. */
#define DFU_MAGIC 0x44465521UL

static __noinit uint32_t dfu_request[2];

void board_early_init_hook(void)
{
	if (dfu_request[0] != DFU_MAGIC || dfu_request[1] != ~DFU_MAGIC) {
		return;
	}
	dfu_request[0] = 0;
	dfu_request[1] = 0;

	/* arch_kernel_init() has already turned the MPU on. */
	MPU->CTRL = 0;
	__DSB();
	__ISB();

	LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_SYSCFG);
	LL_SYSCFG_SetRemapMemory(LL_SYSCFG_REMAP_SYSTEMFLASH);
	SCB->VTOR = SYSMEM_BASE;

	uint32_t sp = sys_read32(SYSMEM_BASE);
	uint32_t pc = sys_read32(SYSMEM_BASE + 4);

	/*
	 * Back to the reset state of the core: MSP in thread mode, nothing
	 * masked. Zephyr boots on the PSP with BASEPRI raised, and this has
	 * to be one asm block because the stack changes under it.
	 */
	__asm volatile(
		"msr basepri, %[zero]\n"
		"msr msp, %[sp]\n"
		"msr control, %[zero]\n"
		"isb\n"
		"cpsie i\n"
		"bx %[pc]\n"
		:
		: [sp] "r"(sp), [pc] "r"(pc), [zero] "r"(0)
		: "memory");
	CODE_UNREACHABLE;
}

static void board_dfu_reboot(void)
{
	dfu_request[0] = DFU_MAGIC;
	dfu_request[1] = ~DFU_MAGIC;
	sys_reboot(SYS_REBOOT_COLD);
}

#if DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(board_cdc_acm_uart)) && defined(CONFIG_UART_LINE_CTRL)
/*
 * The 1200-baud touch: the host sets 1200 baud and closes the port, which
 * drops DTR. Polled, because the line coding only reaches the class driver
 * and the shell owns the UART callbacks.
 */
static const struct device *const cdc_uart = DEVICE_DT_GET(DT_NODELABEL(board_cdc_acm_uart));

static void touch_poll(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(touch_work, touch_poll);

static void touch_poll(struct k_work *work)
{
	uint32_t baud, dtr;

	if (uart_line_ctrl_get(cdc_uart, UART_LINE_CTRL_BAUD_RATE, &baud) == 0 && baud == 1200 &&
	    uart_line_ctrl_get(cdc_uart, UART_LINE_CTRL_DTR, &dtr) == 0 && dtr == 0) {
		board_dfu_reboot();
	}

	k_work_schedule(&touch_work, K_MSEC(100));
}

static int board_touch_init(void)
{
	if (device_is_ready(cdc_uart)) {
		k_work_schedule(&touch_work, K_MSEC(100));
	}

	return 0;
}

SYS_INIT(board_touch_init, APPLICATION, 99);
#endif

#ifdef CONFIG_SHELL
static int cmd_dfu(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Rebooting into the USB DFU bootloader (0483:df11)");
	/* Let the line out over CDC ACM before USB goes away. */
	k_sleep(K_MSEC(300));
	board_dfu_reboot();

	return 0;
}

SHELL_CMD_REGISTER(dfu, NULL, "reboot into the ROM USB DFU bootloader", cmd_dfu);
#endif

static int board_crs_init(void)
{
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_CRS);
	LL_CRS_SetSyncSignalSource(LL_CRS_SYNC_SOURCE_USB);
	LL_CRS_EnableAutoTrimming();
	LL_CRS_EnableFreqErrorCounter();

	return 0;
}

SYS_INIT(board_crs_init, PRE_KERNEL_2, 0);
