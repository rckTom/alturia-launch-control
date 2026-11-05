/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef HELTEC_V4_FEM_H_
#define HELTEC_V4_FEM_H_

#include "radio.h"

/*
 * Power up the Heltec V4's RF front-end, find out which of the two
 * revisions is fitted, and return the matching radio_frontend. Returns
 * NULL if the front-end GPIOs could not be configured.
 */
const struct radio_frontend *heltec_v4_fem_init(void);

#endif /* HELTEC_V4_FEM_H_ */
