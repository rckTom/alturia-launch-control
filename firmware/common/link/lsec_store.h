/* SPDX-License-Identifier: GPL-3.0-only */

/* Persistence of lsec pairings through Zephyr settings. */

#ifndef LSEC_STORE_H_
#define LSEC_STORE_H_

#include "lsec.h"

/* @p idx is the slot on the controller, 0 for a satellite's own pairing. */
typedef void (*lsec_store_cb)(uint8_t idx, const struct lsec_pairing *p, void *user);

/*
 * Hand every stored pairing to @p cb. With @p net_id non-NULL (the
 * controller), also load the network id, creating and saving one on first
 * boot. Needs settings_subsys_init() first. Returns the number of
 * pairings or a negative errno.
 */
int lsec_store_load(uint32_t *net_id, lsec_store_cb cb, void *user);

/* Save (p != NULL) or erase a pairing. Fits struct lsec_io.store. */
int lsec_store_save(void *user, uint8_t slot, const struct lsec_pairing *p);

/* Erase every pairing, keeping the network id. */
int lsec_store_clear(void);

#endif /* LSEC_STORE_H_ */
