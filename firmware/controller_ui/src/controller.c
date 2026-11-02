#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "controller.h"
#include "inputs.h"
#include "power.h"

LOG_MODULE_REGISTER(controller, LOG_LEVEL_INF);

/* A satellite goes SAFE after this long without a poll; so is it gone for us. */
#define LINK_TIMEOUT_MS     CONFIG_LINK_SAT_SAFE_TIMEOUT_MS
#define POLL_PERIOD_MS      CONFIG_APP_POLL_PERIOD_MS
#define RETRY_MS            CONFIG_APP_CMD_RETRY_MS
#define RETRY_JITTER_MS     100
/* Unacknowledged requests are retried this long; then the fault handling decides. */
#define RETRY_FOR_MS        5000
#define ARM_ACK_TIMEOUT_MS  2000
#define FIRE_ACK_TIMEOUT_MS 3000
#define ARM_TIMEOUT_MS      (CONFIG_APP_ARM_TIMEOUT_S * 1000)
#define LINK_STABLE_MS      2000  /* link needed this long before pads return */
/* Channel utilization that makes the channel crowded, and clears it again */
#define CROWDED_PCT         70
#define UNCROWDED_PCT       55

static struct ctrl_view v = {
	.sat = {[0 ... NUM_SATS - 1] = {.ok_since = -1, .rx_time = -1}},
	.cursor = -1,
	.diag = DIAG_NONE,
};

/* Arming armed the pad under the cursor, not ticked ones: untick it after */
static bool quick_armed;

static struct ctrl_event events[CTRL_EVENTS];
static int event_count;
static int event_next;

static int64_t next_tx;
/* The last poll was a regular one: time the next from when it actually went out. */
static bool anchor_pending;
static int64_t ack_deadline;   /* ARMING / FIRING acknowledgement */
static int64_t arm_deadline;   /* inactivity timeout while ARMED */
static int64_t fire_hold_start = -1;
static bool fire_was_held;

/*
 * Per satellite, the request it has to acknowledge (common/wire/sat_wire.h):
 * the poll carries it until the satellite's status reports its id. A new
 * request replaces an unacknowledged one. id 0: nothing pending.
 */
struct request {
	enum link_cmd_type type;
	uint8_t mask;
	uint8_t id;
	int64_t since;
};

static struct request req[NUM_SATS];
static uint8_t last_req_id;

/* Per satellite, latched when arming: channels of the selected pads */
static uint8_t arm_mask[NUM_SATS];
static uint32_t cmd_seq[NUM_SATS];   /* status seq when the command was sent */
static uint8_t fire_count[NUM_SATS]; /* status fire_count when FIRE was sent */

const char *ctrl_state_name(enum ctrl_state state)
{
	switch (state) {
	case CTRL_SAFE:
		return "SAFE";
	case CTRL_ARMING:
		return "ARMING";
	case CTRL_ARMED:
		return "ARMED";
	case CTRL_FIRING:
		return "FIRING";
	}
	return "?";
}

/* Into the log of the last events, and the system log */
static const struct ctrl_event *log_event(enum msg_level level, const char *fmt, va_list ap)
{
	struct ctrl_event *e = &events[event_next];

	*e = (struct ctrl_event){.level = level, .time = k_uptime_get()};
	vsnprintf(e->text, sizeof(e->text), fmt, ap);
	event_next = (event_next + 1) % CTRL_EVENTS;
	event_count = MIN(event_count + 1, CTRL_EVENTS);

	if (level != MSG_INFO) {
		LOG_WRN("%s", e->text);
	} else {
		LOG_INF("%s", e->text);
	}
	return e;
}

/*
 * Something the operator has to be told: shown for a few seconds (a fault
 * until acknowledged) and logged.
 */
static void message(enum msg_level level, const char *fmt, ...)
{
	const struct ctrl_event *e;
	va_list ap;

	va_start(ap, fmt);
	e = log_event(level, fmt, ap);
	va_end(ap);

	strcpy(v.message, e->text);
	v.message_level = level;
	v.message_time = e->time;
}

/* Something the screen already shows (a link state, ARMED): only logged */
static void note(enum msg_level level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void)log_event(level, fmt, ap);
	va_end(ap);
}

static void set_state(enum ctrl_state s)
{
	if (s != v.state) {
		LOG_INF("%s -> %s", ctrl_state_name(v.state), ctrl_state_name(s));
		v.state = s;
	}
	fire_hold_start = -1;
	v.fire_hold_ms = 0;
}

const struct ctrl_event *ctrl_event(int age)
{
	if (age < 0 || age >= event_count) {
		return NULL;
	}
	return &events[(event_next - 1 - age + CTRL_EVENTS) % CTRL_EVENTS];
}

bool ctrl_pad_listed(int p)
{
	return p >= 0 && p < NUM_PADS && pads[p].type != PAD_OFF && v.sat[pads[p].sat].uid != 0;
}

int ctrl_pads_sats(int sats[NUM_SATS])
{
	int n = 0;

	for (int sat = 0; sat < NUM_SATS; sat++) {
		bool used = v.sat[sat].uid != 0;

		for (int p = 0; p < NUM_PADS && !used; p++) {
			used = pads[p].type != PAD_OFF && pads[p].sat == sat;
		}
		if (used) {
			sats[n++] = sat;
		}
	}
	return n;
}

int ctrl_channel_pad(int sat, int ch)
{
	for (int p = 0; p < NUM_PADS; p++) {
		if (pads[p].sat == sat && (pad_channels(&pads[p]) & BIT(ch))) {
			return p;
		}
	}
	return -1;
}

int ctrl_list_order(int order[NUM_PADS])
{
	int n = 0;

	for (int sat = 0; sat < NUM_SATS; sat++) {
		for (int ch = 0; ch < NUM_CHANNELS; ch++) {
			for (int p = 0; p < NUM_PADS; p++) {
				if (ctrl_pad_listed(p) && pads[p].sat == sat && pads[p].ign == ch) {
					order[n++] = p;
				}
			}
		}
	}
	return n;
}

static bool involved(int sat)
{
	return arm_mask[sat] != 0;
}

