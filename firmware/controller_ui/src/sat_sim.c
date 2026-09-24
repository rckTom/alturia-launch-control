/*
 * Simulated satellites and LoRa link.
 *
 * One thread runs all satellites. Each behaves like the pad box described
 * in design/satellite.md closely enough to exercise the controller:
 * link latency and loss, pad key switch, E-STOP, heartbeat timeout,
 * continuity readings, fire pulses, igniters burning open, and valve
 * outputs that are only powered while armed.
 *
 * The link is the real one: each satellite runs the satellite end of the
 * link security layer (lsec_sat), and the controller's ctrl_link talks to
 * them through sat_sim_transport(), an in-process "air" with latency,
 * loss, link-down and the two radio profiles. So a satellite has to be
 * paired before it does anything; "sat pair a" is its pairing gesture.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "link.h"
#include "link_transport.h"
#include "link_wire.h"
#include "lsec.h"
#include "pads.h"
#include "sat_sim.h"

LOG_MODULE_REGISTER(sat_sim, LOG_LEVEL_INF);

#define TICK_MS            20
#define STATUS_PERIOD_MS   250
#define HEARTBEAT_TIMEOUT  1500
#define FIRE_PULSE_MS      1000
#define LATENCY_MIN_MS     80
#define LATENCY_SPAN_MS    120
#define AIR_QUEUE_LEN      8
#define VALVE_MA           450
#define SIM_STACK_SIZE     6144

/* Simulated satellite identities, as the hwinfo id would give them. */
#define SIM_UID(sat)       (0x51A7E11172000000ULL + (sat))

/* Continuity thresholds */
#define SHORT_BELOW_MOHM   200
#define OPEN_ABOVE_MOHM    100000

/* A frame in flight, sent on a radio profile and heard only on the same. */
struct air_frame {
	int64_t due;
	enum link_profile profile;
	uint8_t len;
	uint8_t data[LSEC_MAX_FRAME];
};

struct air_queue {
	struct air_frame f[AIR_QUEUE_LEN];
	int len;
};

struct sim {
	/* Link */
	bool link_up;
	int loss_pct;
	struct lsec_sat lsec;
	struct air_queue to_sat;    /* from the controller */
	struct air_queue to_ctrl;

	/* Satellite */
	struct sat_status st;
	int32_t load_mohm[NUM_CHANNELS]; /* nominal, reported with noise */
	int64_t last_heartbeat;
	int64_t fire_end;
	int64_t next_status;
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
};

/* The controller's end of the air. */
struct ctrl_frame {
	int16_t rssi_dbm;
	uint8_t len;
	uint8_t data[LSEC_MAX_FRAME];
};

K_MSGQ_DEFINE(ctrl_inbox, sizeof(struct ctrl_frame), 8, 4);
static enum link_profile ctrl_profile;

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

/* Put a frame on the air towards one end, unless the link loses it. */
static void air_put(struct sim *s, struct air_queue *q, enum link_profile profile,
		    const void *buf, size_t len)
{
	struct air_frame *f;

	if (packet_lost(s) || q->len >= AIR_QUEUE_LEN) {
		return;
	}

	f = &q->f[q->len++];
	f->due = k_uptime_get() + LATENCY_MIN_MS + rnd_range(0, LATENCY_SPAN_MS);
	f->profile = profile;
	f->len = len;
	memcpy(f->data, buf, len);
}

/* Take the oldest frame that has arrived, if any. */
static bool air_take(struct air_queue *q, int64_t now, struct air_frame *out)
{
	for (int i = 0; i < q->len; i++) {
		if (q->f[i].due <= now) {
			*out = q->f[i];
			memmove(&q->f[i], &q->f[i + 1], (q->len - i - 1) * sizeof(q->f[0]));
			q->len--;
			return true;
		}
	}
	return false;
}

