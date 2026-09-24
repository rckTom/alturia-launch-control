/* SPDX-License-Identifier: Apache-2.0 */

/*
 * LoRa bench test for two Heltec WiFi LoRa 32 V4 boards.
 *
 * Both boards run this same image. By default a board just listens and
 * answers every PING with a PONG. Pressing the PRG button, or "bench ping"
 * on the shell, makes it send a run of PINGs and log, for each one, the
 * round-trip time and the RSSI/SNR in both directions.
 *
 * With a role set ("bench role ctrl|sat") the board runs secure.c instead:
 * pairing, sessions and sealed pings on the link security layer.
 */

#include <errno.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>

#include "bench.h"
#include "heltec_v4_fem.h"
#include "oled.h"
#include "radio.h"
#include "secure.h"

LOG_MODULE_REGISTER(bench, LOG_LEVEL_INF);

/* Pings sent for one press of the PRG button. */
#define BUTTON_PING_COUNT    10
#define BUTTON_PING_INTERVAL 2000

/* How often a listening board checks whether a ping run was requested. */
#define LISTEN_WINDOW K_MSEC(250)

/* How often the display is redrawn when nothing happens, for the ages. */
#define DISPLAY_REFRESH_MS 1000

/* Time allowed for the other board to turn around and start its PONG. */
#define PONG_TURNAROUND_MS 100

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static uint16_t node_id;
static enum bench_role role;

/* What the display shows. Only main() and the functions it calls touch it. */
static struct bench_status status;
static bool status_changed;

/* A ping run requested by the shell or the button, run by main(). */
static struct {
	uint32_t count;
	uint32_t interval_ms;
} ping_run;
static atomic_t ping_busy;
static atomic_t ping_stop;
static K_SEM_DEFINE(ping_requested, 0, 1);

uint16_t bench_node_id(void)
{
	return node_id;
}

int bench_ping_start(uint32_t count, uint32_t interval_ms)
{
	if (!atomic_cas(&ping_busy, 0, 1)) {
		return -EBUSY;
	}

	ping_run.count = count;
	ping_run.interval_ms = interval_ms;
	atomic_clear(&ping_stop);
	k_sem_give(&ping_requested);
	return 0;
}

void bench_ping_stop(void)
{
	atomic_set(&ping_stop, 1);
}

static void on_button(struct input_event *evt, void *user_data)
{
	if (evt->type != INPUT_EV_KEY || evt->code != INPUT_KEY_0) {
		return;
	}

	if (role != ROLE_OFF) {
		secure_button(evt->value);
		return;
	}

	if (evt->value) {
		if (bench_ping_start(BUTTON_PING_COUNT, BUTTON_PING_INTERVAL) == -EBUSY) {
			bench_ping_stop();
		}
	}
}
INPUT_CALLBACK_DEFINE(NULL, on_button, NULL);

/* Record a received packet for the display; true if it is a bench packet. */
static bool note_rx(const struct bench_pkt *pkt, int len,
		    const struct radio_rx_info *rx)
{
	bool valid = len == sizeof(*pkt) && pkt->magic == BENCH_MAGIC;

	status.rx_time = k_uptime_get();
	status.rx_type = valid ? pkt->type : 0;
	status.rx_seq = pkt->seq;
	status.rx_src = pkt->src;
	status.rx_rssi_dbm = rx->rssi_dbm;
	status.rx_snr_db = rx->snr_db;
	status_changed = true;

	return valid;
}

static int send_pkt(const struct bench_pkt *pkt)
{
	int ret;

	gpio_pin_set_dt(&led, 1);
	ret = radio_send(pkt, sizeof(*pkt));
	gpio_pin_set_dt(&led, 0);

	if (ret) {
		LOG_ERR("send: %d", ret);
	}
	return ret;
}

/* Listen for one window and answer a PING if one arrives. */
static void answer_pings(void)
{
	struct bench_pkt ping;
	struct radio_rx_info rx;
	int len;

	len = radio_recv(&ping, sizeof(ping), LISTEN_WINDOW, &rx);
	if (len == -EAGAIN) {
		return;
	}
	if (len < 0) {
		LOG_WRN("recv: %d", len);
		return;
	}
	if (!note_rx(&ping, len, &rx) || ping.type != BENCH_PING) {
		LOG_INF("ignored %d-byte packet, %d dBm", len, rx.rssi_dbm);
		return;
	}

	struct bench_pkt pong = {
		.magic = BENCH_MAGIC,
		.type = BENCH_PONG,
		.src = node_id,
		.dst = ping.src,
		.seq = ping.seq,
		.rssi_dbm = rx.rssi_dbm,
		.snr_db = rx.snr_db,
	};

	LOG_INF("ping %u from %04x: %d dBm, SNR %d dB", ping.seq, ping.src,
		rx.rssi_dbm, rx.snr_db);
	send_pkt(&pong);
}