static bool pad_is_armed_hybrid(int p)
{
	return v.state != CTRL_SAFE && (v.selected & BIT64(p)) && pads[p].type == PAD_HYBRID;
}

/* Valve outputs to energize on one satellite, from the hybrid pad ops */
static uint8_t valves_for(int sat)
{
	uint8_t valves = 0;

	for (int p = 0; p < NUM_PADS; p++) {
		if (pads[p].sat != sat || !pad_is_armed_hybrid(p)) {
			continue;
		}
		/* Vent is normally open: energized means closed */
		if (v.pad[p].op != HYB_VENTING) {
			valves |= BIT(pads[p].vent);
		}
		if (v.pad[p].op == HYB_FILLING) {
			valves |= BIT(pads[p].fill);
		}
	}
	return valves;
}

static struct link_cmd cmd_for(int sat, enum link_cmd_type type, uint8_t mask, uint8_t id)
{
	return (struct link_cmd){
		.type = type,
		.mask = mask,
		.valves = involved(sat) ? valves_for(sat) : 0,
		.id = id,
	};
}

/* Ask satellite sat for something; the polls repeat it until acknowledged. */
static void request(int sat, enum link_cmd_type type, uint8_t mask)
{
	/*
	 * Ids start at random, so that after a controller reboot the first
	 * request is not taken for the satellite's last one. Never 0.
	 */
	if (last_req_id == 0) {
		last_req_id = sys_rand8_get();
	}
	last_req_id = last_req_id == UINT8_MAX ? 1 : last_req_id + 1;

	req[sat] = (struct request){
		.type = type,
		.mask = mask,
		.id = last_req_id,
		.since = k_uptime_get(),
	};
}

/* The satellite has executed (or answered) request sat's current request. */
static bool acked(int sat)
{
	return req[sat].id == 0 || (v.sat[sat].have_status && v.sat[sat].st.cmd_id == req[sat].id);
}

/*
 * Something still has to reach a joined satellite, or come back from it:
 * a request without its acknowledgement, valves it has not applied yet, or
 * the status that confirms ARM or the end of a fire pulse.
 */
static bool retrying(int64_t now)
{
	for (int sat = 0; sat < NUM_SATS; sat++) {
		const struct sat_view *s = &v.sat[sat];

		if (!s->joined) {
			continue;
		}
		if ((v.state == CTRL_ARMING || v.state == CTRL_FIRING) && involved(sat)) {
			return true;
		}
		if (!acked(sat) && now - req[sat].since < RETRY_FOR_MS) {
			return true;
		}
		if (v.state == CTRL_ARMED && involved(sat) && s->have_status &&
		    s->st.valve_mask != (valves_for(sat) & s->st.armed_mask)) {
			return true;
		}
	}
	return false;
}

/* What every satellite gets in a poll. */
static void build_cmds(struct link_cmd cmds[NUM_SATS])
{
	for (int sat = 0; sat < NUM_SATS; sat++) {
		if (acked(sat) && !involved(sat) && v.sat[sat].have_status &&
		    v.sat[sat].st.state != SAT_SAFE) {
			/* We think it is safe: make sure the satellite agrees */
			request(sat, CMD_DISARM, 0);
		}
		if (!acked(sat)) {
			cmds[sat] = cmd_for(sat, req[sat].type, req[sat].mask, req[sat].id);
		} else {
			cmds[sat] = cmd_for(sat, CMD_HEARTBEAT, 0, 0);
		}
	}
}

/* Satellites whose full status is wanted with the next poll. */
static bool want_full[NUM_SATS];

/*
 * One poll carries every satellite's command; each answers with its
 * status. While a request waits for its acknowledgement the polls come
 * every RETRY_MS, with some jitter so that they do not keep meeting
 * another network's cycle at the same point; otherwise every
 * POLL_PERIOD_MS.
 */
static void send_poll(void)
{
	struct link_cmd cmds[NUM_SATS];
	int64_t now = k_uptime_get();
	bool full = false;
	int ret;

	/*
	 * Replies are the short status; a satellite that has just joined
	 * sends its full one once, with the resistances and link quality.
	 */
	for (int sat = 0; sat < NUM_SATS; sat++) {
		full |= want_full[sat] && v.sat[sat].joined;
	}

	build_cmds(cmds);
	ret = link_poll(cmds, full);
	if (ret == 0 && full) {
		memset(want_full, 0, sizeof(want_full));
	}

	/* Nobody joined is normal; a backlog means the cycle is too long. */
	if (ret == -EBUSY) {
		LOG_WRN("poll dropped, link busy");
	}
	/* Only a poll that was queued has a start to time the next one from. */
	if (retrying(now)) {
		anchor_pending = false;
		next_tx = now + RETRY_MS + sys_rand16_get() % (RETRY_JITTER_MS + 1);
	} else {
		anchor_pending = ret == 0;
		next_tx = now + POLL_PERIOD_MS;
	}
}

static void set_all_ops(enum hybrid_op op)
{
	for (int p = 0; p < NUM_PADS; p++) {
		v.pad[p].op = op;
	}
}

/* Disarm everything. Hybrid vents open by themselves once unpowered. */
static void disarm_all(void)
{
	for (int sat = 0; sat < NUM_SATS; sat++) {
		request(sat, CMD_DISARM, 0);
		arm_mask[sat] = 0;
	}
	set_all_ops(HYB_CLOSED);
	set_state(CTRL_SAFE);
	if (quick_armed) {
		v.selected = 0;
		quick_armed = false;
	}
	send_poll();
}

/* Disarm everything and show a fault that stays until BACK is pressed */
static void fault(const char *fmt, int sat)
{
	disarm_all();
	message(MSG_FAULT, fmt, sat_name(sat));
	v.fault_active = true;
}

