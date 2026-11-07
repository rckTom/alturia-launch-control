/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * Ignition log: the current through each channel during its fire pulses,
 * for the last CONFIG_SAT_IGNLOG_RECORDS channel pulses. See
 * documentation/ignition_log.md and common/ignlog/ignlog_rec.h.
 *
 * Measurement: every TPS1HB08 mirrors its load current, 1/K_SNS of it, out
 * of SNS into R_SNS while DIA_EN is high (SEL1 is tied low, so SNS shows
 * current rather than temperature). The MCP3208 reads the voltage, CH(5 - ch)
 * for channel ch. DIA_EN is common to all six switches; it goes high at the
 * start of each pulse, before the outputs (the SNS output settles within
 * 165 us of EN, tSNSION3) and back to what the shell set afterwards.
 *
 * Sampling: one adc_read() on a thread of its own, every
 * CONFIG_SAT_IGNLOG_PERIOD_US, on the ADC driver's kernel timer. The
 * sequence holds the fired channels and V_PYRO; the per-sampling callback
 * moves each sample into its channel's record and repeats into the same
 * small buffer, so the records are the only buffers. The pulse's end stops
 * it IGNLOG_TAIL samples later, so the record shows the switch-off.
 *
 * Storage: a ring in RAM that is not cleared at boot (__noinit). It
 * survives a reset, a watchdog trip and the reboot through MCUboot (whose
 * RAM ends well below the app's noinit section; checked at every build,
 * see documentation/ignition_log.md), but not a power-off. Every record
 * has a CRC; a bad one is dropped at boot, a bad store header starts the
 * store over with a new store id.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include "ignlog.h"
#include "ignlog_mgmt.h"
#include "satellite.h"

LOG_MODULE_REGISTER(ignlog, LOG_LEVEL_INF);

#define N_RECS      CONFIG_SAT_IGNLOG_RECORDS
#define MAX_SAMPLES CONFIG_SAT_IGNLOG_MAX_SAMPLES
#define REC_BYTES   IGNLOG_REC_LEN(MAX_SAMPLES)
#define IGNLOG_TAIL 5

#define STORE_MAGIC 0x49474c31UL   /* "IGL1" */

/* The sense chain, hardware/satellite/pcb/output_channel.kicad_sch. */
#define K_SNS      5000   /* TPS1HB08-Q1 I_OUT / I_SNS (datasheet KSNS) */
#define R_SNS_OHM  560
/* I_SNSFH, the fault level: at least 4 mA. Codes at or above it are a fault. */
#define I_SNSFH_UA 4000
/* V_PYRO_ARMED into CH6 through 3k3 over 1k. */
#define VPYRO_CH   6
#define VPYRO_NUM  4300
#define VPYRO_DEN  1000

#define RAILS DT_NODELABEL(adc_rails)
#if DT_NODE_HAS_PROP(RAILS, ref_external0_mv)
#define RAILS_VREF_MV DT_PROP(RAILS, ref_external0_mv)   /* native_sim emulator */
#else
#define RAILS_VREF_MV DT_PROP(DT_CHILD(RAILS, channel_6), zephyr_vref_mv)
#endif

/* nA of load current per code: Vref / 4096 / R_SNS * K_SNS. */
#define SCALE_NA ((uint32_t)((uint64_t)RAILS_VREF_MV * K_SNS * 1000000 / (4096ULL * R_SNS_OHM)))
#define FAULT_CODE ((uint32_t)((uint64_t)I_SNSFH_UA * R_SNS_OHM * 4096 / (1000ULL * RAILS_VREF_MV)))

static inline uint8_t sense_input(int ch)
{
	return SAT_CHANNELS - 1 - ch;
}

struct slot {
	uint32_t crc;      /* over rec[0 .. len) */
	uint16_t len;      /* 0: empty, or being recorded */
	uint16_t pad;
	uint8_t rec[REC_BYTES] __aligned(4);
};

struct store {
	uint32_t magic;
	uint16_t store_id;
	uint16_t boots;
	uint32_t next_seq;
	uint32_t next_slot;
	uint32_t check;    /* over the fields above */
	struct slot slot[N_RECS];
};

