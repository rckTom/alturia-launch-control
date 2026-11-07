/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * The satellites' ignition logs, downloaded over LoRa (ignlog.c): each
 * satellite's index, and a cache of the records asked for. The download
 * runs one log poll at a time, only while nothing is armed and no command
 * waits for its acknowledgement; see documentation/ignition_log.md. The
 * records are in the format of common/ignlog/ignlog_rec.h, byte for byte
 * as the satellite holds them, and are served again over mcumgr.
 *
 * Everything here runs on the LVGL thread (ctrl_tick() and the views),
 * except ignlog_rx() on the link thread and the mcumgr backend on the
 * mcumgr work queue.
 */

#ifndef CTRL_IGNLOG_H_
#define CTRL_IGNLOG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ignlog_rec.h"

enum ignlog_job_state {
	IGNLOG_IDLE,
	IGNLOG_BUSY,
	IGNLOG_DONE,
	IGNLOG_FAILED,
};

struct ignlog_progress {
	enum ignlog_job_state state;
	int sat;             /* -1: no download yet */
	uint32_t seq;        /* the record being downloaded, 0 for the index */
	size_t done, total;  /* bytes of it */
	bool paused;         /* waiting for the link to be free of commands */
	const char *error;   /* IGNLOG_FAILED: why */
};

/* A log reply from satellite @p sat, from the link thread. */
void ignlog_rx(int sat, const uint8_t *buf, size_t len);

/*
 * Run the download, from ctrl_tick(). @p allowed while the controller is
 * SAFE and no command waits for its acknowledgement; otherwise no new log
 * poll goes out.
 */
void ignlog_tick(int64_t now, bool allowed);

/* Each replaces the download running. -ENOTCONN if @p sat is not joined. */
int ignlog_get_index(int sat);
int ignlog_get_record(int sat, uint32_t seq);
/* The index, then every record not yet held. */
int ignlog_get_all(int sat);
void ignlog_cancel(void);

void ignlog_progress(struct ignlog_progress *p);

/*
 * The last index of satellite @p sat, false if there is none. now_ms is
 * moved on to the satellite's uptime now; have marks the records held.
 */
bool ignlog_index(int sat, struct ignlog_store_view *v);

/*
 * A record held whole, or NULL. The pointer stays valid on the LVGL thread
 * until the next ignlog_tick().
 */
const uint8_t *ignlog_record(int sat, uint32_t seq, size_t *len);

#endif /* CTRL_IGNLOG_H_ */
