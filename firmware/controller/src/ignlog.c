/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * See ignlog.h. A download is a short sequence of log polls to one
 * satellite (common/wire/sat_wire.h): the index, then a record in chunks
 * of up to WIRE_LOG_CHUNK_MAX bytes. One log poll is out at a time; it is
 * answered in the satellite's reply slot, or asked again after
 * POLL_TIMEOUT_MS. The regular polls go on as before, so every satellite
 * keeps its heartbeat.
 *
 * A record from a satellite whose store id has changed (it was switched
 * off and on) is gone from it, so the cache drops it too: records are
 * known by (satellite, store id, seq).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "controller.h"
#include "ignlog.h"
#include "ignlog_mgmt.h"
#include "link.h"
#include "sat_wire.h"

LOG_MODULE_REGISTER(ignlog, LOG_LEVEL_INF);

#define CACHE_RECS      CONFIG_APP_IGNLOG_CACHE
#define REC_MAX         IGNLOG_REC_LEN(CONFIG_APP_IGNLOG_MAX_SAMPLES)
/* Poll airtime + turnaround + a full slot is some 250 ms; polls queued ahead add to it. */
#define POLL_TIMEOUT_MS 900
#define POLL_TRIES      6

struct cached_rec {
	int8_t sat;          /* -1: free */
	bool complete;
	uint16_t store_id;
	uint32_t seq;
	size_t len;
	int64_t used;        /* for evicting the least recently used */
	uint8_t data[REC_MAX];
};

struct sat_index {
	bool valid;
	int64_t at;          /* when it arrived, k_uptime_get() */
	struct ignlog_store_view v;
};

/* The cache, read by the mcumgr backend too. */
static K_MUTEX_DEFINE(lock);
static struct cached_rec cache[CACHE_RECS];
static struct sat_index index_of[NUM_SATS];

/* Log replies, from the link thread. */
struct rx_msg {
	int8_t sat;
	uint8_t len;
	uint8_t buf[MAX(WIRE_LOG_CHUNK_LEN, WIRE_LOG_INDEX_LEN(WIRE_LOG_INDEX_MAX))];
};
K_MSGQ_DEFINE(rx_q, sizeof(struct rx_msg), 2, 4);

/* Requests from the mcumgr backend, run on the LVGL thread. */
struct mgmt_req {
	bool clear;
	int8_t sat;          /* clear: -1 for all */
};
K_MSGQ_DEFINE(mgmt_q, sizeof(struct mgmt_req), 4, 4);

static struct {
	enum ignlog_job_state state;
	int sat;
	bool all;            /* every record missing from the cache, after the index */
	bool want_index;
	uint32_t seq;        /* the record, 0 while getting the index */
	size_t off, total;
	struct cached_rec *rec;
	bool in_flight;
	int64_t sent;
	int tries;
	bool paused;
	const char *error;
} job = { .sat = -1 };

static void evict_init(void)
{
	static bool done;

	if (!done) {
		for (int i = 0; i < CACHE_RECS; i++) {
			cache[i].sat = -1;
		}
		done = true;
	}
}

/* With the lock held. */
static struct cached_rec *find(int sat, uint16_t store_id, uint32_t seq)
{
	for (int i = 0; i < CACHE_RECS; i++) {
		struct cached_rec *c = &cache[i];

		if (c->sat == sat && c->store_id == store_id && c->seq == seq) {
			return c;
		}
	}
	return NULL;
}

/* A buffer for a new record: a free one, else the least recently used. Lock held. */
static struct cached_rec *alloc(void)
{
	struct cached_rec *best = NULL;

	for (int i = 0; i < CACHE_RECS; i++) {
		struct cached_rec *c = &cache[i];

		if (c->sat < 0) {
			return c;
		}
		if (c != job.rec && (!best || c->used < best->used)) {
			best = c;
		}
	}
	return best;
}

static void drop_sat(int sat)
{
	for (int i = 0; i < CACHE_RECS; i++) {
		if (cache[i].sat == sat) {
			cache[i].sat = -1;
		}
	}
}

static uint8_t have_mask(int sat, const struct ignlog_store_view *v)
{
	uint8_t have = 0;

	for (int i = 0; i < v->count; i++) {
		const struct cached_rec *c = find(sat, v->store_id, v->e[i].seq);

		if (c && c->complete) {
			have |= BIT(i);
		}
	}
	return have;
}