static __noinit struct store store;
static K_MUTEX_DEFINE(lock);

static const struct device *const adc = DEVICE_DT_GET(RAILS);

/* The capture in progress. */
static struct {
	uint8_t mask;
	uint8_t nconv;               /* conversions per sampling */
	int8_t dst[SAT_CHANNELS + 1]; /* per conversion: slot, or -1 for V_PYRO */
	int8_t ch_of[SAT_CHANNELS + 1];
	uint16_t buf[SAT_CHANNELS + 1];
	uint16_t count;
	volatile uint16_t stop_at;   /* 0: until the pulse ends */
	uint16_t vpyro_min;
	uint32_t t_fire_ms;
	uint32_t pulse_ms;
	bool truncated;
	int status;
} cap;

static atomic_t busy;
static K_SEM_DEFINE(start_sem, 0, 1);

static uint32_t header_check(const struct store *s)
{
	uint32_t w[] = { s->magic, ((uint32_t)s->store_id << 16) | s->boots, s->next_seq,
			 s->next_slot };

	return crc32_ieee((const uint8_t *)w, sizeof(w));
}

static bool slot_valid(const struct slot *sl)
{
	return sl->len >= IGNLOG_HDR_LEN && sl->len <= REC_BYTES &&
	       crc32_ieee(sl->rec, sl->len) == sl->crc;
}

static void store_reset(void)
{
	memset(&store, 0, sizeof(store));
	store.magic = STORE_MAGIC;
	do {
		store.store_id = (uint16_t)sys_rand32_get();
	} while (store.store_id == 0);
	store.next_seq = 1;
}

static enum adc_action on_sampling(const struct device *dev, const struct adc_sequence *seq,
				   uint16_t index)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(seq);
	ARG_UNUSED(index);

	for (int i = 0; i < cap.nconv; i++) {
		uint16_t v = cap.buf[i];

		if (cap.dst[i] < 0) {
			cap.vpyro_min = MIN(cap.vpyro_min, v);
		} else {
			uint8_t *p = store.slot[cap.dst[i]].rec + IGNLOG_HDR_LEN + 2 * cap.count;

			p[0] = v & 0xFF;
			p[1] = v >> 8;
		}
	}
	cap.count++;

	if (cap.stop_at && cap.count >= cap.stop_at) {
		return ADC_ACTION_FINISH;
	}
	if (cap.count >= MAX_SAMPLES) {
		cap.truncated = cap.stop_at == 0;
		return ADC_ACTION_FINISH;
	}
	return ADC_ACTION_REPEAT;
}

static void finish(void)
{
	uint16_t vpyro_mv = cap.vpyro_min == UINT16_MAX ? 0 :
		(uint32_t)cap.vpyro_min * RAILS_VREF_MV / 4096 * VPYRO_NUM / VPYRO_DEN;

	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < cap.nconv; i++) {
		struct slot *sl;
		struct ignlog_meta m = { 0 };

		if (cap.dst[i] < 0) {
			continue;
		}
		sl = &store.slot[cap.dst[i]];

		m.e.seq = store.next_seq++;
		m.e.t_fire_ms = cap.t_fire_ms;
		m.e.boot = store.boots;
		m.e.n_samples = cap.count;
		m.e.period_us = CONFIG_SAT_IGNLOG_PERIOD_US;
		m.e.channel = cap.ch_of[i];
		m.store_id = store.store_id;
		m.pulse_ms = MIN(cap.pulse_ms, UINT16_MAX);
		m.vpyro_min_mv = vpyro_mv;
		m.scale_na = SCALE_NA;
		ignlog_summarize(sl->rec, &m);
		if (m.e.peak_code >= FAULT_CODE) {
			m.e.flags |= IGNLOG_F_FAULT;
		}
		if (cap.truncated) {
			m.e.flags |= IGNLOG_F_TRUNCATED;
		}
		if (cap.status == -EBUSY) {
			m.e.flags |= IGNLOG_F_OVERRUN;
		}
		ignlog_meta_put(&m, sl->rec);
		sl->len = IGNLOG_REC_LEN(cap.count);
		sl->crc = crc32_ieee(sl->rec, sl->len);

		LOG_INF("#%u CH%d: peak %d mA, %s, %u samples%s", m.e.seq, m.e.channel + 1,
			ignlog_ma(m.e.peak_code, m.scale_na),
			m.e.flags & IGNLOG_F_NO_CURRENT ? "no current" :
			m.e.open_idx == IGNLOG_OPEN_NONE ? "never opened" : "opened",
			m.e.n_samples, m.e.flags & IGNLOG_F_FAULT ? ", FAULT" : "");
	}
	store.check = header_check(&store);
	k_mutex_unlock(&lock);
}

