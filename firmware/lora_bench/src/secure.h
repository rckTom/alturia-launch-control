/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Secure-link mode: the bench as a controller or a satellite on the lsec
 * layer in ../common/link, for pairing, joining and sealed pings over the
 * air between two boards.
 */

#ifndef SECURE_H_
#define SECURE_H_

#include <stdbool.h>
#include <stdint.h>

#include "lsec.h"

enum bench_role {
	ROLE_OFF,   /* plain ping/pong */
	ROLE_CTRL,
	ROLE_SAT,
};

/* What the display and "bench link" show. Written by the secure loop only. */
struct secure_status {
	enum bench_role role;
	uint64_t uid;          /* satellite: own; controller: the one paired */
	bool paired;
	bool joined;
	bool scanning;
	bool pairing;
	bool sas_shown;
	uint8_t sas[LSEC_SAS_FRAMES];
	const char *note;      /* the last notable event, for one line */
	struct lsec_stats stats;

	bool pinging;
	uint32_t ping_sent;
	uint32_t ping_total;
	uint32_t pongs;
	uint32_t group_pongs;
	uint32_t rtt_ms;

	uint8_t slot;          /* the satellite's slot, 0 if none */
};

/* Role saved in settings; read it after settings_subsys_init(). */
enum bench_role secure_role_load(void);
int secure_role_save(enum bench_role role);

/* Run the secure loop for @p role. Does not return. */
void secure_main(enum bench_role role);

/* Requests from the shell and the button, executed by the loop. */
enum secure_req_type {
	REQ_CONFIRM,
	REQ_REJECT,
	REQ_PAIR,        /* satellite: pairing mode. controller: pair with uid (0 = first seen) into slot arg */
	REQ_SCAN,        /* controller: arg = on/off */
	REQ_UNPAIR,
	REQ_REPLAY,
	REQ_PING,        /* controller: arg = count, arg2 = interval */
	REQ_STOP,
	REQ_POLL,        /* controller: arg = count, arg2 = interval, uid = phantoms | reply_len << 8 */
	REQ_JAM,         /* arg = ms of back-to-back raw frames, without listening first */
};

/* PRG button edges, from the input callback. */
void secure_button(bool pressed);

int secure_request(enum secure_req_type type, uint64_t uid, uint32_t arg, uint32_t arg2);
const struct secure_status *secure_status(void);

#endif /* SECURE_H_ */
