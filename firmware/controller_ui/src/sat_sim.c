/*
 * Simulated satellites and LoRa link.
 *
 * One thread runs all satellites. Each behaves like the pad box described
 * in design/satellite.md closely enough to exercise the controller:
 * link loss, pad key switch, E-STOP, heartbeat timeout, continuity
 * readings, fire pulses, igniters burning open, and valve outputs that are
 * only powered while armed.
 *
 * The link is the real one: each satellite runs the satellite end of the
 * link security layer (lsec_sat), and the controller's ctrl_link talks to
 * them through sat_sim_transport(), an in-process "air" with loss,
 * link-down and the two radio profiles. So a satellite has to be paired
 * before it does anything; "sat pair a" is its pairing gesture.
 *
 * The air keeps time like the radio does: every frame is on the air for
 * its LoRa airtime, frames that overlap on the same profile are lost, and
 * nobody hears while transmitting. A satellite answers a poll in its reply slot, after
 * checking the channel is clear, as sat_link does on the real board. So a
 * mistake in the slot timing shows up here as collisions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "link.h"
#include "link_access.h"
#include "link_sched.h"
#include "link_transport.h"
#include "sat_wire.h"
#include "lsec.h"
#include "pads.h"
#include "sat_sim.h"

LOG_MODULE_REGISTER(sat_sim, LOG_LEVEL_INF);

#define TICK_MS            20
#define FIRE_PULSE_MS      1000
#define VALVE_MA           450
#define SIM_STACK_SIZE     6144

/* The simulated modem: the settings of ../common/lora/radio.c. */
#define LINK_SF 7
#define PAIR_SF 8
#define BW_KHZ  250

/* Frames the air remembers, for delivery and for collisions. */
#define AIR_RECORDS 32
#define FROM_CTRL   (-1)

/* Simulated satellite identities, as the hwinfo id would give them. */
#define SIM_UID(sat)       (0x51A7E11172000000ULL + (sat))

/* Continuity thresholds */
#define SHORT_BELOW_MOHM   200
#define OPEN_ABOVE_MOHM    100000

/* One transmission, from its first to its last symbol. */
struct air_tx {
	int64_t start_us;
	int64_t end_us;
	int from;                 /* FROM_CTRL or a satellite */
	enum link_profile profile;
	bool delivered;           /* satellite frames: handed to the controller */
	uint8_t len;
	uint8_t data[LSEC_MAX_FRAME];
};

struct sim {
	/* Link */
	bool link_up;
	int loss_pct;
	struct lsec_sat lsec;
	struct {
		bool pending;
		int64_t at_us;
		size_t len;
		uint8_t buf[WIRE_STATUS_LEN];
	} reply;
	uint32_t replies;
	uint32_t collisions;   /* its frames lost to an overlap at the controller */
	uint32_t slot_busy;    /* reply slots let pass: the channel was busy */
	uint32_t slot_late;
	uint32_t busy_drops;   /* other frames dropped: the channel was busy */

	/* Satellite */
	struct sat_status st;
	int32_t load_mohm[NUM_CHANNELS]; /* nominal, reported with noise */
	int64_t last_heartbeat;
	int64_t fire_end;
};

/* Everything below is guarded by lock */
static K_MUTEX_DEFINE(lock);

static struct sim sims[NUM_SATS] = {
	{
		/* Pad 1 hybrid on CH1/5/6, pads 2-4 on CH2-4. CH3 not hooked up. */
		.load_mohm = {1200, 900, RES_OPEN_MOHM, 2100, 28000, 26000},
		.st = {.vbat_mv = 12400},
	},
	{
		/* Pad 5 hybrid on CH1/5/6, pads 6-7 on CH2-3. CH4 unused. */
		.load_mohm = {1500, 1100, 1800, RES_OPEN_MOHM, 27000, 25000},
		.st = {.vbat_mv = 12150},
	},
	/* The others: igniters on CH1-4, CH5-6 unused (see sat_sim_transport) */
};

static struct air_tx air[AIR_RECORDS];
static int air_next;

