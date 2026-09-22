/*
 * Simulated satellites and LoRa link.
 *
 * One thread runs all satellites. Each behaves like the pad box described
 * in design/satellite.md closely enough to exercise the controller:
 * link latency and loss, pad key switch, E-STOP, heartbeat timeout,
 * continuity readings, fire pulses, igniters burning open, and valve
 * outputs that are only powered while armed.
 */

#include <string.h>

#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "link.h"
#include "pads.h"
#include "sat_sim.h"

LOG_MODULE_REGISTER(sat_sim, LOG_LEVEL_INF);

#define TICK_MS            20
#define STATUS_PERIOD_MS   250
#define HEARTBEAT_TIMEOUT  1500
#define FIRE_PULSE_MS      1000
#define LATENCY_MIN_MS     80
#define LATENCY_SPAN_MS    120
#define CMD_QUEUE_LEN      8
#define VALVE_MA           450

/* Continuity thresholds */
#define SHORT_BELOW_MOHM   200
#define OPEN_ABOVE_MOHM    100000

struct pending_cmd {
	struct link_cmd cmd;
	int64_t due;
};

struct sim {
	/* Link */
	bool link_up;
	int loss_pct;
	struct pending_cmd cmd_q[CMD_QUEUE_LEN];
	int cmd_q_len;
	struct sat_status rx_status;
	int64_t rx_time;
	struct sat_status tx_status;
	int64_t tx_due;

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

void link_send(int sat, const struct link_cmd *cmd)
{
	struct sim *s = &sims[sat];

	k_mutex_lock(&lock, K_FOREVER);
	if (!packet_lost(s) && s->cmd_q_len < CMD_QUEUE_LEN) {
		s->cmd_q[s->cmd_q_len].cmd = *cmd;
		s->cmd_q[s->cmd_q_len].due = k_uptime_get() + LATENCY_MIN_MS +
					     rnd_range(0, LATENCY_SPAN_MS);
		s->cmd_q_len++;
	}
	k_mutex_unlock(&lock);
}

int64_t link_last_status(int sat, struct sat_status *out)
{
	int64_t t;

	k_mutex_lock(&lock, K_FOREVER);
	*out = sims[sat].rx_status;
	t = sims[sat].rx_time;
	k_mutex_unlock(&lock);
	return t;
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

	/* Deliver due commands in order */
	for (int i = 0; i < s->cmd_q_len;) {
		if (s->cmd_q[i].due <= now) {
			struct link_cmd cmd = s->cmd_q[i].cmd;

			memmove(&s->cmd_q[i], &s->cmd_q[i + 1],
				(s->cmd_q_len - i - 1) * sizeof(s->cmd_q[0]));
			s->cmd_q_len--;
			handle_cmd(sat, &cmd, now);
		} else {
			i++;
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
		s->next_status = now + STATUS_PERIOD_MS;
		update_measurements(s);
		st->seq++;
		if (!packet_lost(s)) {
			s->tx_status = *st;
			s->tx_due = now + LATENCY_MIN_MS + rnd_range(0, LATENCY_SPAN_MS);
		}
	}

	if (s->tx_due >= 0 && now >= s->tx_due) {
		s->rx_status = s->tx_status;
		s->rx_time = now;
		s->tx_due = -1;
	}
}

static void sat_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	k_mutex_lock(&lock, K_FOREVER);
	for (int sat = 0; sat < NUM_SATS; sat++) {
		sims[sat].link_up = true;
		sims[sat].loss_pct = 5;
		sims[sat].rx_time = -1;
		sims[sat].tx_due = -1;
		sims[sat].st.key_armed = true;
		/* Stagger the status packets of the satellites */
		sims[sat].next_status = sat * STATUS_PERIOD_MS / NUM_SATS;
	}
	k_mutex_unlock(&lock);

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

K_THREAD_DEFINE(sat_sim_tid, 2048, sat_thread, NULL, NULL, NULL, 5, 0, 0);

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
