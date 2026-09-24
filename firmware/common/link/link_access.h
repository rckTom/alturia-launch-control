/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The kernel side of channel access, shared by sat_link and ctrl_link: a
 * microsecond clock and a blocking listen-before-talk on top of the
 * policy in link_sched.h.
 */

#ifndef LINK_ACCESS_H_
#define LINK_ACCESS_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include "link_sched.h"
#include "link_transport.h"

/* The clock that link_rx_info.time_us and the slots are counted in. */
static inline int64_t link_now_us(void)
{
	return k_ticks_to_us_floor64(k_uptime_ticks());
}

static inline void link_sleep_until_us(int64_t t_us)
{
	if (t_us > link_now_us()) {
		k_sleep(K_TIMEOUT_ABS_US(t_us));
	}
}

struct link_lbt_stats {
	uint32_t busy;      /* listens that found the channel busy */
	uint32_t forced;    /* sent anyway at the deadline */
	uint32_t gave_up;   /* dropped at the deadline */
};

/*
 * Listen before sending on @p tp, backing off while the channel is busy,
 * for at most @p max_defer_us. Returns LINK_LBT_SEND, LINK_LBT_FORCED or
 * LINK_LBT_GIVE_UP. With listen-before-talk off it returns SEND at once.
 */
enum link_lbt_res link_lbt_wait(const struct link_transport *tp, uint32_t max_defer_us,
				bool force, bool predelay, struct link_lbt_stats *stats);

#endif /* LINK_ACCESS_H_ */