/* Reception time of the frame a satellite's lsec is handling. */
static int64_t sim_rx_time_us;
/* sim_send is sending a slot reply, which has had its listen already. */
static bool sim_in_slot;

/* The controller's end of the air. */
struct ctrl_frame {
	int16_t rssi_dbm;
	uint8_t len;
	int64_t time_us;
	uint8_t data[LSEC_MAX_FRAME];
};

K_MSGQ_DEFINE(ctrl_inbox, sizeof(struct ctrl_frame), 12, 4);
static enum link_profile ctrl_profile;
/* Wakes the satellite thread when something new is on the air. */
static K_SEM_DEFINE(wake, 0, 1);

static uint32_t rng_state = 0x2545f491;

static uint32_t rnd(void)
{
	/* xorshift32, good enough for noise and packet loss */
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static int rnd_range(int lo, int hi)
{
	return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

static bool packet_lost(const struct sim *s)
{
	return !s->link_up || rnd_range(0, 99) < s->loss_pct;
}

static enum link_profile sat_profile(const struct sim *s)
{
	return lsec_sat_pairing_active(&s->lsec) ? LINK_PROFILE_PAIR : LINK_PROFILE_LINK;
}

static int64_t airtime_us(enum link_profile p, size_t len)
{
	return (int64_t)link_airtime_ms(len, p == LINK_PROFILE_PAIR ? PAIR_SF : LINK_SF, BW_KHZ) *
	       1000;
}

static struct air_tx *air_add(int from, enum link_profile p, const void *buf, size_t len)
{
	struct air_tx *t = &air[air_next++ % AIR_RECORDS];

	t->start_us = link_now_us();
	t->end_us = t->start_us + airtime_us(p, len);
	t->from = from;
	t->profile = p;
	t->delivered = false;
	t->len = len;
	memcpy(t->data, buf, len);
	return t;
}

/* Was anyone but @p who on the air at some point in [t0, t1]? */
static bool air_busy(int64_t t0, int64_t t1, int who)
{
	for (int i = 0; i < AIR_RECORDS; i++) {
		const struct air_tx *t = &air[i];

		if (t->len && t->from != who && t->start_us <= t1 && t->end_us > t0) {
			return true;
		}
	}
	return false;
}

/* Was @p who itself transmitting at some point in [t0, t1]? */
static bool air_sending(int64_t t0, int64_t t1, int who)
{
	for (int i = 0; i < AIR_RECORDS; i++) {
		const struct air_tx *t = &air[i];

		if (t->len && t->from == who && t->start_us < t1 && t->end_us > t0) {
			return true;
		}
	}
	return false;
}

/* Controller side of the air, the transport under ctrl_link. */
static int tp_send(const void *buf, size_t len)
{
	struct air_tx *t;
	int64_t start;
	int64_t end;

	k_mutex_lock(&lock, K_FOREVER);
	t = air_add(FROM_CTRL, ctrl_profile, buf, len);
	start = t->start_us;
	end = t->end_us;
	k_mutex_unlock(&lock);

	/* Like lora_send(): back when the frame has left the antenna. */
	k_sleep(K_USEC(end - start));

	k_mutex_lock(&lock, K_FOREVER);
	for (int sat = 0; sat < NUM_SATS; sat++) {
		struct sim *s = &sims[sat];

		/* A satellite that was transmitting meanwhile heard nothing. */
		if (packet_lost(s) || sat_profile(s) != ctrl_profile ||
		    air_sending(start, end, sat)) {
			continue;
		}
		sim_rx_time_us = end;
		lsec_sat_rx(&s->lsec, buf, len, k_uptime_get());
	}
	k_mutex_unlock(&lock);

	k_sem_give(&wake);
	return 0;
}

static int tp_recv(void *buf, size_t size, k_timeout_t timeout, struct link_rx_info *info)
{
	struct ctrl_frame f;

	if (k_msgq_get(&ctrl_inbox, &f, timeout)) {
		return -EAGAIN;
	}
	if (f.len > size) {
		return -EMSGSIZE;
	}
	memcpy(buf, f.data, f.len);
	info->rssi_dbm = f.rssi_dbm;
	info->snr_db = 9;
	info->time_us = f.time_us;
	return f.len;
}

static void tp_wake(void)
{
	struct ctrl_frame f = {.len = 0};

	(void)k_msgq_put(&ctrl_inbox, &f, K_NO_WAIT);
}

static int tp_set_profile(enum link_profile p)
{
	ctrl_profile = p;
	return 0;
}

static enum link_profile tp_profile(void)
{
	return ctrl_profile;
}

static int tp_channel_clear(uint32_t listen_us, int16_t *peak_dbm)
{
	int64_t t0 = link_now_us();
	bool busy;

	k_sleep(K_USEC(listen_us));

	k_mutex_lock(&lock, K_FOREVER);
	busy = air_busy(t0, link_now_us(), FROM_CTRL);
	k_mutex_unlock(&lock);

	if (peak_dbm) {
		*peak_dbm = busy ? -70 : -110;
	}
	return !busy;
}

static uint32_t tp_airtime_ms(size_t len)
{
	return link_airtime_ms(len, ctrl_profile == LINK_PROFILE_PAIR ? PAIR_SF : LINK_SF,
			       BW_KHZ);
}

static const struct link_transport transport = {
	.send = tp_send,
	.recv = tp_recv,
	.wake = tp_wake,
	.set_profile = tp_set_profile,
	.profile = tp_profile,
	.channel_clear = tp_channel_clear,
	.airtime_ms = tp_airtime_ms,
};

/* Hand the satellites' frames that have finished to the controller. */
static void deliver_to_ctrl(int64_t now_us, int64_t *next_us)
{
	for (int i = 0; i < AIR_RECORDS; i++) {
		struct air_tx *t = &air[i];
		struct sim *s;

		if (!t->len || t->from == FROM_CTRL || t->delivered) {
			continue;
		}
		if (t->end_us > now_us) {
			*next_us = MIN(*next_us, t->end_us);
			continue;
		}

		t->delivered = true;
		s = &sims[t->from];

		/*
		 * Lost to an overlap on the same spreading factor (others are
		 * close to orthogonal), or to the controller's own sending.
		 */
		for (int j = 0; j < AIR_RECORDS; j++) {
			const struct air_tx *o = &air[j];

			if (j != i && o->len && o->from != t->from &&
			    (o->from == FROM_CTRL || o->profile == t->profile) &&
			    o->start_us < t->end_us && o->end_us > t->start_us) {
				s->collisions++;
				LOG_WRN("sat %c: frame lost in a collision", sat_name(t->from));
				goto next;
			}
		}

		if (!packet_lost(s) && t->profile == ctrl_profile) {
			struct ctrl_frame cf = {
				.rssi_dbm = s->st.rssi_dbm,
				.len = t->len,
				.time_us = t->end_us,
			};

			memcpy(cf.data, t->data, t->len);
			(void)k_msgq_put(&ctrl_inbox, &cf, K_NO_WAIT);
		}
next:;
	}
}

static void go_safe(int sat, const char *why)
{
	struct sat_status *st = &sims[sat].st;

	if (st->state != SAT_SAFE) {
		LOG_WRN("sat %c -> SAFE (%s)", sat_name(sat), why);
	}
	st->state = SAT_SAFE;
	st->armed_mask = 0;
	st->firing_mask = 0;
	st->valve_mask = 0;
}

static void handle_cmd(int sat, const struct link_cmd *cmd, int64_t now)
{
	struct sim *s = &sims[sat];
	struct sat_status *st = &s->st;

	s->last_heartbeat = now;

	/* A request is executed once; a repeat is only answered. */
	if (cmd->id != 0 && cmd->id != st->cmd_id) {
		st->cmd_id = cmd->id;

		switch (cmd->type) {
		case CMD_HEARTBEAT:
			break;
		case CMD_ARM:
			if (st->state != SAT_SAFE) {
				break;
			}
			if (!st->key_armed || st->estop) {
				LOG_WRN("sat %c: ARM refused, pad key %s, E-STOP %s", sat_name(sat),
					st->key_armed ? "armed" : "safe",
					st->estop ? "pressed" : "released");
				return;
			}
			/*
			 * Pre-arm check (spec 11.3): measure disarmed and refuse if an
			 * output reads shorted, since a shorted FET plus the closed arm
			 * relay would fire. The sim treats any short as a shorted FET.
			 */
			st->arm_refused = 0;
			for (int ch = 0; ch < NUM_CHANNELS; ch++) {
				if ((cmd->mask & BIT(ch)) && s->load_mohm[ch] < SHORT_BELOW_MOHM) {
					st->arm_refused |= BIT(ch);
				}
			}
			if (st->arm_refused) {
				LOG_WRN("sat %c: ARM refused, shorted outputs 0x%02x", sat_name(sat),
					st->arm_refused);
				return;
			}
			st->state = SAT_ARMED;
			st->armed_mask = cmd->mask;
			LOG_INF("sat %c ARMED, mask 0x%02x", sat_name(sat), cmd->mask);
			break;
		case CMD_DISARM:
			st->arm_refused = 0;
			go_safe(sat, "disarm command");
			return;
		case CMD_FIRE:
			if (st->state != SAT_ARMED || (cmd->mask & ~st->armed_mask) != 0) {
				LOG_WRN("sat %c: FIRE refused, mask 0x%02x", sat_name(sat), cmd->mask);
				return;
			}
			st->state = SAT_FIRING;
			st->firing_mask = cmd->mask;
			st->fire_count++;
			s->fire_end = now + FIRE_PULSE_MS;
			LOG_INF("sat %c FIRING, mask 0x%02x", sat_name(sat), cmd->mask);
			break;
		}
	}

	if (st->state != SAT_SAFE) {
		uint8_t valves = cmd->valves & st->armed_mask;

		if (valves != st->valve_mask) {
			LOG_INF("sat %c valves 0x%02x", sat_name(sat), valves);
		}
		st->valve_mask = valves;
	}
}

static void update_measurements(struct sim *s)
{
	struct sat_status *st = &s->st;
	int32_t rail_ma = 0;

	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		int32_t r = s->load_mohm[ch];

		if (r == RES_OPEN_MOHM || r > OPEN_ABOVE_MOHM) {
			st->res_mohm[ch] = RES_OPEN_MOHM;
			st->cont[ch] = CONT_OPEN;
		} else {
			/* +-2 % noise */
			r += r * rnd_range(-20, 20) / 1000;
			st->res_mohm[ch] = r;
			st->cont[ch] = r < SHORT_BELOW_MOHM ? CONT_SHORT : CONT_OK;
		}

		if ((st->firing_mask & BIT(ch)) && s->load_mohm[ch] != RES_OPEN_MOHM) {
			/* I = V / (R_load + ~0.3 Ohm wiring), capped by the fuse */
			int32_t ma = (int32_t)((int64_t)st->vbat_mv * 1000 /
					       (s->load_mohm[ch] + 300));
			rail_ma += MIN(ma, 15000);
		}
		if (st->valve_mask & BIT(ch)) {
			rail_ma += VALVE_MA;
		}
	}

	st->rail_ma = rail_ma + rnd_range(0, 3);
	st->vbat_mv -= rnd_range(0, 1) + (rail_ma > 0 ? 1 : 0); /* slow drain */
	st->rssi_dbm = -78 + rnd_range(-4, 4);
	st->snr_db = 9 + rnd_range(-2, 2);
}

/* The satellite's slot has come: listen, then answer or let it pass. */
static void send_reply(int sat, int64_t now_us)
{
	struct sim *s = &sims[sat];

	s->reply.pending = false;

	if (now_us - s->reply.at_us > LINK_SLOT_GUARD_MS * 1000 / 2) {
		s->slot_late++;
		return;
	}
	if (air_busy(now_us, now_us, sat)) {
		s->slot_busy++;
		return;
	}

	sim_in_slot = true;
	if (lsec_sat_send(&s->lsec, s->reply.buf, s->reply.len) == 0) {
		s->replies++;
	}
	sim_in_slot = false;
}

static void sat_tick(int sat, int64_t now_us, int64_t *next_us)
{
	struct sim *s = &sims[sat];
	struct sat_status *st = &s->st;
	int64_t now = now_us / 1000;

	if (s->reply.pending) {
		if (now_us >= s->reply.at_us) {
			send_reply(sat, now_us);
		} else {
			*next_us = MIN(*next_us, s->reply.at_us);
		}
	}
	/* HELLO and the like would land in someone's slot. */
	if (!s->reply.pending) {
		lsec_sat_poll(&s->lsec, now);
	}

	/* Local safety checks, independent of the controller */
	if (st->state != SAT_SAFE) {
		if (!st->key_armed) {
			go_safe(sat, "pad key turned to SAFE");
		} else if (st->estop) {
			go_safe(sat, "E-STOP");
		} else if (now - s->last_heartbeat > LINK_SAT_SAFE_TIMEOUT_MS) {
			go_safe(sat, "heartbeat lost");
		}
	}

	if (st->state == SAT_FIRING && now >= s->fire_end) {
		for (int ch = 0; ch < NUM_CHANNELS; ch++) {
			if (st->firing_mask & BIT(ch)) {
				/* The bridgewire is gone after a successful firing */
				s->load_mohm[ch] = RES_OPEN_MOHM;
			}
		}
		st->firing_mask = 0;
		st->state = SAT_ARMED;
		LOG_INF("sat %c fire pulse complete", sat_name(sat));
	}
}

/* lsec callbacks of the simulated satellites, under lock */
static void sim_send(void *user, const uint8_t *buf, size_t len)
{
	struct sim *s = user;
	int sat = s - sims;
	int64_t now = link_now_us();

	/* Outside its slot a satellite only talks on a clear channel. */
	if (!sim_in_slot && air_busy(now, now, sat)) {
		s->busy_drops++;
		return;
	}

	air_add(sat, sat_profile(s), buf, len);
	k_sem_give(&wake);
}

static void sim_event(void *user, const struct lsec_event *ev)
{
	struct sim *s = user;
	int sat = s - sims;
	struct link_cmd cmd;
	bool full = false;

	switch (ev->type) {
	case LSEC_EV_POLL:
		if (wire_decode_poll(ev->data, ev->len, ev->rank, ev->count, &cmd, &full) == 0) {
			handle_cmd(sat, &cmd, k_uptime_get());
		}
		/* The status reflects the command just applied. */
		update_measurements(s);
		s->st.seq++;
		s->reply.len = wire_encode_status(&s->st, full, s->reply.buf);
		s->reply.at_us = link_slot_start_us(sim_rx_time_us, ev->rank, ev->slot_ms);
		s->reply.pending = true;
		break;
	case LSEC_EV_SAS: {
		char rows[LSEC_SAS_FRAMES][LSEC_SAS_LEDS + 1];

		for (int r = 0; r < LSEC_SAS_FRAMES; r++) {
			for (int i = 0; i < LSEC_SAS_LEDS; i++) {
				rows[r][i] = (ev->sas[r] & BIT(i)) ? '#' : '.';
			}
			rows[r][LSEC_SAS_LEDS] = '\0';
		}
		LOG_WRN("sim: satellite %c LEDs show  1 %s  2 %s  3 %s  4 %s", sat_name(sat),
			rows[0], rows[1], rows[2], rows[3]);
		LOG_WRN("sim: \"sat confirm %c\" if the controller shows the same", 'a' + sat);
		break;
	}
	case LSEC_EV_PAIRED:
		LOG_INF("sim: satellite %c paired as slot %u", sat_name(sat), ev->slot);
		break;
	case LSEC_EV_PAIR_FAILED:
		LOG_WRN("sim: satellite %c pairing failed (%d)", sat_name(sat), ev->reason);
		break;
	case LSEC_EV_JOINED:
		LOG_INF("sim: satellite %c session up", sat_name(sat));
		break;
	case LSEC_EV_LEFT:
		LOG_WRN("sim: satellite %c not polled for %u ms, session dropped", sat_name(sat),
			LSEC_SESSION_TIMEOUT_MS);
		break;
	default:
		break;
	}
}

/* Simulated satellites keep their pairing in settings, as the real ones do */
static int sim_store(void *user, uint8_t slot, const struct lsec_pairing *p)
{
	struct sim *s = user;
	char name[20];

	ARG_UNUSED(slot);
	snprintf(name, sizeof(name), "sim/pair%d", (int)(s - sims));
	return p ? settings_save_one(name, p, sizeof(*p)) : settings_delete(name);
}

static int sim_load(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
		    void *param)
{
	struct lsec_pairing p;
	int sat;

	ARG_UNUSED(param);
	if (strncmp(key, "pair", 4) != 0) {
		return 0;
	}
	sat = atoi(key + 4);
	if (sat >= 0 && sat < NUM_SATS && len == sizeof(p) &&
	    read_cb(cb_arg, &p, len) == (ssize_t)len) {
		lsec_sat_load(&sims[sat].lsec, &p);
	}
	return 0;
}

static void sat_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		int64_t now_us;
		int64_t next_us;

		k_mutex_lock(&lock, K_FOREVER);
		now_us = link_now_us();
		next_us = now_us + TICK_MS * 1000;
		deliver_to_ctrl(now_us, &next_us);
		for (int sat = 0; sat < NUM_SATS; sat++) {
			sat_tick(sat, now_us, &next_us);
		}
		k_mutex_unlock(&lock);

		/* Until the next slot or delivery, or until the air changes. */
		(void)k_sem_take(&wake, K_TIMEOUT_ABS_US(next_us));
	}
}