static const char *pad_problem(int p)
{
	static const char *const names[3][2] = {
		{"IGN OPEN", "IGN SHORT"},
		{"FILL OPEN", "FILL SHORT"},
		{"VENT OPEN", "VENT SHORT"},
	};
	const struct pad_cfg *cfg = &pads[p];
	const struct sat_status *st = &v.sat[cfg->sat].st;
	uint8_t chans[3] = {cfg->ign, cfg->fill, cfg->vent};
	int n = cfg->type == PAD_HYBRID ? 3 : 1;

	if (cfg->type == PAD_OFF || !v.sat[cfg->sat].have_status) {
		return NULL;
	}
	for (int i = 0; i < n; i++) {
		switch (st->cont[chans[i]]) {
		case CONT_OPEN:
			return names[i][0];
		case CONT_SHORT:
			return names[i][1];
		default:
			break;
		}
	}
	return NULL;
}

/* Per satellite, the channels of the pads in @p set */
static void selection_masks(uint64_t set, uint8_t out[NUM_SATS])
{
	for (int sat = 0; sat < NUM_SATS; sat++) {
		out[sat] = 0;
	}
	for (int p = 0; p < NUM_PADS; p++) {
		if (set & BIT64(p)) {
			out[pads[p].sat] |= pad_channels(&pads[p]);
		}
	}
}

static bool any_op(enum hybrid_op op);

static uint64_t arm_set(void)
{
	if (v.selected) {
		return v.selected;
	}
	return ctrl_pad_listed(v.cursor) ? BIT64(v.cursor) : 0;
}

static const char *arm_block_reason(void)
{
	static char buf[48];
	uint8_t masks[NUM_SATS];

	if (arm_set() == 0) {
		return "No pad to arm";
	}

	selection_masks(arm_set(), masks);
	for (int sat = 0; sat < NUM_SATS; sat++) {
		const struct sat_view *s = &v.sat[sat];

		if (!masks[sat]) {
			continue;
		}
		if (!s->link_ok) {
			snprintf(buf, sizeof(buf), "Satellite %c: no link", sat_name(sat));
			return buf;
		}
		if (!s->st.key_armed) {
			snprintf(buf, sizeof(buf), "Satellite %c: pad key in SAFE", sat_name(sat));
			return buf;
		}
	}
	return NULL;
}

/* Continuity is judged while armed, from the satellite's armed readings */
static const char *launch_block_reason(void)
{
	static char buf[48];

	for (int p = 0; p < NUM_PADS; p++) {
		if ((v.selected & BIT64(p)) && v.pad[p].problem) {
			snprintf(buf, sizeof(buf), "%s: %s", pad_name(p), v.pad[p].problem);
			return buf;
		}
	}
	if (any_op(HYB_VENTING)) {
		return "Close VENT before launch";
	}
	return NULL;
}

static bool fresh(int sat)
{
	/* seq is monotonic, so this rejects packets that were already in flight */
	return (int32_t)(v.sat[sat].st.seq - cmd_seq[sat]) > 0;
}

static void toggle_pad(int p)
{
	if (p < 0 || p >= NUM_PADS) {
		return;
	}
	if (v.state != CTRL_SAFE) {
		message(MSG_WARN, "Pads are locked while armed");
		return;
	}
	if (!ctrl_pad_listed(p)) {
		message(MSG_WARN, "%s is not available", pad_name(p));
		return;
	}
	v.selected ^= BIT64(p);
}

static void request_arm(void)
{
	const char *block;

	if (v.state != CTRL_SAFE) {
		return;
	}
	block = arm_block_reason();
	if (block != NULL) {
		note(MSG_WARN, "Cannot arm: %s", block);
		return;
	}

	quick_armed = v.selected == 0;
	v.selected = arm_set();
	selection_masks(v.selected, arm_mask);
	set_all_ops(HYB_CLOSED);
	set_state(CTRL_ARMING);
	for (int sat = 0; sat < NUM_SATS; sat++) {
		if (involved(sat)) {
			cmd_seq[sat] = v.sat[sat].st.seq;
			request(sat, CMD_ARM, arm_mask[sat]);
		}
	}
	send_poll();
	ack_deadline = k_uptime_get() + ARM_ACK_TIMEOUT_MS;
	note(MSG_INFO, "Arming...");
}

static void request_safe(const char *why)
{
	if (v.state != CTRL_SAFE) {
		disarm_all();
		message(MSG_INFO, "Disarmed: %s", why);
	}
}

static uint64_t armed_hybrids(void)
{
	uint64_t mask = 0;

	for (int p = 0; p < NUM_PADS; p++) {
		if (pad_is_armed_hybrid(p)) {
			mask |= BIT64(p);
		}
	}
	return mask;
}

static bool any_op(enum hybrid_op op)
{
	for (int p = 0; p < NUM_PADS; p++) {
		if (pad_is_armed_hybrid(p) && v.pad[p].op == op) {
			return true;
		}
	}
	return false;
}

/* FILL and VENT act on all armed hybrid pads and toggle */
static void request_valve(enum hybrid_op op)
{
	const char *name = op == HYB_FILLING ? "FILL" : "VENT";
	uint64_t hybrids = armed_hybrids();
	int64_t now = k_uptime_get();
	char list[PADS_FORMAT_LEN];

	if (v.state != CTRL_ARMED) {
		message(MSG_WARN, "%s needs the pads armed", name);
		return;
	}
	if (!hybrids) {
		message(MSG_WARN, "%s: no hybrid pad armed", name);
		return;
	}

	pads_format(list, sizeof(list), hybrids);
	if (any_op(op)) {
		set_all_ops(HYB_CLOSED);
		note(MSG_INFO, "%s closed: %s", name, list);
	} else {
		if (op == HYB_VENTING && any_op(HYB_FILLING)) {
			note(MSG_INFO, "Fill aborted, venting %s", list);
		} else {
			note(MSG_INFO, "%s %s", op == HYB_FILLING ? "Filling" : "Venting", list);
		}
		for (int p = 0; p < NUM_PADS; p++) {
			if (hybrids & BIT64(p)) {
				v.pad[p].op = op;
				v.pad[p].fill_start = now;
			}
		}
	}

	arm_deadline = now + ARM_TIMEOUT_MS;
	send_poll(); /* carries the new valve state, repeated until applied */
}

struct ctrl_req {
	enum ctrl_req_type type;
	int arg;
};

K_MSGQ_DEFINE(req_q, sizeof(struct ctrl_req), 16, 4);

