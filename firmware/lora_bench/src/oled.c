/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * Bench status on the Heltec's 128x64 OLED, as 8 lines of 25 characters
 * in the 5x8 font from font_5x8.c:
 *
 *   079c  KCT8103L (V4.3)
 *   869.850 SF7 +7dBm
 *   RX ping #12 from 3a41
 *     -45dBm 9dB  3s ago
 *   PING 7/10  ok 6
 *     there -41dBm  9dB
 *     back  -43dBm 10dB
 *     rtt 342ms
 */

#include <errno.h>
#include <stdarg.h>
#include <zephyr/device.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include "bench.h"
#include "oled.h"
#include "radio.h"
#include "secure.h"

LOG_MODULE_REGISTER(oled, LOG_LEVEL_INF);

#define LINE_HEIGHT 8
#define LINE_CHARS  25

static const struct device *const display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static bool ready;

int oled_init(void)
{
	int ret;

	if (!device_is_ready(display)) {
		LOG_WRN("no display");
		return -ENODEV;
	}

	ret = cfb_framebuffer_init(display);
	if (ret == 0) {
		/* The 5x8 font is the only one built in. */
		ret = cfb_framebuffer_set_font(display, 0);
	}
	if (ret == 0) {
		ret = display_blanking_off(display);
	}
	if (ret) {
		LOG_ERR("display init: %d", ret);
		return ret;
	}

	ready = true;
	return 0;
}

static void line(int row, const char *fmt, ...)
{
	char text[LINE_CHARS + 1];
	va_list ap;

	va_start(ap, fmt);
	vsnprintk(text, sizeof(text), fmt, ap);
	va_end(ap);

	cfb_print(display, text, 0, row * LINE_HEIGHT);
}

static const char *pkt_name(uint8_t type)
{
	switch (type) {
	case BENCH_PING:
		return "ping";
	case BENCH_PONG:
		return "pong";
	default:
		return "?";
	}
}

static void show_rx(const struct bench_status *st)
{
	if (st->rx_time == 0) {
		line(2, "RX nothing yet");
		return;
	}

	if (st->rx_type == 0) {
		line(2, "RX unknown packet");
	} else {
		line(2, "RX %s #%u from %04x", pkt_name(st->rx_type), st->rx_seq,
		     st->rx_src);
	}

	line(3, "  %ddBm %ddB  %ds ago", st->rx_rssi_dbm, st->rx_snr_db,
	     (int)((k_uptime_get() - st->rx_time) / 1000));
}

static void show_pings(const struct bench_status *st)
{
	if (st->ping_total == 0) {
		line(4, "PRG button: ping run");
		return;
	}

	line(4, "%s %u/%u  ok %u", st->pinging ? "PING" : "DONE", st->ping_sent,
	     st->ping_total, st->pongs);

	if (st->pongs > 0) {
		line(5, "  there %ddBm %ddB", st->there_rssi_dbm, st->there_snr_db);
		line(6, "  back  %ddBm %ddB", st->back_rssi_dbm, st->back_snr_db);
		line(7, "  rtt %ums", st->rtt_ms);
	}
}

void oled_show(const struct bench_status *st)
{
	uint32_t f = radio_frequency();

	if (!ready) {
		return;
	}

	cfb_framebuffer_clear(display, false);

	line(0, "%04x  %s", st->node_id, st->frontend ? st->frontend : "");

	if (st->error) {
		line(2, "ERROR");
		line(3, "%s", st->error);
		line(4, "code %d", st->error_code);
	} else {
		line(1, "%u.%03u SF%u %+ddBm", f / 1000000, (f / 1000) % 1000,
		     radio_spreading_factor(), radio_tx_power());
		show_rx(st);
		show_pings(st);
	}

	cfb_framebuffer_finalize(display);
}

/*
 * Secure mode, normally:
 *
 *   079c CTRL SF7 +7dBm
 *   link: joined
 *   sat 00c0ffee12345678
 *   > joined
 *   ok 12 tag 0 replay 1
 *   net 0 nosess 0
 *   PING 7/10 ok 6 grp 3
 *     rtt 342ms
 *
 * and while a pairing waits for the user, the pattern to compare: four
 * numbered rows of six boxes, filled where the satellite's LED is lit.
 */
#define SAS_TOP   12
#define SAS_PITCH 13
#define BOX_W     12
#define BOX_H     10
#define BOX_PITCH 18
#define BOX_LEFT  14

static void show_sas(const struct secure_status *st)
{
	line(0, "COMPARE PRG=ok hold=no");

	for (int r = 0; r < LSEC_SAS_FRAMES; r++) {
		int y = SAS_TOP + r * SAS_PITCH;
		char label[2] = {'1' + r, '\0'};

		cfb_draw_text(display, label, 0, y + 1);

		for (int led = 0; led < LSEC_SAS_LEDS; led++) {
			int x = BOX_LEFT + led * BOX_PITCH;
			struct cfb_position a = {x, y};
			struct cfb_position b = {x + BOX_W - 1, y + BOX_H - 1};

			cfb_draw_rect(display, &a, &b);
			if (st->sas[r] & BIT(led)) {
				cfb_invert_area(display, x + 1, y + 1, BOX_W - 2, BOX_H - 2);
			}
		}
	}
}

static const char *link_state(const struct secure_status *st)
{
	if (st->scanning) {
		return "looking for sats";
	}
	if (st->pairing) {
		return st->role == ROLE_SAT ? "pairing mode" : "pairing";
	}
	if (!st->paired) {
		return "not paired";
	}
	return st->joined ? "joined" : "paired, joining";
}

void oled_show_secure(uint16_t node_id, const struct secure_status *st)
{
	if (!ready) {
		return;
	}

	cfb_framebuffer_clear(display, false);

	if (st->sas_shown) {
		show_sas(st);
		cfb_framebuffer_finalize(display);
		return;
	}

	line(0, "%04x %s SF%u %+ddBm", node_id, st->role == ROLE_CTRL ? "CTRL" : "SAT",
	     radio_spreading_factor(), radio_tx_power());
	line(1, "link: %s", link_state(st));
	if (st->uid != 0) {
		line(2, "%s %016llx", st->role == ROLE_CTRL ? "sat" : "me", st->uid);
	}
	if (st->note) {
		line(3, "> %s", st->note);
	}
	line(4, "ok %u tag %u replay %u", st->stats.rx_ok, st->stats.bad_tag,
	     st->stats.replay);
	line(5, "net %u nosess %u", st->stats.foreign, st->stats.no_session);

	if (st->ping_total > 0) {
		line(6, "%s %u/%u ok %u grp %u", st->pinging ? "PING" : "DONE", st->ping_sent,
		     st->ping_total, st->pongs, st->group_pongs);
		if (st->pongs > 0) {
			line(7, "  rtt %ums", st->rtt_ms);
		}
	}

	cfb_framebuffer_finalize(display);
}
