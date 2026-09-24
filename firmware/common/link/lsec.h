/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Link security for the controller <-> satellite LoRa link.
 *
 * Pairing, per-boot sessions, the group key and frame protection, as
 * specified in design/radio_security.md. The library owns no thread, timer
 * or radio: the application hands it every received packet and calls
 * lsec_*_poll() periodically, both with the current uptime in ms. It
 * answers through the callbacks in struct lsec_io, which may be called from
 * inside any lsec_* function. None of it is thread safe; serialize calls on
 * one context.
 *
 * Crypto is PSA: ECDH P-256, HMAC-SHA256, HKDF-SHA256 and AES-128-CCM with
 * an 8-byte tag.
 */

#ifndef LSEC_H_
#define LSEC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef LSEC_MAX_SATS
#define LSEC_MAX_SATS 8
#endif

/* Addresses. Satellites use slots 1..LSEC_MAX_SATS. */
#define LSEC_ADDR_CTRL  0
#define LSEC_ADDR_GROUP 0xFF

#define LSEC_MAX_FRAME   255
/* Header (10) plus tag (8) on every sealed frame. */
#define LSEC_OVERHEAD    18
#define LSEC_MAX_PAYLOAD (LSEC_MAX_FRAME - LSEC_OVERHEAD)

#define LSEC_KEY_BYTES 16

/* A poll's body starts with the mask and the slot length. */
#define LSEC_POLL_HDR         2
#define LSEC_MAX_POLL_PAYLOAD (LSEC_MAX_PAYLOAD - LSEC_POLL_HDR)

/* The short authentication string: 4 frames of 6 LEDs, bit n = LED n+1. */
#define LSEC_SAS_FRAMES 4
#define LSEC_SAS_LEDS   6

/* Timing, in ms. */
#define LSEC_ADV_MS          1000   /* satellite PAIR_ADV interval */
#define LSEC_PAIR_RETRY_MS   1000   /* controller retransmit while pairing */
#define LSEC_PAIR_WINDOW_MS  120000 /* satellite pairing mode */
#define LSEC_PAIR_TIMEOUT_MS 90000  /* controller, from pair start */
#define LSEC_PAIR_LINGER_MS  3000   /* satellite answers late CHK_C after pairing */
#define LSEC_HELLO_MS        2000   /* satellite HELLO while it wants a session */
#define LSEC_JOIN_RETRY_MS   10000  /* controller JOIN_REQ to an unjoined slot */
#define LSEC_HELLO_JOIN_MS   1000   /* controller answers HELLOs at most this often */
#define LSEC_RESEND_MS       1000   /* JOIN_OK and GROUP_UPDATE until acked */

/*
 * Session timeouts, the defaults of lsec_sat.session_timeout_ms and
 * lsec_ctrl.peer_timeout_ms. They come from ../link/Kconfig in an app.
 */
#ifdef CONFIG_LINK_SAT_SESSION_TIMEOUT_MS
#define LSEC_SESSION_TIMEOUT_MS CONFIG_LINK_SAT_SESSION_TIMEOUT_MS
#else
#define LSEC_SESSION_TIMEOUT_MS 5000   /* satellite: no addressed poll, drop the session */
#endif
#ifdef CONFIG_LINK_CTRL_PEER_TIMEOUT_MS
#define LSEC_PEER_TIMEOUT_MS CONFIG_LINK_CTRL_PEER_TIMEOUT_MS
#else
#define LSEC_PEER_TIMEOUT_MS 4000      /* controller: no reply, drop the satellite's session */
#endif

/* What a pairing leaves behind on both sides. */
struct lsec_pairing {
	uint64_t sat_uid;
	uint32_t net_id;
	uint8_t slot;
	uint8_t ltk[LSEC_KEY_BYTES];
};

enum lsec_event_type {
	LSEC_EV_ADV,         /* controller: a satellite in pairing mode (uid, rssi) */
	LSEC_EV_SAS,         /* both: show sas[], then confirm or reject */
	LSEC_EV_PAIRED,      /* both: pairing complete and handed to io->store */
	LSEC_EV_PAIR_FAILED, /* both: reason says why */
	LSEC_EV_JOINED,      /* both: session up (slot) */
	LSEC_EV_DATA,        /* both: authenticated payload (slot, group, data, len) */
	LSEC_EV_POLL,        /* satellite: a poll addressed to it (rank, count, slot_ms, data, len) */
	LSEC_EV_LEFT,        /* both: session dropped by its timeout (slot) */
};