void ctrl_post(enum ctrl_req_type type, int arg)
{
	struct ctrl_req req = {.type = type, .arg = arg};

	(void)k_msgq_put(&req_q, &req, K_NO_WAIT);
}

static bool field_visible(const struct pad_cfg *cfg, int field)
{
	switch (field) {
	case FIELD_SAT:
	case FIELD_IGN:
		return cfg->type != PAD_OFF;
	case FIELD_FILL:
	case FIELD_VENT:
		return cfg->type == PAD_HYBRID;
	default:
		return true;
	}
}

/* Where the cursor is in the list order, -1 if not on a listed pad */
static int cursor_index(const int *order, int n)
{
	for (int i = 0; i < n; i++) {
		if (order[i] == v.cursor) {
			return i;
		}
	}
	return -1;
}

/* Move the main list cursor by steps pads, stopping at the ends */
static void move_cursor(int steps)
{
	int order[NUM_PADS];
	int n = ctrl_list_order(order);
	int i = cursor_index(order, n);

	if (n > 0) {
		v.cursor = order[CLAMP(i + steps, 0, n - 1)];
	}
}

/* Left/right: to the first pad of the previous or next satellite with pads */
static void jump_sat(int dir)
{
	int order[NUM_PADS];
	int n = ctrl_list_order(order);
	int i = cursor_index(order, n);
	int sat;

	if (i < 0) {
		return;
	}
	sat = pads[order[i]].sat;
	if (dir > 0) {
		while (i < n && pads[order[i]].sat == sat) {
			i++;
		}
		if (i < n) {
			v.cursor = order[i];
		}
	} else {
		/* Back to the start of this group, then of the one before */
		while (i > 0 && pads[order[i - 1]].sat == sat) {
			i--;
		}
		if (i > 0) {
			sat = pads[order[i - 1]].sat;
			i--;
			while (i > 0 && pads[order[i - 1]].sat == sat) {
				i--;
			}
		}
		v.cursor = order[i];
	}
}

/* Keep the cursor on a listed pad after pads came or went */
static void fix_cursor(void)
{
	int order[NUM_PADS];
	int n = ctrl_list_order(order);

	if (cursor_index(order, n) < 0) {
		v.cursor = n > 0 ? order[0] : -1;
	}
}

static void setup_open(int pad)
{
	struct pad_setup *su = &v.setup;

	*su = (struct pad_setup){
		.active = true,
		.mode = SETUP_FIELDS,
		.pad = pad,
		.field = FIELD_LABEL,
		.cfg = pads[pad],
	};
	su->error = pads_check(su->pad, &su->cfg);
}

/* A new rocket pad on a free channel, in the first unused pad slot */
static void setup_open_new(int sat, int ch)
{
	struct pad_setup *su = &v.setup;

	for (int p = 0; p < NUM_PADS; p++) {
		if (pads[p].type == PAD_OFF) {
			setup_open(p);
			su->cfg = (struct pad_cfg){.type = PAD_ROCKET, .sat = sat, .ign = ch};
			su->is_new = true;
			su->error = pads_check(su->pad, &su->cfg);
			return;
		}
	}
	message(MSG_WARN, "All %d pads are in use", NUM_PADS);
}

static void setup_save(void)
{
	struct pad_setup *su = &v.setup;

	if (su->error) {
		message(MSG_WARN, "Cannot save: %s", su->error);
		return;
	}
	su->active = false;
	if (su->cfg.type == PAD_OFF) {
		v.selected &= ~BIT64(su->pad);
	}
	if (pads_store(su->pad, &su->cfg)) {
		message(MSG_WARN, "%s applied, but could not be stored", pad_name(su->pad));
	} else if (su->cfg.type == PAD_OFF) {
		message(MSG_INFO, "Pad %d switched off", su->pad + 1);
	} else {
		message(MSG_INFO, "%s saved", pad_name(su->pad));
	}
}

static int wrap(int value, int count)
{
	return ((value % count) + count) % count;
}

/* Left/right: change the value of the field under the cursor */
static void setup_adjust(int steps)
{
	struct pad_setup *su = &v.setup;
	struct pad_cfg *cfg = &su->cfg;

	if (su->mode == SETUP_LABEL) {
		su->pos = CLAMP(su->pos + steps, 0, PAD_LABEL_MAX - 1);
		return;
	}
	switch (su->field) {
	case FIELD_TYPE:
		cfg->type = wrap(cfg->type + steps, PAD_TYPE_COUNT);
		break;
	case FIELD_SAT:
		cfg->sat = wrap(cfg->sat + steps, NUM_SATS);
		break;
	case FIELD_IGN:
		cfg->ign = wrap(cfg->ign + steps, NUM_CHANNELS);
		break;
	case FIELD_FILL:
		cfg->fill = wrap(cfg->fill + steps, NUM_CHANNELS);
		break;
	case FIELD_VENT:
		cfg->vent = wrap(cfg->vent + steps, NUM_CHANNELS);
		break;
	default:
		break;
	}
}

/* Up/down: move between fields, or change the label character */
static void setup_cursor(int steps)
{
	static const char charset[] = LABEL_CHARSET;
	struct pad_setup *su = &v.setup;
	struct pad_cfg *cfg = &su->cfg;

	if (su->mode == SETUP_LABEL) {
		char *c = &su->label[su->pos];
		const char *at = strchr(charset, *c);
		int i = at ? at - charset : 0;

		/* Up is the next character */
		*c = charset[wrap(i - steps, sizeof(charset) - 1)];
		return;
	}

	/* Step over hidden fields, stop at the ends */
	int dir = steps > 0 ? 1 : -1;

	for (int n = abs(steps); n > 0; n--) {
		int f = su->field + dir;

		while (f >= 0 && f < FIELD_COUNT && !field_visible(cfg, f)) {
			f += dir;
		}
		if (f < 0 || f >= FIELD_COUNT) {
			break;
		}
		su->field = f;
	}
}