static K_THREAD_STACK_DEFINE(sim_stack, SIM_STACK_SIZE);
static struct k_thread sim_thread;

const struct link_transport *sat_sim_transport(void)
{
	static bool started;

	if (started) {
		return &transport;
	}
	started = true;

	k_mutex_lock(&lock, K_FOREVER);
	for (int sat = 0; sat < NUM_SATS; sat++) {
		struct sim *s = &sims[sat];
		const struct lsec_io io = {
			.send = sim_send,
			.event = sim_event,
			.store = sim_store,
			.user = s,
		};

		if (!s->st.vbat_mv) {
			for (int ch = 0; ch < NUM_CHANNELS; ch++) {
				s->load_mohm[ch] = ch < 4 ? 1100 + 150 * ((sat + ch) % 5)
							  : RES_OPEN_MOHM;
			}
			s->st.vbat_mv = 12300 - 60 * sat;
		}
		s->link_up = true;
		s->loss_pct = 5;
		s->st.key_armed = true;
		lsec_sat_init(&s->lsec, &io, SIM_UID(sat));
	}
	settings_load_subtree_direct("sim", sim_load, NULL);
	k_mutex_unlock(&lock);

	k_thread_create(&sim_thread, sim_stack, K_THREAD_STACK_SIZEOF(sim_stack), sat_thread,
			NULL, NULL, NULL, 5, 0, K_NO_WAIT);
	k_thread_name_set(&sim_thread, "sat_sim");

	return &transport;
}

