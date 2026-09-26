/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/logging/log.h>

#include "link_access.h"

LOG_MODULE_REGISTER(link_access, LOG_LEVEL_INF);

enum link_csma_res link_csma_wait(const struct link_transport *tp,
				  const struct link_csma_class *c, struct link_util *util,
				  struct k_mutex *lock, struct link_csma_stats *stats)
{
	struct link_csma s;
	bool deferred = false;
	int64_t at;

	if (LINK_LBT_LISTEN_US == 0 || !tp->channel_clear) {
		return LINK_CSMA_SEND;
	}

	k_mutex_lock(lock, K_FOREVER);
	at = link_csma_begin(&s, c, link_util_pct(util, link_now_us()), link_now_us());
	k_mutex_unlock(lock);

	while (true) {
		enum link_csma_res res;
		int16_t peak = 0;
		int64_t start;
		int64_t now;
		int clear;

		link_sleep_until_us(at);

		/* A failed sense counts as clear: never block a frame on it. */
		start = link_now_us();
		clear = tp->channel_clear(LINK_LBT_LISTEN_US, &peak) != 0;
		now = link_now_us();
		res = link_csma_step(&s, clear, now, &at);

		k_mutex_lock(lock, K_FOREVER);
		if (!clear) {
			link_util_add(util, start, now);
			stats->busy++;
			if (!deferred) {
				deferred = true;
				stats->deferred++;
			}
		}
		if (res == LINK_CSMA_FORCED) {
			stats->forced++;
		} else if (res == LINK_CSMA_GIVE_UP) {
			stats->gave_up++;
		}
		k_mutex_unlock(lock);

		switch (res) {
		case LINK_CSMA_SENSE:
			break;
		case LINK_CSMA_FORCED:
			LOG_DBG("busy (%d dBm) until the deadline, sending anyway", peak);
			return res;
		case LINK_CSMA_GIVE_UP:
			LOG_DBG("busy (%d dBm) until the deadline, giving up", peak);
			return res;
		default:
			return res;
		}
	}
}