static void setup_label_done(void)
{
	struct pad_setup *su = &v.setup;
	size_t len = PAD_LABEL_MAX;

	while (len > 0 && su->label[len - 1] == ' ') {
		len--;
	}
	memcpy(su->cfg.label, su->label, len);
	su->cfg.label[len] = '\0';
	su->mode = SETUP_FIELDS;
}

/* OK: edit the label, or step any other value on */
static void setup_push(void)
{
	struct pad_setup *su = &v.setup;

	if (su->mode == SETUP_LABEL) {
		setup_label_done();
	} else if (su->field == FIELD_LABEL) {
		snprintf(su->label, sizeof(su->label), "%-*s", PAD_LABEL_MAX, su->cfg.label);
		su->pos = 0;
		su->mode = SETUP_LABEL;
	} else {
		setup_adjust(1);
	}
}

/* While the setup screen is open it consumes every request */
static void setup_handle(const struct ctrl_req *req)
{
	struct pad_setup *su = &v.setup;

	switch (req->type) {
	case CTRL_REQ_CURSOR:
		setup_cursor(req->arg);
		break;
	case CTRL_REQ_ADJUST:
		setup_adjust(req->arg);
		break;
	case CTRL_REQ_SELECT:
		setup_push();
		break;
	case CTRL_REQ_EDIT:
		/* Hold: finish the label, or save the whole pad */
		if (su->mode == SETUP_LABEL) {
			setup_label_done();
		} else {
			setup_save();
		}
		break;
	case CTRL_REQ_BACK:
		/* BACK backs out one level: the label edit, then the whole screen */
		if (su->mode == SETUP_LABEL) {
			su->mode = SETUP_FIELDS;
		} else {
			su->active = false;
			message(MSG_INFO, "%s not changed", su->is_new ? "New pad" : pad_name(su->pad));
		}
		break;
	case CTRL_REQ_BACK_LONG:
		break;
	default:
		message(MSG_WARN, "Finish the pad setup first (hold OK to save, BACK to cancel)");
		break;
	}

	if (su->active) {
		su->error = pads_check(su->pad, &su->cfg);
	}
}

static const char *pair_fail_text(enum lsec_fail reason)
{
	switch (reason) {
	case LSEC_FAIL_TIMEOUT:
		return "timed out";
	case LSEC_FAIL_REJECTED:
		return "cancelled";
	case LSEC_FAIL_PEER_ABORT:
		return "cancelled on the satellite";
	case LSEC_FAIL_COMMITMENT:
	case LSEC_FAIL_CHECK:
		return "keys did not match, someone may be interfering";
	case LSEC_FAIL_STORE:
		return "could not store the key";
	default:
		return "crypto error";
	}
}

static void pair_open(int sat)
{
	v.pair = (struct pair_view){
		.active = true,
		.phase = PAIR_SCAN,
		.sat = sat,
	};
	link_scan(true);
	note(MSG_INFO, "Hold the pairing button on the satellite for 5 s");
}

static void pair_close(void)
{
	link_scan(false);
	v.pair.active = false;
}

/* While the pairing screen is open it consumes every request */
static void pair_handle(const struct ctrl_req *req)
{
	struct pair_view *pv = &v.pair;

	switch (req->type) {
	case CTRL_REQ_CURSOR:
		if (pv->phase == PAIR_SCAN && pv->found > 0) {
			pv->cursor = CLAMP(pv->cursor + req->arg, 0, pv->found - 1);
		}
		break;
	case CTRL_REQ_SELECT:
		if (pv->phase == PAIR_SCAN && pv->found > 0) {
			if (link_pair(pv->sat, pv->uid[pv->cursor]) == 0) {
				pv->phase = PAIR_WAIT;
			}
		} else if (pv->phase == PAIR_COMPARE) {
			link_pair_confirm(true);
			pv->phase = PAIR_CONFIRMED;
		}
		break;
	case CTRL_REQ_BACK:
		/* Back out; past the scan this also tells the satellite */
		if (pv->phase != PAIR_SCAN) {
			link_pair_confirm(false);
		}
		pair_close();
		message(MSG_INFO, "Pairing satellite %c cancelled", sat_name(pv->sat));
		break;
	case CTRL_REQ_ADJUST:
	case CTRL_REQ_BACK_LONG:
		break;
	default:
		message(MSG_WARN, "Finish pairing first (BACK to cancel)");
		break;
	}
}

/* Pairing progress from the link */
static void pair_events(void)
{
	struct link_pair_event ev;
	struct pair_view *pv = &v.pair;

	while (link_pair_event(&ev)) {
		switch (ev.type) {
		case LSEC_EV_ADV: {
			int i = 0;

			if (!pv->active || pv->phase != PAIR_SCAN) {
				break;
			}
			while (i < pv->found && pv->uid[i] != ev.uid) {
				i++;
			}
			if (i == pv->found) {
				if (pv->found == PAIR_MAX_FOUND) {
					break;
				}
				pv->found++;
				pv->uid[i] = ev.uid;
			}
			pv->rssi_dbm[i] = ev.rssi_dbm;
			break;
		}
		case LSEC_EV_SAS:
			if (pv->active && ev.sat == pv->sat) {
				char rows[LSEC_SAS_FRAMES][LSEC_SAS_LEDS + 1];

				memcpy(pv->sas, ev.sas, sizeof(pv->sas));
				pv->phase = PAIR_COMPARE;

				for (int r = 0; r < LSEC_SAS_FRAMES; r++) {
					for (int i = 0; i < LSEC_SAS_LEDS; i++) {
						rows[r][i] = (ev.sas[r] & BIT(i)) ? '#' : '.';
					}
					rows[r][LSEC_SAS_LEDS] = '\0';
				}
				LOG_INF("pairing satellite %c, compare  1 %s  2 %s  3 %s  4 %s",
					sat_name(pv->sat), rows[0], rows[1], rows[2], rows[3]);
			}
			break;
		case LSEC_EV_PAIRED:
			pair_close();
			message(MSG_INFO, "Satellite %c paired", sat_name(ev.sat));
			break;
		case LSEC_EV_PAIR_FAILED:
			if (pv->active) {
				pair_close();
				message(MSG_WARN, "Pairing satellite %c failed: %s", sat_name(pv->sat),
					pair_fail_text(ev.reason));
			}
			break;
		default:
			break;
		}
	}
}

