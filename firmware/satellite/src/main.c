/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <stdbool.h>

#include "satellite.h"

LOG_MODULE_REGISTER(satellite, LOG_LEVEL_INF);

int main(void)
{
	uint8_t frame[64];
	const int32_t leak_threshold_ma = 10;
	int ret;

	ret = settings_subsys_init();
	if (ret) {
		LOG_ERR("settings init: %d", ret);
		return ret;
	}

	ret = keys_load();
	if (ret) {
		LOG_WRN("no session key provisioned (%d), link stays closed", ret);
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

	ret = link_init();
	if (ret) {
		LOG_ERR("link init: %d", ret);
		return ret;
	}

	LOG_INF("satellite up, %d channels", SAT_CHANNELS);

	while (true) {
		size_t len = sizeof(frame);

		ret = link_recv(frame, len, 1000);
		if (ret <= 0) {
			continue;
		}

		uint8_t plain[64];
		size_t plain_len = sizeof(plain);

		if (keys_unseal(frame, ret, plain, &plain_len) != 0) {
			LOG_WRN("dropped unauthenticated frame");
			continue;
		}

		/* Command dispatch lives here; the real protocol is the
		 * console's, and is not part of the size experiment.
		 */
		LOG_DBG("frame accepted, %u bytes", (unsigned)plain_len);

		int32_t rail_mv, rail_ma;

		if (rail_read(&rail_mv, &rail_ma) == 0 && rail_ma > leak_threshold_ma) {
			LOG_ERR("rail leaking %d mA with no channel firing", rail_ma);
			for (int ch = 0; ch < SAT_CHANNELS; ch++) {
				channels_arm(ch, false);
			}
		}
	}

	return 0;
}