static void finish(enum ignlog_job_state state, const char *error)
{
	if (state == IGNLOG_FAILED) {
		LOG_WRN("satellite %c: log download failed: %s", 'A' + job.sat, error);
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (job.rec && !job.rec->complete) {
		job.rec->sat = -1;
	}
	k_mutex_unlock(&lock);
	job.rec = NULL;
	job.state = state;
	job.error = error;
	job.in_flight = false;
}

static void start(int sat, bool all, bool want_index, uint32_t seq)
{
	if (job.state == IGNLOG_BUSY) {
		finish(IGNLOG_IDLE, NULL);
	}
	job.state = IGNLOG_BUSY;
	job.sat = sat;
	job.all = all;
	job.want_index = want_index;
	job.seq = seq;
	job.off = 0;
	job.total = 0;
	job.rec = NULL;
	job.in_flight = false;
	job.tries = 0;
	job.paused = false;
	job.error = NULL;
}

/* After the index, or a record done: the next record a "get all" still lacks. */
static void next_record(void)
{
	const struct sat_index *ix = &index_of[job.sat];
	uint32_t seq = 0;

	k_mutex_lock(&lock, K_FOREVER);
	/* Newest first: the one most likely wanted. */
	for (int i = ix->v.count - 1; i >= 0 && ix->valid; i--) {
		const struct cached_rec *c = find(job.sat, ix->v.store_id, ix->v.e[i].seq);

		if (!c || !c->complete) {
			seq = ix->v.e[i].seq;
			break;
		}
	}
	k_mutex_unlock(&lock);

	if (seq == 0) {
		finish(IGNLOG_DONE, NULL);
		return;
	}
	job.seq = seq;
	job.off = 0;
	job.total = 0;
}

static void on_index(int sat, const struct wire_log_index *wi)
{
	struct sat_index *ix = &index_of[sat];

	k_mutex_lock(&lock, K_FOREVER);
	if (ix->valid && ix->v.store_id != wi->store_id) {
		drop_sat(sat);   /* the satellite started its store over */
	}
	ix->valid = true;
	ix->at = k_uptime_get();
	ix->v.store_id = wi->store_id;
	ix->v.boots = wi->boots;
	ix->v.now_ms = wi->now_ms;
	ix->v.scale_na = wi->scale_na;
	ix->v.count = MIN(wi->count, IGNLOG_LIST_MAX);
	memcpy(ix->v.e, wi->e, ix->v.count * sizeof(ix->v.e[0]));
	k_mutex_unlock(&lock);

	if (job.state != IGNLOG_BUSY || job.sat != sat) {
		return;
	}
	job.in_flight = false;
	job.tries = 0;
	if (job.want_index) {
		job.want_index = false;
		if (job.all) {
			next_record();
		} else if (job.seq == 0) {
			finish(IGNLOG_DONE, NULL);
		}
	} else if (job.seq) {
		/* Asked for a chunk, got the index: the record is gone. */
		finish(IGNLOG_FAILED, "record no longer on the satellite");
	}
}

static void on_chunk(int sat, const struct wire_log_chunk *c)
{
	const struct sat_index *ix = &index_of[sat];
	struct ignlog_meta m;

	if (job.state != IGNLOG_BUSY || job.sat != sat || job.want_index || c->seq != job.seq ||
	    c->off != job.off) {
		return;   /* a late answer to an earlier poll */
	}
	job.in_flight = false;
	job.tries = 0;
	if (!ix->valid) {
		finish(IGNLOG_FAILED, "no index");
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (c->off == 0) {
		if (c->total < IGNLOG_HDR_LEN || c->total > REC_MAX) {
			k_mutex_unlock(&lock);
			finish(IGNLOG_FAILED, "record too large");
			return;
		}
		job.total = c->total;
		job.rec = find(sat, ix->v.store_id, c->seq);
		if (!job.rec) {
			job.rec = alloc();
		}
		*job.rec = (struct cached_rec){
			.sat = sat, .store_id = ix->v.store_id, .seq = c->seq,
		};
	}
	if (!job.rec || c->total != job.total) {
		k_mutex_unlock(&lock);
		finish(IGNLOG_FAILED, "record changed while downloading");
		return;
	}
	memcpy(job.rec->data + c->off, c->data, c->len);
	job.off += c->len;
	job.rec->len = job.off;
	job.rec->used = k_uptime_get();

	if (job.off < job.total) {
		k_mutex_unlock(&lock);
		if (c->len == 0) {
			finish(IGNLOG_FAILED, "empty chunk");
		}
		return;
	}
	if (ignlog_meta_get(job.rec->data, job.rec->len, &m) ||
	    IGNLOG_REC_LEN(m.e.n_samples) != job.rec->len || m.e.seq != job.seq) {
		k_mutex_unlock(&lock);
		finish(IGNLOG_FAILED, "record does not parse");
		return;
	}
	job.rec->complete = true;
	job.rec = NULL;
	k_mutex_unlock(&lock);
	LOG_INF("satellite %c: record #%u, %u bytes", 'A' + sat, job.seq, (unsigned)job.total);

	if (job.all) {
		next_record();
	} else {
		finish(IGNLOG_DONE, NULL);
	}
}

void ignlog_rx(int sat, const uint8_t *buf, size_t len)
{
	struct rx_msg msg = { .sat = sat, .len = len };

	if (sat < 0 || sat >= NUM_SATS || len > sizeof(msg.buf)) {
		return;
	}
	memcpy(msg.buf, buf, len);
	if (k_msgq_put(&rx_q, &msg, K_NO_WAIT)) {
		LOG_WRN("log reply dropped");
	}
}

static void handle_rx(void)
{
	struct rx_msg msg;

	while (k_msgq_get(&rx_q, &msg, K_NO_WAIT) == 0) {
		struct wire_log_index wi;
		struct wire_log_chunk c;

		switch (wire_reply_kind(msg.buf, msg.len)) {
		case WIRE_REPLY_LOG_INDEX:
			if (wire_decode_log_index(msg.buf, msg.len, &wi) == 0) {
				on_index(msg.sat, &wi);
			}
			break;
		case WIRE_REPLY_LOG_CHUNK:
			if (wire_decode_log_chunk(msg.buf, msg.len, &c) == 0) {
				on_chunk(msg.sat, &c);
			}
			break;
		default:
			break;
		}
	}
}

static void handle_mgmt(void)
{
	struct mgmt_req r;

	while (k_msgq_get(&mgmt_q, &r, K_NO_WAIT) == 0) {
		if (!r.clear) {
			(void)ignlog_get_all(r.sat);
			continue;
		}
		/* The copies are gone already (ignlog_be_clear()); stop downloading more. */
		if (job.state == IGNLOG_BUSY && (r.sat < 0 || r.sat == job.sat)) {
			finish(IGNLOG_IDLE, NULL);
		}
	}
}

void ignlog_tick(int64_t now, bool allowed)
{
	struct wire_log_req req;
	int ret;

	evict_init();
	handle_mgmt();
	handle_rx();

	if (job.state != IGNLOG_BUSY) {
		return;
	}
	if (!link_joined(job.sat)) {
		finish(IGNLOG_FAILED, "satellite not connected");
		return;
	}
	if (job.in_flight) {
		if (now - job.sent < POLL_TIMEOUT_MS) {
			return;
		}
		job.in_flight = false;
		if (++job.tries >= POLL_TRIES) {
			finish(IGNLOG_FAILED, "no answer from the satellite");
			return;
		}
	}
	job.paused = !allowed;
	if (!allowed) {
		return;
	}

	req.op = job.want_index ? WIRE_LOG_OP_INDEX : WIRE_LOG_OP_CHUNK;
	req.seq = job.want_index ? 0 : job.seq;
	req.off = job.want_index ? 0 : job.off;
	ret = link_log_poll(job.sat, &req);
	if (ret == 0) {
		job.in_flight = true;
		job.sent = now;
	} else if (ret != -EBUSY && ret != -EAGAIN) {
		finish(IGNLOG_FAILED, "link refused the log poll");
	}
}

int ignlog_get_index(int sat)
{
	if (sat < 0 || sat >= NUM_SATS || !link_joined(sat)) {
		return -ENOTCONN;
	}
	start(sat, false, true, 0);
	return 0;
}

int ignlog_get_record(int sat, uint32_t seq)
{
	if (sat < 0 || sat >= NUM_SATS || !link_joined(sat)) {
		return -ENOTCONN;
	}
	start(sat, false, false, seq);
	return 0;
}

int ignlog_get_all(int sat)
{
	if (sat < 0 || sat >= NUM_SATS || !link_joined(sat)) {
		return -ENOTCONN;
	}
	start(sat, true, true, 0);
	return 0;
}

void ignlog_cancel(void)
{
	if (job.state == IGNLOG_BUSY) {
		finish(IGNLOG_IDLE, NULL);
	}
}

void ignlog_progress(struct ignlog_progress *p)
{
	p->state = job.state;
	p->sat = job.sat;
	p->seq = job.want_index ? 0 : job.seq;
	p->done = job.off;
	p->total = job.total;
	p->paused = job.paused;
	p->error = job.error;
}

/* Lock held. */
static bool view_locked(int sat, struct ignlog_store_view *v)
{
	const struct sat_index *ix = &index_of[sat];

	if (!ix->valid) {
		return false;
	}
	*v = ix->v;
	v->now_ms = ix->v.now_ms + (uint32_t)(k_uptime_get() - ix->at);
	v->have = have_mask(sat, v);
	return true;
}

bool ignlog_index(int sat, struct ignlog_store_view *v)
{
	bool ok;

	if (sat < 0 || sat >= NUM_SATS) {
		return false;
	}
	k_mutex_lock(&lock, K_FOREVER);
	ok = view_locked(sat, v);
	k_mutex_unlock(&lock);
	return ok;
}

const uint8_t *ignlog_record(int sat, uint32_t seq, size_t *len)
{
	const struct cached_rec *c;

	if (sat < 0 || sat >= NUM_SATS || !index_of[sat].valid) {
		return NULL;
	}
	k_mutex_lock(&lock, K_FOREVER);
	c = find(sat, index_of[sat].v.store_id, seq);
	if (c && !c->complete) {
		c = NULL;
	}
	k_mutex_unlock(&lock);
	if (c) {
		*len = c->len;
		return c->data;
	}
	return NULL;
}

/*
 * The mcumgr group's backend (common/ignlog/ignlog_mgmt.h), on the mcumgr
 * work queue. Stores are satellite slots 1..8, satellite sat - 1.
 */
int ignlog_be_view(uint8_t sat, struct ignlog_store_view *v)
{
	bool ok;

	if (sat < 1 || sat > NUM_SATS) {
		return -ENOENT;
	}
	k_mutex_lock(&lock, K_FOREVER);
	ok = view_locked(sat - 1, v);
	k_mutex_unlock(&lock);
	return ok ? 0 : -ENOENT;
}

int ignlog_be_read(uint8_t sat, uint32_t seq, size_t off, uint8_t *buf, size_t len,
		   size_t *total)
{
	const struct cached_rec *c;
	int ret = -ENOENT;

	if (sat < 1 || sat > NUM_SATS) {
		return -ENOENT;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (index_of[sat - 1].valid) {
		c = find(sat - 1, index_of[sat - 1].v.store_id, seq);
		if (c && c->complete) {
			*total = c->len;
			ret = off > c->len ? -EINVAL : MIN(len, c->len - off);
			if (ret > 0) {
				memcpy(buf, c->data + off, ret);
			}
		}
	}
	k_mutex_unlock(&lock);
	return ret;
}

int ignlog_be_clear(uint8_t sat)
{
	struct mgmt_req r = { .clear = true, .sat = sat ? sat - 1 : -1 };

	if (sat > NUM_SATS) {
		return -ENOENT;
	}
	/*
	 * Drop the copies now, so that a list right after shows them gone. A
	 * download of them stops at its next answer (no index), or when the
	 * LVGL thread takes the request below.
	 */
	k_mutex_lock(&lock, K_FOREVER);
	for (int s = 0; s < NUM_SATS; s++) {
		if (r.sat < 0 || r.sat == s) {
			index_of[s].valid = false;
			for (int i = 0; i < CACHE_RECS; i++) {
				if (cache[i].sat == s && &cache[i] != job.rec) {
					cache[i].sat = -1;
				}
			}
		}
	}
	k_mutex_unlock(&lock);
	(void)k_msgq_put(&mgmt_q, &r, K_NO_WAIT);
	return 0;
}

int ignlog_be_fetch(uint8_t sat)
{
	struct mgmt_req r = { .sat = sat - 1 };

	if (sat < 1 || sat > NUM_SATS) {
		return -ENOENT;
	}
	if (ctrl_view()->state != CTRL_SAFE) {
		return -EBUSY;
	}
	if (!link_joined(sat - 1)) {
		return -ENOTCONN;
	}
	return k_msgq_put(&mgmt_q, &r, K_NO_WAIT) ? -EBUSY : 0;
}

int ignlog_be_busy(void)
{
	return job.state == IGNLOG_BUSY ? job.sat + 1 : 0;
}

uint8_t ignlog_be_sats(void)
{
	uint8_t mask = 0;

	k_mutex_lock(&lock, K_FOREVER);
	for (int s = 0; s < NUM_SATS; s++) {
		if (index_of[s].valid) {
			mask |= BIT(s);
		}
	}
	k_mutex_unlock(&lock);
	return mask;
}
