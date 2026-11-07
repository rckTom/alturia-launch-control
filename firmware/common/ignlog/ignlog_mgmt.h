/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * The ignition log over mcumgr: group IGNLOG_MGMT_GROUP (ignlog_mgmt.c),
 * the same on the satellite and the controller. Each app provides the
 * backend below. The satellite is store 0 (its own); the controller has
 * one store per satellite slot, 1..8, holding what it downloaded.
 * documentation/ignition_log.md has the commands.
 */

#ifndef IGNLOG_MGMT_H_
#define IGNLOG_MGMT_H_

#include <stddef.h>
#include <stdint.h>

#include "ignlog_rec.h"

#define IGNLOG_MGMT_GROUP 64   /* MGMT_GROUP_ID_PERUSER */

enum ignlog_mgmt_id {
	IGNLOG_MGMT_ID_LIST,   /* read */
	IGNLOG_MGMT_ID_READ,   /* read */
	IGNLOG_MGMT_ID_CLEAR,  /* write */
	IGNLOG_MGMT_ID_FETCH,  /* write, controller only */
};

/* -ENOENT for a store that does not exist or has no index yet. */
int ignlog_be_view(uint8_t sat, struct ignlog_store_view *v);
/* As ignlog_read(): bytes copied, -ENOENT if the record is not held whole. */
int ignlog_be_read(uint8_t sat, uint32_t seq, size_t off, uint8_t *buf, size_t len,
		   size_t *total);
/* Satellite: forget its records (-EBUSY while armed). Controller: drop the copy. */
int ignlog_be_clear(uint8_t sat);
/* Controller: download satellite @p sat's index and records over LoRa. */
int ignlog_be_fetch(uint8_t sat);
/* Satellite: 1 while a pulse is being recorded. Controller: slot being fetched, or 0. */
int ignlog_be_busy(void);
/* Controller: bit n set when slot n + 1 has an index. Satellite: 0. */
uint8_t ignlog_be_sats(void);

#endif /* IGNLOG_MGMT_H_ */
