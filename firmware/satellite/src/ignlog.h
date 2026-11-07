/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * The satellite's ignition log (ignlog.c): the current through each fired
 * channel, kept in RAM over resets. Records are in the format of
 * common/ignlog/ignlog_rec.h.
 */

#ifndef SAT_IGNLOG_H_
#define SAT_IGNLOG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ignlog_rec.h"

/* Set up the sense inputs and keep or start the store. */
int ignlog_init(void);

/*
 * From channels.c: a pulse on the channels in @p mask starts now, and
 * ends. Start turns DIA_EN on before it returns.
 */
void ignlog_start(uint8_t mask, uint32_t pulse_ms);
void ignlog_stop(void);
bool ignlog_recording(void);

/* The records held, oldest first: the newest IGNLOG_LIST_MAX of them. */
void ignlog_view(struct ignlog_store_view *v);
/*
 * Copy up to @p len bytes of record @p seq from @p off. Returns the number
 * copied, with the record's size in @p total; -ENOENT if it is not held.
 */
int ignlog_read(uint32_t seq, size_t off, uint8_t *buf, size_t len, size_t *total);
/* -EBUSY while a pulse is being recorded. */
int ignlog_clear(void);

#endif /* SAT_IGNLOG_H_ */
