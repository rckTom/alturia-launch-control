/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The kernel side of channel access, shared by sat_link and ctrl_link: a
 * microsecond clock and a blocking CSMA/CA on top of the policy in
 * link_sched.h.
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

struct link_csma_stats {
	uint32_t busy;      /* senses that found the channel busy */
	uint32_t deferred;  /* frames that met a busy channel at least once */
	uint32_t forced;    /* sent anyway at the deadline */
	uint32_t gave_up;   /* dropped at the deadline */
};

/*
 * CSMA/CA before sending on @p tp, with class @p c (link_sched.h). The
 * contention window follows @p util, and busy senses are added to it.
 * @p lock guards @p util and @p stats; it may already be held by the
 * caller. Returns LINK_CSMA_SEND, LINK_CSMA_FORCED or LINK_CSMA_GIVE_UP.
 * With carrier sensing off it returns SEND at once.
 */
enum link_csma_res link_csma_wait(const struct link_transport *tp,
				  const struct link_csma_class *c, struct link_util *util,
				  struct k_mutex *lock, struct link_csma_stats *stats);

#endif /* LINK_ACCESS_H_ */