void sat_sim_pair(int sat)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (sims[sat].st.state != SAT_SAFE) {
		LOG_WRN("sim: satellite %c is armed, no pairing", sat_name(sat));
	} else {
		LOG_INF("sim: satellite %c in pairing mode", sat_name(sat));
		lsec_sat_pair_start(&sims[sat].lsec, k_uptime_get());
	}
	k_mutex_unlock(&lock);
}

void sat_sim_pair_confirm(int sat, bool accept)
{
	k_mutex_lock(&lock, K_FOREVER);
	lsec_sat_pair_confirm(&sims[sat].lsec, accept, k_uptime_get());
	k_mutex_unlock(&lock);
}

void sat_sim_unpair(int sat)
{
	k_mutex_lock(&lock, K_FOREVER);
	lsec_sat_unpair(&sims[sat].lsec);
	k_mutex_unlock(&lock);
}

void sat_sim_set_link(int sat, bool up)
{
	k_mutex_lock(&lock, K_FOREVER);
	sims[sat].link_up = up;
	k_mutex_unlock(&lock);
}

/* native_sim keys F1, F2, ... toggle the link of satellite A, B, ... */
static void sim_input(struct input_event *evt, void *user_data)
{
	int sat = evt->code - INPUT_KEY_F1;
	bool up;

	ARG_UNUSED(user_data);
	if (evt->type != INPUT_EV_KEY || !evt->value || sat < 0 || sat >= NUM_SATS) {
		return;
	}

	k_mutex_lock(&lock, K_FOREVER);
	up = sims[sat].link_up = !sims[sat].link_up;
	k_mutex_unlock(&lock);
	LOG_WRN("sim: satellite %c link %s", sat_name(sat), up ? "UP" : "DOWN");
}

