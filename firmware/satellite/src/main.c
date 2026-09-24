/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <stdbool.h>

#include "radio.h"
#include "sat_link.h"
#include "satellite.h"

LOG_MODULE_REGISTER(satellite, LOG_LEVEL_INF);

/* When a poll addressed to us last arrived (k_uptime_get_32); the console's heartbeat. */
static atomic_t last_poll_ms;

static void disarm_all(const char *why)
{
	if (channels_all_safe()) {
		return;
	}
	LOG_WRN("SAFE: %s", why);
	for (int ch = 0; ch < SAT_CHANNELS; ch++) {
		channels_arm(ch, false);
	}
}

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
		 * The console polls us: it is alive and counts us in. Its
		 * commands will be in ev->data; with no status format yet the
		 * link answers with an empty frame, which keeps the session.
		 */
		atomic_set(&last_poll_ms, (atomic_val_t)k_uptime_get_32());
		break;
	case LSEC_EV_LEFT:
		disarm_all("session with the console dropped");
		break;
	case LSEC_EV_DATA:
		/* Command dispatch lives here; the real protocol is the
		 * console's, and is not part of the size experiment. Only
		 * authenticated, fresh frames from our console get this far.
		 */
		LOG_DBG("%s frame, %u bytes", ev->group ? "group" : "unicast", (unsigned)ev->len);
		break;
	default:
		break;
	}
}

int main(void)
{
	const int32_t leak_threshold_ma = 10;
	int ret;

	ret = settings_subsys_init();
	if (ret) {
		LOG_ERR("settings init: %d", ret);
		return ret;
	}

	ret = channels_init();
	if (ret) {
		LOG_ERR("channels init: %d", ret);
		return ret;
	}

	ret = rail_init();
	if (ret) {
		LOG_ERR("rail monitor init: %d", ret);
		return ret;
	}

	ret = pair_ui_init();
	if (ret) {
		LOG_ERR("pairing UI init: %d", ret);
		return ret;
	}

	/* The RA-01SH switches its own RF path from DIO2: no front-end. */
	ret = radio_init(NULL);
	if (ret) {
		LOG_ERR("radio init: %d", ret);
		return ret;
	}

	ret = sat_link_init(on_link_event, &link_radio_transport);
	if (ret) {
		LOG_ERR("link init: %d", ret);
		return ret;
	}

	LOG_INF("satellite up, %d channels", SAT_CHANNELS);

	while (true) {
		int32_t rail_mv, rail_ma;

		k_sleep(K_MSEC(100));

		if (rail_read(&rail_mv, &rail_ma) == 0 && rail_ma > leak_threshold_ma) {
			LOG_ERR("rail leaking %d mA with no channel firing", rail_ma);
			for (int ch = 0; ch < SAT_CHANNELS; ch++) {
				channels_arm(ch, false);
			}
		}

		/* The console's heartbeat is being polled; without it, SAFE. */
		if ((uint32_t)(k_uptime_get_32() - (uint32_t)atomic_get(&last_poll_ms)) >
		    LINK_SAT_SAFE_TIMEOUT_MS) {
			disarm_all("the console stopped polling");
		}
	}

	return 0;
}