/* While the satellites screen is open it consumes every request */
static void sats_handle(const struct ctrl_req *req)
{
	int sat = v.sats.cursor;

	switch (req->type) {
	case CTRL_REQ_CURSOR:
		v.sats.cursor = CLAMP(sat + req->arg, 0, NUM_SATS - 1);
		break;
	case CTRL_REQ_SELECT:
		pair_open(sat);
		break;
	case CTRL_REQ_EDIT:
		/* Hold: forget. Its pads stay configured, but lose their link. */
		if (!v.sat[sat].uid) {
			message(MSG_WARN, "Satellite %c is not paired", sat_name(sat));
		} else if (link_forget(sat)) {
			message(MSG_WARN, "Satellite %c: could not erase the key", sat_name(sat));
		} else {
			message(MSG_INFO, "Satellite %c forgotten", sat_name(sat));
		}
		break;
	case CTRL_REQ_BACK:
		v.sats.active = false;
		break;
	case CTRL_REQ_ADJUST:
	case CTRL_REQ_BACK_LONG:
		break;
	default:
		message(MSG_WARN, "Leave the menu first (BACK)");
		break;
	}
}

static void settings_open(void)
{
	if (v.state != CTRL_SAFE) {
		message(MSG_WARN, "The menu is only available in SAFE");
		return;
	}
	v.settings = (struct menu_view){.active = true};
}

static void settings_close(void)
{
	v.settings.active = false;
	fix_cursor();
}

static void power_ask(void)
{
	if (v.state != CTRL_SAFE) {
		message(MSG_WARN, "Disarm before switching off");
		return;
	}
	v.power_confirm = true;
}

/* While the power off question is open it consumes every request */
static void power_handle(const struct ctrl_req *req)
{
	switch (req->type) {
	case CTRL_REQ_SELECT:
		v.power_confirm = false;
		if (!power_can_off()) {
			message(MSG_WARN, "This controller cannot switch itself off");
			break;
		}
		v.powering_off = true;
		power_off();
		break;
	case CTRL_REQ_BACK:
		v.power_confirm = false;
		break;
	default:
		break;
	}
}

static void menu_move(struct menu_view *m, int steps, int count)
{
	m->cursor = CLAMP(m->cursor + steps, 0, count - 1);
}

static void pad_list_open(void);

/* The menu */
static void settings_handle(const struct ctrl_req *req)
{
	switch (req->type) {
	case CTRL_REQ_CURSOR:
		menu_move(&v.settings, req->arg, SETTINGS_COUNT);
		break;
	case CTRL_REQ_SELECT:
		switch (v.settings.cursor) {
		case SETTINGS_EDIT:
			if (!ctrl_pad_listed(v.cursor)) {
				message(MSG_WARN, "No pad under the cursor");
				break;
			}
			/* Straight back to the list when done */
			settings_close();
			setup_open(v.cursor);
			break;
		case SETTINGS_PADS:
			pad_list_open();
			break;
		case SETTINGS_SATS:
			v.sats.active = true;
			break;
		case SETTINGS_MSGS:
			v.messages = (struct menu_view){.active = true};
			break;
		case SETTINGS_DIAG:
			v.diag_menu = (struct menu_view){.active = true};
			break;
		case SETTINGS_OFF:
			power_ask();
			break;
		}
		break;
	case CTRL_REQ_EDIT:
	case CTRL_REQ_BACK:
		settings_close();
		break;
	case CTRL_REQ_ADJUST:
	case CTRL_REQ_BACK_LONG:
		break;
	default:
		message(MSG_WARN, "Leave the menu first (BACK)");
		break;
	}
}

/* Channel list: starts on the channel of the pad under the main cursor */
static void pad_list_open(void)
{
	int sats[NUM_SATS];
	int n = ctrl_pads_sats(sats);

	v.pad_list = (struct menu_view){.active = true};
	for (int i = 0; i < n && ctrl_pad_listed(v.cursor); i++) {
		if (sats[i] == pads[v.cursor].sat) {
			v.pad_list.cursor = i * NUM_CHANNELS + pads[v.cursor].ign;
		}
	}
}

/* Channel list: OK sets up the pad on a channel, or a new one if it is free */
static void pad_list_handle(const struct ctrl_req *req)
{
	int sats[NUM_SATS];
	int n = ctrl_pads_sats(sats);
	int sat;
	int ch;
	int p;

	switch (req->type) {
	case CTRL_REQ_CURSOR:
		menu_move(&v.pad_list, req->arg, MAX(n * NUM_CHANNELS, 1));
		break;
	case CTRL_REQ_ADJUST:
		/* A satellite at a time, keeping the channel */
		v.pad_list.cursor = CLAMP(v.pad_list.cursor + req->arg * NUM_CHANNELS, 0,
					  MAX(n * NUM_CHANNELS - 1, 0));
		break;
	case CTRL_REQ_SELECT:
		if (n == 0) {
			message(MSG_WARN, "Pair a satellite first");
			break;
		}
		sat = sats[v.pad_list.cursor / NUM_CHANNELS];
		ch = v.pad_list.cursor % NUM_CHANNELS;
		p = ctrl_channel_pad(sat, ch);
		if (p >= 0) {
			setup_open(p);
		} else {
			setup_open_new(sat, ch);
		}
		break;
	case CTRL_REQ_BACK:
		v.pad_list.active = false;
		break;
	case CTRL_REQ_EDIT:
	case CTRL_REQ_BACK_LONG:
		break;
	default:
		message(MSG_WARN, "Leave the menu first (BACK)");
		break;
	}
}

/* Messages: up and down scroll through them */
static void messages_handle(const struct ctrl_req *req)
{
	int count = 0;

	while (ctrl_event(count)) {
		count++;
	}
	switch (req->type) {
	case CTRL_REQ_CURSOR:
		menu_move(&v.messages, req->arg, MAX(count - MESSAGES_ROWS + 1, 1));
		break;
	case CTRL_REQ_BACK:
		v.messages.active = false;
		break;
	case CTRL_REQ_SELECT:
	case CTRL_REQ_ADJUST:
	case CTRL_REQ_EDIT:
	case CTRL_REQ_BACK_LONG:
		break;
	default:
		message(MSG_WARN, "Leave the menu first (BACK)");
		break;
	}
}

