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

#include "chime.h"
#include "controller.h"
#include "dfu_guard.h"
#include "link.h"
#include "pads.h"
#include "ui.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define CTRL_PERIOD_MS 50

/* After this long with the UI running, a newly updated image is kept. */
#define DFU_CONFIRM_MS 10000

/* mcumgr uploads and resets (common/mgmt/dfu_guard.c): only while SAFE. */
bool app_dfu_allowed(void)
{
	return ctrl_view()->state == CTRL_SAFE;
}

static void dfu_confirm_cb(lv_timer_t *timer)
{
	ARG_UNUSED(timer);
	dfu_image_ok();
}

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
		chime_error();
		dfu_image_bad();
		return 0;
	}

	/* The splash first, while the rest starts up */
	ui_init();
	lv_timer_handler();
	display_blanking_off(display);

	(void)pads_init();
	/* After pads_init(), which brings up the settings subsystem */
	if (link_init()) {
		LOG_ERR("secure link failed to start");
		chime_error();
		dfu_image_bad();
	} else {
		chime_startup();
	}
	lv_timer_create(ctrl_timer_cb, CTRL_PERIOD_MS, NULL);
	lv_timer_set_repeat_count(lv_timer_create(dfu_confirm_cb, DFU_CONFIRM_MS, NULL), 1);

	LOG_INF("launch controller UI running");

	/*
	 * Sleep until LVGL's next timer: the controller tick, or a redraw.
	 * Requests posted in between wait for the tick anyway, so waking more
	 * often would only cost battery.
	 */
	while (true) {
		uint32_t sleep_ms = lv_timer_handler();

		k_msleep(MIN(sleep_ms, CTRL_PERIOD_MS));
	}
	return 0;
}