/* Controller side of the air, the transport under ctrl_link. */
static int tp_send(const void *buf, size_t len)
{
	k_mutex_lock(&lock, K_FOREVER);
	for (int sat = 0; sat < NUM_SATS; sat++) {
		air_put(&sims[sat], &sims[sat].to_sat, ctrl_profile, buf, len);
	}
	k_mutex_unlock(&lock);
	return 0;
}

static int tp_recv(void *buf, size_t size, k_timeout_t timeout, int16_t *rssi_dbm,
		   int8_t *snr_db)
{
	struct ctrl_frame f;

	if (k_msgq_get(&ctrl_inbox, &f, timeout)) {
		return -EAGAIN;
	}
	if (f.len > size) {
		return -EMSGSIZE;
	}
	memcpy(buf, f.data, f.len);
	*rssi_dbm = f.rssi_dbm;
	*snr_db = 9;
	return f.len;
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

static const struct link_transport transport = {
	.send = tp_send,
	.recv = tp_recv,
	.set_profile = tp_set_profile,
	.profile = tp_profile,
};

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

static void sat_tick(int sat, int64_t now)
{
	struct sim *s = &sims[sat];
	struct sat_status *st = &s->st;

	struct air_frame f;

	/* Frames from the controller; commands come back as lsec DATA events */
	while (air_take(&s->to_sat, now, &f)) {
		if (f.profile == sat_profile(s)) {
			lsec_sat_rx(&s->lsec, f.data, f.len, now);
		}
	}
	lsec_sat_poll(&s->lsec, now);

	/* Frames to the controller */
	while (air_take(&s->to_ctrl, now, &f)) {
		struct ctrl_frame cf = {.rssi_dbm = st->rssi_dbm, .len = f.len};

		if (f.profile == ctrl_profile) {
			memcpy(cf.data, f.data, f.len);
			(void)k_msgq_put(&ctrl_inbox, &cf, K_NO_WAIT);
		}
	}

	/* Local safety checks, independent of the controller */
	if (st->state != SAT_SAFE) {
		if (!st->key_armed) {
			go_safe(sat, "pad key turned to SAFE");
		} else if (st->estop) {
			go_safe(sat, "E-STOP");
		} else if (now - s->last_heartbeat > HEARTBEAT_TIMEOUT) {
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

	if (now >= s->next_status) {
		uint8_t buf[WIRE_STATUS_LEN];

		s->next_status = now + STATUS_PERIOD_MS;
		update_measurements(s);
		st->seq++;
		/* Sealed; without a session there is nobody to report to */
		(void)lsec_sat_send(&s->lsec, buf, wire_encode_status(st, buf));
	}
}

/* lsec callbacks of the simulated satellites, under lock */
static void sim_send(void *user, const uint8_t *buf, size_t len)
{
	struct sim *s = user;

	air_put(s, &s->to_ctrl, sat_profile(s), buf, len);
}

static void sim_event(void *user, const struct lsec_event *ev)
{
	struct sim *s = user;
	int sat = s - sims;
	struct link_cmd cmd;

	switch (ev->type) {
	case LSEC_EV_DATA:
		if (wire_decode_cmd(ev->data, ev->len, &cmd) == 0) {
			handle_cmd(sat, &cmd, k_uptime_get());
		}
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
	default:
		break;
	}
}

/* Simulated satellites keep their pairing in settings, as the real ones do */
static int sim_store(void *user, uint8_t slot, const struct lsec_pairing *p)
{
	struct sim *s = user;
	char name[16];

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
		int64_t now = k_uptime_get();

		k_mutex_lock(&lock, K_FOREVER);
		for (int sat = 0; sat < NUM_SATS; sat++) {
			sat_tick(sat, now);
		}
		k_mutex_unlock(&lock);

		k_msleep(TICK_MS);
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

		s->link_up = true;
		s->loss_pct = 5;
		s->st.key_armed = true;
		/* Stagger the status packets of the satellites */
		s->next_status = sat * STATUS_PERIOD_MS / NUM_SATS;
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