/* Diagnostics menu */
static void diag_menu_handle(const struct ctrl_req *req)
{
	switch (req->type) {
	case CTRL_REQ_CURSOR:
		menu_move(&v.diag_menu, req->arg, DIAG_COUNT);
		break;
	case CTRL_REQ_SELECT:
		v.diag = v.diag_menu.cursor;
		break;
	case CTRL_REQ_BACK:
		v.diag_menu.active = false;
		break;
	case CTRL_REQ_ADJUST:
	case CTRL_REQ_EDIT:
	case CTRL_REQ_BACK_LONG:
		break;
	default:
		message(MSG_WARN, "Leave the menu first (BACK)");
		break;
	}
}

/* One button press or shell request, given to whatever is open */
static void handle_request(const struct ctrl_req *req)
{
	if (v.powering_off) {
		return;
	}
	if (v.power_confirm) {
		power_handle(req);
		return;
	}
	if (req->type == CTRL_REQ_POWER) {
		power_ask();
		return;
	}
	if (v.fault_active) {
		if (req->type == CTRL_REQ_BACK) {
			v.fault_active = false;
			note(MSG_INFO, "Fault acknowledged");
		}
		return;
	}
	/* The button test takes every button, so only holding BACK leaves it */
	if (v.diag != DIAG_NONE) {
		if (req->type == (v.diag == DIAG_BUTTONS ? CTRL_REQ_BACK_LONG : CTRL_REQ_BACK)) {
			v.diag = DIAG_NONE;
		}
		return;
	}
	if (v.pair.active) {
		pair_handle(req);
		return;
	}
	if (v.sats.active) {
		sats_handle(req);
		return;
	}
	if (v.setup.active) {
		setup_handle(req);
		return;
	}
	if (v.pad_list.active) {
		pad_list_handle(req);
		return;
	}
	if (v.messages.active) {
		messages_handle(req);
		return;
	}
	if (v.diag_menu.active) {
		diag_menu_handle(req);
		return;
	}
	if (v.settings.active) {
		settings_handle(req);
		return;
	}
	switch (req->type) {
	case CTRL_REQ_TOGGLE_PAD:
		toggle_pad(req->arg);
		break;
	case CTRL_REQ_CURSOR:
		if (v.state == CTRL_ARMED) {
			int max = __builtin_popcountll(v.selected) - ARMED_TABLE_ROWS;

			v.table_scroll = CLAMP(v.table_scroll + req->arg, 0, MAX(max, 0));
		} else {
			move_cursor(req->arg);
		}
		break;
	case CTRL_REQ_ADJUST:
		for (int n = abs(req->arg); n > 0 && v.state == CTRL_SAFE; n--) {
			jump_sat(req->arg);
		}
		break;
	case CTRL_REQ_SELECT:
		toggle_pad(v.cursor);
		break;
	case CTRL_REQ_EDIT:
		settings_open();
		break;
	case CTRL_REQ_ARM:
		request_arm();
		break;
	case CTRL_REQ_BACK:
		if (v.state == CTRL_SAFE) {
			v.selected = 0;
		} else {
			request_safe("BACK pressed");
		}
		break;
	case CTRL_REQ_FILL:
		request_valve(HYB_FILLING);
		break;
	case CTRL_REQ_VENT:
		request_valve(HYB_VENTING);
		break;
	default:
		break;
	}
}

static void drain_requests(void)
{
	struct ctrl_req req;

	while (k_msgq_get(&req_q, &req, K_NO_WAIT) == 0) {
		int64_t shown = v.message_time;

		handle_request(&req);
		/*
		 * The next button takes a message away, unless it caused a new
		 * one: the hint it covers says what ARM would arm. A fault
		 * stays until acknowledged.
		 */
		if (v.message_time == shown && !v.fault_active) {
			v.message[0] = '\0';
		}
	}
}

static void tick_arming(int64_t now)
{
	bool all_armed = true;

	for (int sat = 0; sat < NUM_SATS; sat++) {
		uint8_t refused = v.sat[sat].st.arm_refused;

		if (involved(sat) && fresh(sat) && refused) {
			int ch = __builtin_ctz(refused) + 1;

			disarm_all();
			message(MSG_FAULT, "Satellite %c refused ARM: CH%d output shorted",
				sat_name(sat), ch);
			v.fault_active = true;
			return;
		}
		if (involved(sat) &&
		    !(acked(sat) && fresh(sat) && v.sat[sat].st.state == SAT_ARMED)) {
			all_armed = false;
			if (now >= ack_deadline) {
				fault("Satellite %c did not acknowledge ARM", sat);
				return;
			}
		}
	}

	if (all_armed) {
		arm_deadline = now + ARM_TIMEOUT_MS;
		v.table_scroll = 0;
		set_state(CTRL_ARMED);
		note(MSG_INFO, "ARMED");
	}
}

static void start_fire(int64_t now)
{
	char list[PADS_FORMAT_LEN];

	/* LAUNCH stops a running fill; vents stay closed during the pulse */
	set_all_ops(HYB_CLOSED);

	for (int sat = 0; sat < NUM_SATS; sat++) {
		uint8_t ign = 0;

		if (!involved(sat)) {
			continue;
		}
		for (int p = 0; p < NUM_PADS; p++) {
			if ((v.selected & BIT64(p)) && pads[p].sat == sat) {
				ign |= BIT(pads[p].ign);
			}
		}
		fire_count[sat] = v.sat[sat].st.fire_count;
		cmd_seq[sat] = v.sat[sat].st.seq;
		/* Repeated until acknowledged: the request id keeps it to one pulse. */
		request(sat, CMD_FIRE, ign);
	}

	send_poll();

	ack_deadline = now + FIRE_ACK_TIMEOUT_MS;
	set_state(CTRL_FIRING);
	pads_format(list, sizeof(list), v.selected);
	note(MSG_INFO, "LAUNCH %s", list);
}

