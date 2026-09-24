/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/logging/log.h>

#include "link_access.h"

LOG_MODULE_REGISTER(link_access, LOG_LEVEL_INF);

enum link_lbt_res link_lbt_wait(const struct link_transport *tp, uint32_t max_defer_us,
				bool force, bool predelay, struct link_lbt_stats *stats)
{
	struct link_lbt l;
	int64_t at;

	if (LINK_LBT_LISTEN_US == 0 || !tp->channel_clear) {
		return LINK_LBT_SEND;
	}

	at = link_lbt_begin(&l, link_now_us(), max_defer_us, force, predelay);

	while (true) {
		enum link_lbt_res res;
		int16_t peak = 0;
		int clear;

		link_sleep_until_us(at);

		/* A failed sample counts as clear: never block a frame on it. */
		clear = tp->channel_clear(LINK_LBT_LISTEN_US, &peak);
		res = link_lbt_step(&l, clear != 0, link_now_us(), &at);

		switch (res) {
		case LINK_LBT_RETRY:
			stats->busy++;
			break;
		case LINK_LBT_FORCED:
			stats->busy++;
			stats->forced++;
			LOG_DBG("busy (%d dBm) for %u us, sending anyway", peak, max_defer_us);
			return res;
		case LINK_LBT_GIVE_UP:
			stats->busy++;
			stats->gave_up++;
			LOG_DBG("busy (%d dBm) for %u us, giving up", peak, max_defer_us);
			return res;
		default:
			return res;
		}
	}
}