static void capture_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		struct adc_sequence_options opts = {
			.interval_us = CONFIG_SAT_IGNLOG_PERIOD_US,
			.callback = on_sampling,
			.extra_samplings = 1,   /* never reached: the callback repeats */
		};
		struct adc_sequence seq = {
			.options = &opts,
			.buffer = cap.buf,
			.buffer_size = sizeof(cap.buf),
			.resolution = 12,
		};

		k_sem_take(&start_sem, K_FOREVER);

		for (int i = 0; i < cap.nconv; i++) {
			seq.channels |= BIT(cap.dst[i] < 0 ? VPYRO_CH : sense_input(cap.ch_of[i]));
		}
		cap.status = adc_read(adc, &seq);
		if (cap.status && cap.status != -EBUSY) {
			LOG_ERR("sampling: %d", cap.status);
		}
		/* Finish whatever was sampled, even after an error. */
		finish();
		channels_sense_hold(false);
		atomic_clear(&busy);
	}
}

K_THREAD_DEFINE(ignlog_tid, 1024, capture_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, 0);

void ignlog_start(uint8_t mask, uint32_t pulse_ms)
{
	int n = 0;

	if (!atomic_cas(&busy, 0, 1)) {
		LOG_WRN("pulse 0x%02x not logged: the last one is still being recorded", mask);
		return;
	}

	/* Sense on first: the outputs go on right after this returns. */
	channels_sense_hold(true);

	k_mutex_lock(&lock, K_FOREVER);
	/*
	 * Conversions in ascending input order, which is the order the
	 * sequence fills the buffer in: sense_input() counts down from
	 * channel 0, so the channels go high to low, then V_PYRO (CH6).
	 */
	for (int ch = SAT_CHANNELS - 1; ch >= 0; ch--) {
		struct slot *sl;

		if (!(mask & BIT(ch))) {
			continue;
		}
		cap.dst[n] = store.next_slot;
		cap.ch_of[n] = ch;
		sl = &store.slot[store.next_slot];
		sl->len = 0;   /* invalid until finished */
		store.next_slot = (store.next_slot + 1) % N_RECS;
		n++;
	}
	cap.dst[n] = -1;
	cap.ch_of[n] = -1;
	n++;
	store.check = header_check(&store);
	k_mutex_unlock(&lock);

	cap.mask = mask;
	cap.nconv = n;
	cap.count = 0;
	cap.stop_at = 0;
	cap.vpyro_min = UINT16_MAX;
	cap.t_fire_ms = k_uptime_get_32();
	cap.pulse_ms = pulse_ms;
	cap.truncated = false;
	cap.status = 0;

	k_sem_give(&start_sem);
}

void ignlog_stop(void)
{
	if (atomic_get(&busy) && cap.stop_at == 0) {
		cap.stop_at = MIN(cap.count + IGNLOG_TAIL, MAX_SAMPLES);
	}
}

