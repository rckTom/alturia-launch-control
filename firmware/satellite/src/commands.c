/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The console's commands and the status that answers them, see
 * common/wire/sat_wire.h and design/radio_security.md, "Commands".
 *
 * Every poll carries a command for this satellite. A command with a
 * request id is executed once: the console repeats it until the status
 * reports that id, and a repeat is only answered, never executed again.
 * That is what makes it safe to repeat FIRE.
 *
 * Provisional, until the board and its maths exist:
 *   - there are no pad key or E-STOP inputs yet, so the status reports the
 *     key in ARM and no E-STOP;
 *   - continuity is the ADC reading taken as CONT_MOHM_PER_MV mOhm per mV.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "lsec.h"
#include "sat_link.h"
#include "sat_wire.h"
#include "satellite.h"

LOG_MODULE_REGISTER(commands, LOG_LEVEL_INF);

#define FIRE_PULSE_MS     1000
#define SHORT_BELOW_MOHM  200
#define OPEN_ABOVE_MOHM   100000
#define CONT_MOHM_PER_MV  10

BUILD_ASSERT(NUM_CHANNELS == SAT_CHANNELS);

static K_MUTEX_DEFINE(lock);
static struct sat_status st;
static uint8_t last_id;

/* Measurements, refreshed from the main loop: the ADCs are too slow for
 * the few milliseconds between a poll and the reply slot.
 */
void commands_measure(void)
{
	int32_t res[NUM_CHANNELS];
	enum continuity cont[NUM_CHANNELS];
	int32_t mv;

	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		if (channels_continuity(ch, &mv) || mv < 0 ||
		    (int64_t)mv * CONT_MOHM_PER_MV > OPEN_ABOVE_MOHM) {
			res[ch] = RES_OPEN_MOHM;
			cont[ch] = CONT_OPEN;
		} else {
			res[ch] = mv * CONT_MOHM_PER_MV;
			cont[ch] = res[ch] < SHORT_BELOW_MOHM ? CONT_SHORT : CONT_OK;
		}
	}

	k_mutex_lock(&lock, K_FOREVER);
	memcpy(st.res_mohm, res, sizeof(res));
	memcpy(st.cont, cont, sizeof(cont));
	if (rail_vbat_mv(&mv) == 0) {
		st.vbat_mv = mv;
	}
	/* No fire-rail current monitor on the board. */
	st.rail_ma = 0;
	k_mutex_unlock(&lock);
}

static void disarm_locked(void)
{
	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		channels_arm(ch, false);
	}
}

void commands_disarm(const char *why)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (!channels_all_safe()) {
		LOG_WRN("SAFE: %s", why);
		disarm_locked();
	}
	k_mutex_unlock(&lock);
}

static void arm(uint8_t mask)
{
	if (!channels_all_safe()) {
		return;   /* already armed: ARM does not change the set */
	}

	/*
	 * Pre-arm check: an output that reads shorted while disarmed would
	 * fire the moment the arm relay closes.
	 */
	st.arm_refused = 0;
	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		if ((mask & BIT(ch)) && st.cont[ch] == CONT_SHORT) {
			st.arm_refused |= BIT(ch);
		}
	}
	if (st.arm_refused) {
		LOG_WRN("ARM refused, shorted outputs 0x%02x", st.arm_refused);
		return;
	}

	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		if (mask & BIT(ch)) {
			channels_arm(ch, true);
		}
	}
	LOG_INF("ARMED, mask 0x%02x", mask);
}

static void fire(uint8_t mask)
{
	int ret = channels_pulse(mask, FIRE_PULSE_MS);

	if (ret) {
		LOG_WRN("FIRE refused, mask 0x%02x: %d", mask, ret);
		return;
	}
	st.fire_count++;
	LOG_INF("FIRING, mask 0x%02x", mask);
}

static void execute(const struct link_cmd *cmd)
{
	/* A repeat of the request executed last: only answer it. */
	if (cmd->id != 0 && cmd->id != last_id) {
		last_id = cmd->id;

		switch (cmd->type) {
		case CMD_ARM:
			arm(cmd->mask);
			break;
		case CMD_DISARM:
			st.arm_refused = 0;
			if (!channels_all_safe()) {
				LOG_INF("SAFE: disarm command");
			}
			disarm_locked();
			break;
		case CMD_FIRE:
			fire(cmd->mask);
			break;
		case CMD_HEARTBEAT:
			break;
		}
	}

	/* Valves are state, applied on every poll to armed channels only. */
	if (!channels_all_safe()) {
		uint8_t valves = cmd->valves & channels_armed_mask();

		if (valves != channels_valve_mask()) {
			LOG_INF("valves 0x%02x", valves);
		}
		channels_valves(valves);
	}
}

static size_t build_status(bool full, uint8_t *out)
{
	struct sat_link_state ls;
	uint8_t armed = channels_armed_mask();

	sat_link_state(&ls);

	st.seq++;
	st.state = channels_pulsing() ? SAT_FIRING : armed ? SAT_ARMED : SAT_SAFE;
	st.key_armed = true;   /* no pad key input yet */
	st.estop = false;      /* no E-STOP input yet */
	st.armed_mask = armed;
	st.firing_mask = channels_pulsing();
	st.valve_mask = channels_valve_mask();
	st.cmd_id = last_id;
	st.rssi_dbm = ls.rssi_dbm;
	st.snr_db = ls.snr_db;

	return wire_encode_status(&st, full, out);
}

void commands_on_poll(const struct lsec_event *ev)
{
	uint8_t buf[WIRE_STATUS_LEN];
	struct link_cmd cmd;
	bool full = false;
	size_t len;
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	if (wire_decode_poll(ev->data, ev->len, ev->rank, ev->count, &cmd, &full) == 0) {
		execute(&cmd);
	} else if (ev->len) {
		LOG_WRN("undecodable command, %u bytes", (unsigned)ev->len);
	}
	len = build_status(full, buf);
	k_mutex_unlock(&lock);

	ret = sat_link_reply(buf, len);
	if (ret) {
		LOG_WRN("status reply: %d", ret);
	}
}
