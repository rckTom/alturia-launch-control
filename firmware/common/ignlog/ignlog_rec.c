/* SPDX-License-Identifier: GPL-3.0-only */

/* See ignlog_rec.h. */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "ignlog_rec.h"

void ignlog_meta_put(const struct ignlog_meta *m, uint8_t out[IGNLOG_HDR_LEN])
{
	out[0] = IGNLOG_VERSION;
	out[1] = m->e.channel;
	out[2] = m->e.flags;
	out[3] = 0;
	sys_put_le32(m->e.seq, out + 4);
	sys_put_le32(m->e.t_fire_ms, out + 8);
	sys_put_le16(m->e.boot, out + 12);
	sys_put_le16(m->store_id, out + 14);
	sys_put_le16(m->e.period_us, out + 16);
	sys_put_le16(m->e.n_samples, out + 18);
	sys_put_le16(m->pulse_ms, out + 20);
	sys_put_le16(m->vpyro_min_mv, out + 22);
	sys_put_le32(m->scale_na, out + 24);
	sys_put_le16(m->e.peak_code, out + 28);
	sys_put_le16(m->e.open_idx, out + 30);
}

int ignlog_meta_get(const uint8_t *buf, size_t len, struct ignlog_meta *m)
{
	if (len < IGNLOG_HDR_LEN || buf[0] != IGNLOG_VERSION) {
		return -EINVAL;
	}

	m->e.channel = buf[1];
	m->e.flags = buf[2];
	m->e.seq = sys_get_le32(buf + 4);
	m->e.t_fire_ms = sys_get_le32(buf + 8);
	m->e.boot = sys_get_le16(buf + 12);
	m->store_id = sys_get_le16(buf + 14);
	m->e.period_us = sys_get_le16(buf + 16);
	m->e.n_samples = sys_get_le16(buf + 18);
	m->pulse_ms = sys_get_le16(buf + 20);
	m->vpyro_min_mv = sys_get_le16(buf + 22);
	m->scale_na = sys_get_le32(buf + 24);
	m->e.peak_code = sys_get_le16(buf + 28);
	m->e.open_idx = sys_get_le16(buf + 30);
	return 0;
}

uint16_t ignlog_sample(const uint8_t *rec, uint16_t i)
{
	return sys_get_le16(rec + IGNLOG_HDR_LEN + 2 * (size_t)i);
}

void ignlog_summarize(const uint8_t *rec, struct ignlog_meta *m)
{
	uint16_t n = m->e.n_samples;
	uint16_t peak = 0;
	uint32_t min_code;
	uint32_t thresh;
	int last_on = -1;

	for (uint16_t i = 0; i < n; i++) {
		peak = MAX(peak, ignlog_sample(rec, i));
	}

	/* IGNLOG_OPEN_MIN_MA in codes, rounded up. */
	min_code = m->scale_na ? DIV_ROUND_UP((uint64_t)IGNLOG_OPEN_MIN_MA * 1000000, m->scale_na)
			       : 1;
	thresh = MAX(peak / IGNLOG_OPEN_DIV, min_code);

	m->e.peak_code = peak;
	if (peak < min_code) {
		m->e.flags |= IGNLOG_F_NO_CURRENT;
		m->e.open_idx = IGNLOG_OPEN_NONE;
		return;
	}
	m->e.flags &= ~IGNLOG_F_NO_CURRENT;

	for (int i = n - 1; i >= 0; i--) {
		if (ignlog_sample(rec, i) >= thresh) {
			last_on = i;
			break;
		}
	}
	m->e.open_idx = last_on + 1 < n ? last_on + 1 : IGNLOG_OPEN_NONE;

	/* Current that only stopped with the pulse: the igniter never opened. */
	if (m->e.open_idx != IGNLOG_OPEN_NONE && m->pulse_ms &&
	    ((uint32_t)m->e.open_idx + 1) * m->e.period_us >= (uint32_t)m->pulse_ms * 1000) {
		m->e.open_idx = IGNLOG_OPEN_NONE;
	}
}
