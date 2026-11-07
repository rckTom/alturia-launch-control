/* SPDX-License-Identifier: GPL-3.0-only */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <stdbool.h>

#include "dfu_guard.h"
#include "ignlog.h"
#include "radio.h"
#include "sat_link.h"
#include "satellite.h"

LOG_MODULE_REGISTER(satellite, LOG_LEVEL_INF);

/* When a poll addressed to us last arrived (k_uptime_get_32); the console's heartbeat. */
static atomic_t last_poll_ms;

static void disarm_all(const char *why)
{
	commands_disarm(why);
}

/* mcumgr uploads and resets (common/mgmt/dfu_guard.c): only while SAFE. */
bool app_dfu_allowed(void)
{
	return channels_all_safe() && !arm_relay_on();
}

/* After this long in the main loop, a newly updated image is kept. */
#define DFU_CONFIRM_MS 5000

/* Runs on the link thread. */
static void on_link_event(const struct lsec_event *ev)
{
	switch (ev->type) {
	case LSEC_EV_SAS:
		pair_ui_show(ev->sas);
		break;
	case LSEC_EV_PAIRED:
		pair_ui_show(NULL);
		LOG_INF("paired as slot %u", ev->slot);
		break;
	case LSEC_EV_PAIR_FAILED:
		pair_ui_show(NULL);
		LOG_WRN("pairing failed (%d)", ev->reason);
		break;
	case LSEC_EV_JOINED:
		LOG_INF("session with the console up");
		break;
	case LSEC_EV_POLL:
		/*
		 * The console polls us: it is alive and counts us in. The
		 * poll carries our command; the status is the reply.
		 */
		atomic_set(&last_poll_ms, (atomic_val_t)k_uptime_get_32());
		commands_on_poll(ev);
		break;
	case LSEC_EV_LEFT:
		disarm_all("session with the console dropped");
		break;
	case LSEC_EV_DATA:
		/* Commands come in polls (commands.c); nothing else is sent
		 * this way yet. Only authenticated, fresh frames from our
		 * console get this far.
		 */
		LOG_DBG("%s frame, %u bytes", ev->group ? "group" : "unicast", (unsigned)ev->len);
		break;
	default:
		break;
	}
}

int main(void)
{
	int ret;

	ret = settings_subsys_init();
	if (ret) {
		LOG_ERR("settings init: %d", ret);
		chime_error();
		dfu_image_bad();
		return ret;
	}

	ret = arm_init();
	if (ret) {
		LOG_ERR("arm path init: %d", ret);
		chime_error();
		dfu_image_bad();
		return ret;
	}

	ret = channels_init();
	if (ret) {
		LOG_ERR("channels init: %d", ret);
		chime_error();
		dfu_image_bad();
		return ret;
	}

	ret = ignlog_init();
	if (ret) {
		/* Not fatal: the log is for diagnostics only. */
		LOG_ERR("ignition log init: %d", ret);
	}

	ret = pair_ui_init();
	if (ret) {
		LOG_ERR("pairing UI init: %d", ret);
		chime_error();
		dfu_image_bad();
		return ret;
	}

	/*
	 * The Wio-SX1262 switches its own RF path: DIO2 selects TX and the
	 * driver powers the switch from RF_SW (antenna-enable-gpios). No
	 * front-end.
	 */
	ret = radio_init(NULL);
	if (ret) {
		LOG_ERR("radio init: %d", ret);
		chime_error();
		dfu_image_bad();
		return ret;
	}

	ret = sat_link_init(on_link_event, &link_radio_transport);
	if (ret) {
		LOG_ERR("link init: %d", ret);
		chime_error();
		dfu_image_bad();
		return ret;
	}

	LOG_INF("satellite up, %d channels", SAT_CHANNELS);
	chime_startup();

	bool image_ok = false;

	while (true) {
		k_sleep(K_MSEC(100));

		if (!image_ok && k_uptime_get() > DFU_CONFIRM_MS) {
			dfu_image_ok();
			image_ok = true;
		}

		commands_measure();

		/* The console's heartbeat is being polled; without it, SAFE. */
		if ((uint32_t)(k_uptime_get_32() - (uint32_t)atomic_get(&last_poll_ms)) >
		    LINK_SAT_SAFE_TIMEOUT_MS) {
			disarm_all("the console stopped polling");
		}
	}

	return 0;
}
