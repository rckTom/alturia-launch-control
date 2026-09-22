/*
 * Launch controller UI test bench.
 *
 * LVGL runs in the main thread. The controller state machine is ticked
 * from an LVGL timer in the same thread, so UI and controller never race.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

#include "controller.h"
#include "pads.h"
#include "ui.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define CTRL_PERIOD_MS 50

static void ctrl_timer_cb(lv_timer_t *timer)
{
	ARG_UNUSED(timer);
	ctrl_tick();
	ui_refresh();
}

int main(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(display)) {
		LOG_ERR("display not ready");
		return 0;
	}

	(void)pads_init();
	ui_init();
	lv_timer_create(ctrl_timer_cb, CTRL_PERIOD_MS, NULL);
	display_blanking_off(display);

	LOG_INF("launch controller UI running");

	while (true) {
		uint32_t sleep_ms = lv_timer_handler();

		k_msleep(MIN(sleep_ms, 10U));
	}
	return 0;
}