int ignlog_init(void)
{
	int ret;
	int dropped = 0;

	if (!device_is_ready(adc)) {
		return -ENODEV;
	}
	/* The six sense inputs, and V_PYRO (also set up by its voltage divider). */
	for (int ch = 0; ch <= SAT_CHANNELS; ch++) {
		struct adc_channel_cfg cfg = {
			.gain = ADC_GAIN_1,
			.reference = ADC_REF_EXTERNAL0,
			.acquisition_time = ADC_ACQ_TIME_DEFAULT,
			.channel_id = ch < SAT_CHANNELS ? sense_input(ch) : VPYRO_CH,
		};

		ret = adc_channel_setup(adc, &cfg);
		if (ret) {
			return ret;
		}
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (store.magic != STORE_MAGIC || store.check != header_check(&store) ||
	    store.next_slot >= N_RECS) {
		store_reset();
		LOG_INF("store started, id %04x", store.store_id);
	} else {
		for (int i = 0; i < N_RECS; i++) {
			if (store.slot[i].len && !slot_valid(&store.slot[i])) {
				store.slot[i].len = 0;
				dropped++;
			}
		}
		store.boots++;
		LOG_INF("store %04x kept over the reboot, boot %u%s", store.store_id, store.boots,
			dropped ? ", damaged records dropped" : "");
	}
	store.check = header_check(&store);
	k_mutex_unlock(&lock);
	return 0;
}

/* The valid slots, by ascending seq. */
static int sorted_slots(int out[N_RECS])
{
	int n = 0;

	for (int i = 0; i < N_RECS; i++) {
		struct slot *sl = &store.slot[i];
		int j;

		if (!sl->len) {
			continue;
		}
		j = n++;
		while (j > 0 && sys_get_le32(store.slot[out[j - 1]].rec + 4) >
				sys_get_le32(sl->rec + 4)) {
			out[j] = out[j - 1];
			j--;
		}
		out[j] = i;
	}
	return n;
}

void ignlog_view(struct ignlog_store_view *v)
{
	int idx[N_RECS];
	int n;
	int first;

	k_mutex_lock(&lock, K_FOREVER);
	n = sorted_slots(idx);
	first = MAX(0, n - IGNLOG_LIST_MAX);
	v->count = 0;
	for (int i = first; i < n; i++) {
		struct ignlog_meta m;

		(void)ignlog_meta_get(store.slot[idx[i]].rec, store.slot[idx[i]].len, &m);
		v->e[v->count++] = m.e;
	}
	v->have = BIT_MASK(v->count);
	v->store_id = store.store_id;
	v->boots = store.boots;
	v->now_ms = k_uptime_get_32();
	v->scale_na = SCALE_NA;
	k_mutex_unlock(&lock);
}

int ignlog_read(uint32_t seq, size_t off, uint8_t *buf, size_t len, size_t *total)
{
	int ret = -ENOENT;

	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < N_RECS; i++) {
		struct slot *sl = &store.slot[i];

		if (sl->len && sys_get_le32(sl->rec + 4) == seq) {
			*total = sl->len;
			if (off > sl->len) {
				ret = -EINVAL;
				break;
			}
			ret = MIN(len, sl->len - off);
			memcpy(buf, sl->rec + off, ret);
			break;
		}
	}
	k_mutex_unlock(&lock);
	return ret;
}

int ignlog_clear(void)
{
	if (atomic_get(&busy)) {
		return -EBUSY;
	}
	k_mutex_lock(&lock, K_FOREVER);
	for (int i = 0; i < N_RECS; i++) {
		store.slot[i].len = 0;
	}
	store.check = header_check(&store);
	k_mutex_unlock(&lock);
	return 0;
}

bool ignlog_recording(void)
{
	return atomic_get(&busy);
}

/* The mcumgr group's backend (common/ignlog/ignlog_mgmt.h): this satellite is "sat" 0. */
int ignlog_be_view(uint8_t sat, struct ignlog_store_view *v)
{
	if (sat != 0) {
		return -ENOENT;
	}
	ignlog_view(v);
	return 0;
}

int ignlog_be_read(uint8_t sat, uint32_t seq, size_t off, uint8_t *buf, size_t len,
		   size_t *total)
{
	return sat == 0 ? ignlog_read(seq, off, buf, len, total) : -ENOENT;
}

int ignlog_be_clear(uint8_t sat)
{
	if (!channels_all_safe()) {
		return -EBUSY;
	}
	return sat == 0 ? ignlog_clear() : -ENOENT;
}

int ignlog_be_fetch(uint8_t sat)
{
	ARG_UNUSED(sat);
	return -ENOTSUP;
}

int ignlog_be_busy(void)
{
	return ignlog_recording();
}

uint8_t ignlog_be_sats(void)
{
	return 0;
}