static void tick_armed(int64_t now)
{
	bool fire_held = inputs_fire_held();

	for (int sat = 0; sat < NUM_SATS; sat++) {
		if (!involved(sat)) {
			continue;
		}
		if (!v.sat[sat].link_ok) {
			fault("Link to satellite %c lost while armed", sat);
			return;
		}
		if (v.sat[sat].st.state == SAT_SAFE) {
			fault("Satellite %c dropped arm (pad key or heartbeat)", sat);
			return;
		}
	}

	/* A fill can take minutes; timing out would vent the tank */
	v.arm_timer_paused = any_op(HYB_FILLING);
	if (v.arm_timer_paused) {
		arm_deadline = now + ARM_TIMEOUT_MS;
	}
	if (now >= arm_deadline) {
		request_safe("arm timeout");
		return;
	}
	v.arm_remaining_ms = (int32_t)(arm_deadline - now);

	v.launch_block = launch_block_reason();
	if (fire_held && !fire_was_held && v.launch_block) {
		note(MSG_WARN, "Cannot launch: %s", v.launch_block);
	}
	fire_was_held = fire_held;

	if (!fire_held || v.launch_block) {
		fire_hold_start = -1;
		v.fire_hold_ms = 0;
		return;
	}
	if (fire_hold_start < 0) {
		fire_hold_start = now;
	}
	v.fire_hold_ms = (int32_t)(now - fire_hold_start);
	if (v.fire_hold_ms >= CONFIG_APP_FIRE_HOLD_MS) {
		start_fire(now);
	}
}

static void tick_firing(int64_t now)
{
	bool done = true;
	char list[PADS_FORMAT_LEN];

	for (int sat = 0; sat < NUM_SATS; sat++) {
		const struct sat_status *st = &v.sat[sat].st;

		if (!involved(sat)) {
			continue;
		}
		if (!acked(sat) || !fresh(sat) || st->fire_count == fire_count[sat] ||
		    st->state == SAT_FIRING) {
			done = false;
			if (now >= ack_deadline) {
				fault("Satellite %c did not confirm FIRE", sat);
				return;
			}
		}
	}

	if (done) {
		pads_format(list, sizeof(list), v.selected);
		disarm_all();
		/* Spent: they would only show open igniters now */
		v.selected = 0;
		message(MSG_INFO, "Launched %s, disarmed", list);
	}
}

/* Warn when the shared channel fills up: every network's commands slow down. */
static void update_channel(void)
{
	link_channel(&v.channel_util, &v.other_nets);

	if (!v.channel_crowded && v.channel_util >= CROWDED_PCT) {
		v.channel_crowded = true;
		if (v.other_nets) {
			note(MSG_WARN, "Channel crowded: %u%% busy, %u other network%s",
			     v.channel_util, v.other_nets, v.other_nets == 1 ? "" : "s");
		} else {
			note(MSG_WARN, "Channel crowded: %u%% busy", v.channel_util);
		}
	} else if (v.channel_crowded && v.channel_util < UNCROWDED_PCT) {
		v.channel_crowded = false;
	}
}

void ctrl_tick(void)
{
	int64_t now = k_uptime_get();

	update_channel();

	for (int sat = 0; sat < NUM_SATS; sat++) {
		struct sat_view *s = &v.sat[sat];
		int64_t rx = link_last_status(sat, &s->st);

		s->uid = link_paired_uid(sat);
		bool was_joined = s->joined;

		s->joined = link_joined(sat);
		if (s->joined && !was_joined) {
			want_full[sat] = true;
		}
		s->have_status = rx >= 0;
		s->rx_time = rx;
		s->link_ok = s->have_status && now - rx < LINK_TIMEOUT_MS;

		if (!s->link_ok) {
			s->ok_since = -1;
			if (s->visible) {
				s->visible = false;
				note(MSG_WARN, "Satellite %c disconnected", sat_name(sat));
			}
		} else if (s->ok_since < 0) {
			s->ok_since = now;
		} else if (!s->visible && now - s->ok_since >= LINK_STABLE_MS) {
			s->visible = true;
			note(MSG_INFO, "Satellite %c connected", sat_name(sat));
		}
	}

	/*
	 * Ticked pads stay ticked while their satellite's link is gone, and
	 * arming says why it cannot; only pads that leave the list (switched
	 * off, satellite forgotten) are unticked. While armed, losing a
	 * satellite is a fault instead.
	 */
	if (v.state == CTRL_SAFE) {
		for (int p = 0; p < NUM_PADS; p++) {
			if (!ctrl_pad_listed(p)) {
				v.selected &= ~BIT64(p);
			}
		}
	}
	for (int p = 0; p < NUM_PADS; p++) {
		v.pad[p].problem = pad_problem(p);
	}
	if (!v.settings.active) {
		fix_cursor();
	}

	pair_events();
	drain_requests();

	switch (v.state) {
	case CTRL_SAFE:
		break;
	case CTRL_ARMING:
		tick_arming(now);
		break;
	case CTRL_ARMED:
		tick_armed(now);
		break;
	case CTRL_FIRING:
		tick_firing(now);
		break;
	}

	v.arm_set = v.state == CTRL_SAFE ? arm_set() : v.selected;
	v.arm_block = v.state == CTRL_SAFE ? arm_block_reason() : NULL;
	if (v.state != CTRL_ARMED) {
		v.launch_block = NULL;
	}
	if (v.state != CTRL_ARMED) {
		v.arm_remaining_ms = 0;
		v.arm_timer_paused = false;
	}

	/*
	 * A regular poll comes a period after the last one went out, moved
	 * into the gap between other networks' polls (DESYNC), so that
	 * networks sharing the channel settle into turns.
	 */
	if (anchor_pending) {
		int64_t at = link_next_poll_ms(POLL_PERIOD_MS);

		if (at > 0) {
			next_tx = at;
			anchor_pending = false;
		}
	}
	if (now >= next_tx) {
		send_poll();
	}
}

const struct ctrl_view *ctrl_view(void)
{
	return &v;
}