enum lsec_fail {
	LSEC_FAIL_TIMEOUT = 1,
	LSEC_FAIL_REJECTED,    /* our user said no */
	LSEC_FAIL_PEER_ABORT,  /* the other side gave up or its user said no */
	LSEC_FAIL_COMMITMENT,  /* satellite's nonce did not match its commitment */
	LSEC_FAIL_CHECK,       /* key confirmation failed: someone is in the middle */
	LSEC_FAIL_CRYPTO,
	LSEC_FAIL_STORE,
};

struct lsec_event {
	enum lsec_event_type type;
	uint8_t slot;          /* controller: the satellite; satellite: its own */
	bool group;            /* DATA: arrived as a group broadcast */
	uint64_t uid;          /* ADV, SAS, PAIRED */
	int16_t rssi_dbm;      /* ADV */
	enum lsec_fail reason; /* PAIR_FAILED */
	uint8_t sas[LSEC_SAS_FRAMES];
	const uint8_t *data;   /* DATA, POLL: valid during the callback only */
	size_t len;
	uint8_t rank;          /* POLL: reply slot, counted among the addressed satellites */
	uint8_t count;         /* POLL: satellites addressed */
	uint8_t slot_ms;       /* POLL: length of one reply slot */
};

struct lsec_io {
	/* Transmit one frame. Called from inside lsec_* functions. */
	void (*send)(void *user, const uint8_t *buf, size_t len);
	void (*event)(void *user, const struct lsec_event *ev);
	/*
	 * Persist a pairing (p != NULL) or erase it (p == NULL). The
	 * controller stores one per slot, the satellite uses slot 0.
	 * A non-zero return fails the pairing.
	 */
	int (*store)(void *user, uint8_t slot, const struct lsec_pairing *p);
	void *user;
};

/* Why received frames were dropped, for diagnostics. */
struct lsec_stats {
	uint32_t rx_ok;
	uint32_t malformed;
	uint32_t foreign;    /* other net, other satellite, not for us */
	uint32_t bad_tag;    /* failed authentication */
	uint32_t replay;
	uint32_t no_session;
};

/* One direction of a session: key plus counter. */
struct lsec_dir {
	uint8_t key[LSEC_KEY_BYTES];
	uint32_t ctr;        /* tx: next to send. rx: last accepted */
	bool seen;           /* rx: ctr is valid */
};

struct lsec_group {
	struct lsec_dir dir; /* controller: tx. satellite: rx */
	uint8_t kid;
	bool valid;
};

/* Pairing handshake state, the same shape on both sides. */
struct lsec_pair {
	int state;
	uint64_t uid;
	uint32_t net_id;
	uint8_t slot;
	uint32_t key_id;              /* psa_key_id_t of our ECDH key pair */
	uint8_t pk_own[65];
	uint8_t pk_peer[65];
	uint8_t n_own[16];
	uint8_t n_peer[16];
	uint8_t commit[16];
	uint8_t mac_key[32];
	uint8_t ltk[LSEC_KEY_BYTES];
	bool user_ok;
	bool check_ok;
	int64_t started;
	int64_t last_tx;
	int64_t last_adv;
	uint8_t last_frame[LSEC_MAX_FRAME];
	size_t last_len;
};

/* Join handshake state for one controller <-> satellite link. */
struct lsec_join {
	uint8_t nc[8];
	uint8_t ns[8];
	bool pending;
	int64_t sent;
};

struct lsec_ctrl_peer {
	struct lsec_pairing pairing;
	bool paired;
	bool joined;
	bool ok_pending;     /* JOIN_OK sent, waiting for JOIN_ACK */
	bool gtk_pending;    /* GROUP_UPDATE sent, waiting for GROUP_ACK */
	int ok_tries;
	int64_t ok_sent;
	int64_t gtk_sent;
	struct lsec_join hs;
	struct lsec_dir tx;  /* controller -> satellite */
	struct lsec_dir rx;  /* satellite -> controller */
	int64_t last_rx;     /* last authenticated frame from the satellite */
};