INPUT_CALLBACK_DEFINE(NULL, sim_input, NULL);

void sat_sim_set_loss(int sat, int percent)
{
	k_mutex_lock(&lock, K_FOREVER);
	sims[sat].loss_pct = CLAMP(percent, 0, 100);
	k_mutex_unlock(&lock);
}

void sat_sim_set_key(int sat, bool armed)
{
	k_mutex_lock(&lock, K_FOREVER);
	sims[sat].st.key_armed = armed;
	k_mutex_unlock(&lock);
}

void sat_sim_set_estop(int sat, bool pressed)
{
	k_mutex_lock(&lock, K_FOREVER);
	sims[sat].st.estop = pressed;
	k_mutex_unlock(&lock);
}

void sat_sim_set_res(int sat, int ch, int32_t mohm)
{
	k_mutex_lock(&lock, K_FOREVER);
	sims[sat].load_mohm[ch] = mohm;
	k_mutex_unlock(&lock);
}

void sat_sim_set_vbat(int sat, int32_t mv)
{
	k_mutex_lock(&lock, K_FOREVER);
	sims[sat].st.vbat_mv = mv;
	k_mutex_unlock(&lock);
}

void sat_sim_print(int sat, void (*print)(const char *fmt, ...))
{
	static const char *const state_names[] = {"SAFE", "ARMED", "FIRING"};
	const struct sim *s = &sims[sat];

	k_mutex_lock(&lock, K_FOREVER);
	print("sat %c: %s, pad key %s, E-STOP %s, link %s, loss %d %%\n", sat_name(sat),
	      state_names[s->st.state], s->st.key_armed ? "ARM" : "SAFE",
	      s->st.estop ? "pressed" : "released", s->link_up ? "up" : "down",
	      s->loss_pct);
	print("  %s, %s%s, uid %016llx\n", lsec_sat_paired(&s->lsec) ? "paired" : "not paired",
	      lsec_sat_joined(&s->lsec) ? "joined" : "no session",
	      lsec_sat_pairing_active(&s->lsec) ? ", pairing mode" : "", s->lsec.uid);
	print("  %u replies, %u lost in collisions, slots skipped: %u busy %u late, "
	      "%u frames dropped on a busy channel\n",
	      s->replies, s->collisions, s->slot_busy, s->slot_late, s->busy_drops);
	print("  vbat %d mV, rail %d mA, armed 0x%02x, valves 0x%02x\n", s->st.vbat_mv,
	      s->st.rail_ma, s->st.armed_mask, s->st.valve_mask);
	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		if (s->load_mohm[ch] == RES_OPEN_MOHM) {
			print("  CH%d: open\n", ch + 1);
		} else {
			print("  CH%d: %d mOhm\n", ch + 1, s->load_mohm[ch]);
		}
	}
	k_mutex_unlock(&lock);
}
