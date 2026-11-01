/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The satellite has no HSE crystal, so USB full-speed runs from HSI48. That
 * is only inside USB's +-0.25 % clock tolerance when the clock recovery
 * system trims HSI48 against the host's start-of-frame packets, and Zephyr's
 * STM32G4 clock driver never enables CRS. Turn it on here.
 */

#include <zephyr/init.h>
#include <stm32_ll_bus.h>
#include <stm32_ll_crs.h>

static int board_crs_init(void)
{
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_CRS);
	LL_CRS_SetSyncSignalSource(LL_CRS_SYNC_SOURCE_USB);
	LL_CRS_EnableAutoTrimming();
	LL_CRS_EnableFreqErrorCounter();

	return 0;
}

SYS_INIT(board_crs_init, PRE_KERNEL_2, 0);