struct lsec_ctrl {
	struct lsec_io io;
	uint32_t net_id;
	struct lsec_ctrl_peer peer[LSEC_MAX_SATS + 1];   /* index = slot */
	struct lsec_group group;
	bool scanning;
	struct lsec_pair pair;
	struct lsec_stats stats;
	uint32_t peer_timeout_ms;   /* 0: never drop a silent satellite */
};

struct lsec_sat {
	struct lsec_io io;
	uint64_t uid;
	struct lsec_pairing pairing;
	bool paired;
	bool joined;
	struct lsec_join hs;
	uint8_t hs_c2s[LSEC_KEY_BYTES];   /* keys of the pending join */
	uint8_t hs_s2c[LSEC_KEY_BYTES];
	struct lsec_dir tx;
	struct lsec_dir rx;
	struct lsec_group group;
	/* Last unicast frame or addressed poll from the controller. */
	int64_t last_rx;
	int64_t last_hello;
	uint32_t session_timeout_ms;   /* 0: keep the session however long it is quiet */
	struct lsec_pair pair;
	struct lsec_stats stats;
};

/* Controller. net_id identifies this controller's network; keep it stable. */
int lsec_ctrl_init(struct lsec_ctrl *c, const struct lsec_io *io, uint32_t net_id);
int lsec_ctrl_load(struct lsec_ctrl *c, const struct lsec_pairing *p);
void lsec_ctrl_rx(struct lsec_ctrl *c, const uint8_t *buf, size_t len, int16_t rssi_dbm,
		  int64_t now);
void lsec_ctrl_poll(struct lsec_ctrl *c, int64_t now);
/* Seal and transmit to one slot, or to LSEC_ADDR_GROUP. */
int lsec_ctrl_send(struct lsec_ctrl *c, uint8_t dst, const void *data, size_t len);
/*
 * Broadcast a poll. Bit n of @p mask addresses slot n+1; each addressed
 * satellite replies in the slot given by its rank among the set bits, each
 * slot @p slot_ms long. The mask goes out as given, joined or not, so it
 * always matches the order of the records the app put in @p data.
 */
int lsec_ctrl_send_poll(struct lsec_ctrl *c, uint8_t mask, uint8_t slot_ms, const void *data,
			size_t len);
bool lsec_ctrl_joined(const struct lsec_ctrl *c, uint8_t slot);
/* Bit n set when slot n+1 has a session. */
uint8_t lsec_ctrl_joined_mask(const struct lsec_ctrl *c);
const struct lsec_pairing *lsec_ctrl_pairing(const struct lsec_ctrl *c, uint8_t slot);
/* Erase a pairing and move the remaining satellites to a new group key. */
int lsec_ctrl_forget(struct lsec_ctrl *c, uint8_t slot, int64_t now);
/* Report LSEC_EV_ADV for satellites in pairing mode while on. */
void lsec_ctrl_scan(struct lsec_ctrl *c, bool on);
/* Pair with an advertising satellite and give it @p slot. */
int lsec_ctrl_pair_start(struct lsec_ctrl *c, uint64_t uid, uint8_t slot, int64_t now);
/* The user's verdict on the SAS. Rejecting also cancels a pairing at any stage. */
void lsec_ctrl_pair_confirm(struct lsec_ctrl *c, bool accept, int64_t now);
bool lsec_ctrl_pairing_active(const struct lsec_ctrl *c);

/* Satellite. uid is its hardware identity. */
int lsec_sat_init(struct lsec_sat *s, const struct lsec_io *io, uint64_t uid);
int lsec_sat_load(struct lsec_sat *s, const struct lsec_pairing *p);
void lsec_sat_rx(struct lsec_sat *s, const uint8_t *buf, size_t len, int64_t now);
void lsec_sat_poll(struct lsec_sat *s, int64_t now);
/* Seal and transmit to the controller. */
int lsec_sat_send(struct lsec_sat *s, const void *data, size_t len);
bool lsec_sat_joined(const struct lsec_sat *s);
bool lsec_sat_paired(const struct lsec_sat *s);
/* Enter pairing mode. The caller checks that the satellite is SAFE. */
int lsec_sat_pair_start(struct lsec_sat *s, int64_t now);
void lsec_sat_pair_confirm(struct lsec_sat *s, bool accept, int64_t now);
/* True from pair start until the exchange, including its linger, is over. */
bool lsec_sat_pairing_active(const struct lsec_sat *s);
int lsec_sat_unpair(struct lsec_sat *s);

#endif /* LSEC_H_ */
