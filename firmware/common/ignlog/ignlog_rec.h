/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * Ignition log records: the current through one channel during one fire
 * pulse, as the satellite samples it. A record is a byte blob that is the
 * same everywhere it goes. The satellite builds it, the LoRa link carries
 * it in chunks (common/wire/sat_wire.h), the controller caches it as it
 * came, and both serve it over mcumgr (ignlog_mgmt.c). The browser tool
 * parses the same layout (tools/webtool/src/smp/ignlog.ts). See
 * documentation/ignition_log.md.
 *
 * Layout, little-endian, version 1:
 *
 *   0   u8   version
 *   1   u8   channel, 0..5 (panel 1..6)
 *   2   u8   flags, IGNLOG_F_*
 *   3   u8   reserved, 0
 *   4   u32  seq, per satellite store, from 1
 *   8   u32  t_fire_ms, the satellite's uptime at the start of the pulse
 *   12  u16  boot, the satellite's boot count when it fired
 *   14  u16  store_id, random per cold start of the satellite's store
 *   16  u16  period_us between samples
 *   18  u16  n_samples
 *   20  u16  pulse_ms asked for
 *   22  u16  vpyro_min_mv, lowest fire rail voltage during the pulse
 *   24  u32  scale, nA of load current per ADC code
 *   28  u16  peak_code
 *   30  u16  open_idx, first sample after which the current stayed off
 *            (IGNLOG_OPEN_NONE: it never did)
 *   32  u16  samples[n_samples], raw ADC codes
 *
 * Codes and the scale travel raw, so that a later calibration fix does
 * not invalidate stored records.
 */

#ifndef IGNLOG_REC_H_
#define IGNLOG_REC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IGNLOG_VERSION    1
#define IGNLOG_HDR_LEN    32
#define IGNLOG_OPEN_NONE  0xFFFF

#define IGNLOG_REC_LEN(n) (IGNLOG_HDR_LEN + 2 * (size_t)(n))

/* The current hit the end of the sense range: TPS1HB08 fault or current limit. */
#define IGNLOG_F_FAULT      (1U << 0)
/* The pulse outlasted the sample buffer; the record ends early. */
#define IGNLOG_F_TRUNCATED  (1U << 1)
/* The ADC missed sampling intervals. */
#define IGNLOG_F_OVERRUN    (1U << 2)
/* No current worth the name flowed at any time. */
#define IGNLOG_F_NO_CURRENT (1U << 3)

/*
 * "Open" is below max(peak / IGNLOG_OPEN_DIV, IGNLOG_OPEN_MIN_MA); "no
 * current" is a peak below IGNLOG_OPEN_MIN_MA.
 */
#define IGNLOG_OPEN_DIV     8
#define IGNLOG_OPEN_MIN_MA  100

/* What a list of records shows: the header without the per-pulse details. */
struct ignlog_entry {
	uint32_t seq;
	uint32_t t_fire_ms;
	uint16_t boot;
	uint16_t n_samples;
	uint16_t period_us;
	uint16_t peak_code;
	uint16_t open_idx;
	uint8_t channel;
	uint8_t flags;
};

struct ignlog_meta {
	struct ignlog_entry e;
	uint16_t store_id;
	uint16_t pulse_ms;
	uint16_t vpyro_min_mv;
	uint32_t scale_na;
};

/* At most this many records are listed at a time (the newest). */
#define IGNLOG_LIST_MAX 8

/* A store's records as listed: the satellite's own, or the controller's copy. */
struct ignlog_store_view {
	uint16_t store_id;  /* new after every power-on of the satellite */
	uint16_t boots;     /* its reboots since */
	uint32_t now_ms;    /* its uptime now */
	uint32_t scale_na;  /* nA per code */
	uint8_t count;
	uint8_t have;       /* bit i: e[i] is held whole (controller cache) */
	struct ignlog_entry e[IGNLOG_LIST_MAX];  /* oldest first */
};

void ignlog_meta_put(const struct ignlog_meta *m, uint8_t out[IGNLOG_HDR_LEN]);
/*
 * Parse a record's header. -EINVAL if @p len is shorter than the header,
 * or the version is unknown. The samples may still be missing (a record
 * coming in chunks); IGNLOG_REC_LEN(m->e.n_samples) is the full size.
 */
int ignlog_meta_get(const uint8_t *buf, size_t len, struct ignlog_meta *m);

/* The record's sample @p i, from its little-endian bytes. */
uint16_t ignlog_sample(const uint8_t *rec, uint16_t i);

/* Load current in mA for a code. */
static inline int32_t ignlog_ma(uint16_t code, uint32_t scale_na)
{
	return (int32_t)(((uint64_t)code * scale_na + 500000) / 1000000);
}

/*
 * Fill in peak_code, open_idx and IGNLOG_F_NO_CURRENT for the samples of
 * the record at @p rec (m filled in up to n_samples, period, pulse_ms and
 * scale). Current that only stops when the pulse ends is not "open".
 */
void ignlog_summarize(const uint8_t *rec, struct ignlog_meta *m);

#endif /* IGNLOG_REC_H_ */