/* Wait until @p deadline for the PONG to PING @p seq, skipping anything else. */
static bool await_pong(uint32_t seq, int64_t deadline, struct bench_pkt *pong,
		       struct radio_rx_info *rx)
{
	int64_t left;

	while ((left = deadline - k_uptime_get()) > 0) {
		int len = radio_recv(pong, sizeof(*pong), K_MSEC(left), rx);

		if (len >= 0 && note_rx(pong, len, rx) && pong->type == BENCH_PONG &&
		    pong->dst == node_id && pong->seq == seq) {
			return true;
		}
	}

	return false;
}

static void run_pings(uint32_t count, uint32_t interval_ms)
{
	uint32_t airtime = radio_airtime_ms(sizeof(struct bench_pkt));
	uint32_t pongs = 0;
	int64_t rtt_sum = 0;
	int16_t worst_rssi = INT16_MAX;

	LOG_INF("pinging %u times every %u ms, %u ms airtime per packet",
		count, interval_ms, airtime);

	status.pinging = true;
	status.ping_total = count;
	status.ping_sent = 0;
	status.pongs = 0;

	for (uint32_t seq = 1; seq <= count && !atomic_get(&ping_stop); seq++) {
		struct bench_pkt ping = {
			.magic = BENCH_MAGIC,
			.type = BENCH_PING,
			.src = node_id,
			.seq = seq,
		};
		struct bench_pkt pong;
		struct radio_rx_info rx;
		int64_t start = k_uptime_get();

		status.ping_sent = seq;

		if (send_pkt(&ping) == 0 &&
		    await_pong(seq, k_uptime_get() + airtime + PONG_TURNAROUND_MS,
			       &pong, &rx)) {
			int64_t rtt = k_uptime_get() - start;

			LOG_INF("ping %u: pong from %04x in %lld ms, "
				"there %d dBm/%d dB, back %d dBm/%d dB",
				seq, pong.src, rtt, pong.rssi_dbm, pong.snr_db,
				rx.rssi_dbm, rx.snr_db);

			pongs++;
			rtt_sum += rtt;
			worst_rssi = MIN(worst_rssi, MIN(pong.rssi_dbm, rx.rssi_dbm));

			status.pongs = pongs;
			status.there_rssi_dbm = pong.rssi_dbm;
			status.there_snr_db = pong.snr_db;
			status.back_rssi_dbm = rx.rssi_dbm;
			status.back_snr_db = rx.snr_db;
			status.rtt_ms = rtt;
		} else {
			LOG_WRN("ping %u: no pong", seq);
		}

		oled_show(&status);

		k_sleep(K_TIMEOUT_ABS_MS(start + interval_ms));
	}

	status.pinging = false;
	status_changed = true;

	if (pongs > 0) {
		LOG_INF("%u of %u answered, mean RTT %lld ms, worst RSSI %d dBm",
			pongs, count, rtt_sum / pongs, worst_rssi);
	} else {
		LOG_INF("0 of %u answered", count);
	}
}

static int read_node_id(void)
{
	uint8_t id[8];
	ssize_t len = hwinfo_get_device_id(id, sizeof(id));

	if (len < 2) {
		return len < 0 ? len : -EIO;
	}

	node_id = (id[len - 2] << 8) | id[len - 1];
	return 0;
}

/* Redraw on changes, and every DISPLAY_REFRESH_MS for the ages. */
static void refresh_display(void)
{
	static int64_t last;
	int64_t now = k_uptime_get();

	if (status_changed || now - last >= DISPLAY_REFRESH_MS) {
		oled_show(&status);
		status_changed = false;
		last = now;
	}
}

/* Log a bring-up failure and leave it on the display. */
static int fail(const char *what, int err)
{
	LOG_ERR("%s: %d", what, err);
	status.error = what;
	status.error_code = err;
	oled_show(&status);
	return err;
}

int main(void)
{
	const struct radio_frontend *fe;
	int ret;

	/* Optional: the bench works without it, only less visibly. */
	(void)oled_init();

	ret = read_node_id();
	if (ret) {
		return fail("device id", ret);
	}
	status.node_id = node_id;

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		return fail("LED", ret);
	}

	fe = heltec_v4_fem_init();
	if (fe == NULL) {
		return fail("RF front-end", -EIO);
	}
	status.frontend = fe->name;

	ret = radio_init(fe);
	if (ret) {
		return fail("radio init", ret);
	}

	ret = settings_subsys_init();
	if (ret) {
		return fail("settings", ret);
	}

	role = secure_role_load();
	if (role != ROLE_OFF) {
		secure_main(role);
	}

	LOG_INF("node %04x listening; press PRG or run \"bench ping\"", node_id);

	while (true) {
		if (k_sem_take(&ping_requested, K_NO_WAIT) == 0) {
			run_pings(ping_run.count, ping_run.interval_ms);
			atomic_clear(&ping_busy);
		}
		answer_pings();
		refresh_display();
	}

	return 0;
}
