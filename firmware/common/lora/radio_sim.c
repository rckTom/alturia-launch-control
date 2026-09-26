/* SPDX-License-Identifier: Apache-2.0 */

/*
 * radio.h on native_sim, without a radio: frames go to the radio network
 * simulator (tools/radiosim) over UDP, which decides who hears them, and
 * come back from it. The link above sees the same contract as with
 * radio.c: radio_send() blocks for the airtime, received frames carry the
 * uptime at which they ended, and radio_channel_clear() samples the energy
 * the simulator says is on air here.
 *
 * native_sim runs at real time (NATIVE_SIM_SLOWDOWN_TO_REAL_TIME), so the
 * uptime of every node advances with host CLOCK_MONOTONIC, which the
 * simulator timestamps everything in. Host times are converted to uptime
 * at the moment they are used, so socket latency does not shift them.
 *
 * Command line: --radio_sim=host:port (default 127.0.0.1:7700),
 * --radio_name=<name>, --radio_role=ctrl|sat. Shell: "rsim" shows the
 * connection and how well the uptime follows the host clock.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#ifdef CONFIG_HWINFO
#include <zephyr/drivers/hwinfo.h>
#endif

#include <cmdline.h>
#include <nsi_timer_model.h>
#include <posix_native_task.h>

#include "link_sched.h"
#include "radio.h"
#include "radio_sim.h"
#include "radio_sim_bottom.h"

LOG_MODULE_REGISTER(radio_sim, LOG_LEVEL_INF);

#define MAX_TX_POWER_DBM   7
#define CHIP_POWER_MIN     -9
#define CHIP_POWER_MAX     22
#define PAIR_TX_POWER_DBM  0
#define LINK_SF            7
#define PAIR_SF            8
#define FREQUENCY_HZ       869850000
#define BANDWIDTH_KHZ      250

#define RX_QUEUE_DEPTH     12
#define RSSI_SAMPLE_US     200
#define THRESHOLD_MIN_DBM  -120
#define THRESHOLD_MAX_DBM  -60
#define FLOOR_RISE_Q4      4

/* How often the socket is looked at. Host timestamps make up for the delay. */
#define POLL_US            500
/*
 * native_sim holds its uptime to the host clock only every "rt period"
 * (10 ms by default); in between, the uptime runs ahead by up to that much,
 * which host-to-uptime conversion turns into slot timing error. The link's
 * slot guard is 6 ms, so resynchronize much more often. --rt-period on the
 * command line still wins.
 */
#define RT_PERIOD_US       500
#define HEARTBEAT_MS       1000
#define LOST_AFTER_MS      3500
#define ENERGY_SLOTS       32
/* Energy that ended longer ago than this cannot matter to a listen. */
#define ENERGY_KEEP_US     200000
#define DISPLAY_MAX        (sizeof(struct rsim_display) + 16384)

#ifndef RADIO_SIM_ROLE
#define RADIO_SIM_ROLE RSIM_ROLE_OTHER
#endif

#ifdef CONFIG_RADIO_SIM_TX_DELAY_US
#define TX_DELAY_US CONFIG_RADIO_SIM_TX_DELAY_US
#else
#define TX_DELAY_US 0
#endif

/* Command line */
static char *opt_addr;
static char *opt_name;
static char *opt_role;

struct rx_packet {
	struct radio_rx_info info;
	uint8_t len;
	uint8_t data[RADIO_MAX_PAYLOAD];
};

K_MSGQ_DEFINE(rx_queue, sizeof(struct rx_packet), RX_QUEUE_DEPTH, 4);

struct ui_event {
	struct rsim_input in;   /* BUTTON arrives as type 0, index in code */
};

K_MSGQ_DEFINE(ui_queue, sizeof(struct ui_event), 16, 4);

struct energy {
	uint32_t id;
	uint64_t t_start;
	uint64_t t_end;
	int16_t rssi;
	bool used;
};

/* Guards the energy table, the socket reads and the node state below. */
static K_MUTEX_DEFINE(lock);
static K_MUTEX_DEFINE(tx_lock);

static struct energy energy[ENERGY_SLOTS];
static int16_t sim_floor_dbm = -114;
static int32_t floor_q4 = -114 * 16;
static uint16_t node_id;
static int64_t last_welcome_ms = -LOST_AFTER_MS;
static bool was_connected;

static enum radio_profile profile = RADIO_PROFILE_LINK;
static int8_t link_power_dbm = MAX_TX_POWER_DBM;
static int8_t antenna_power_dbm = MAX_TX_POWER_DBM;
static uint16_t leds;
static uint8_t role = RADIO_SIM_ROLE;
static char name[16];
static uint64_t uid;

static void (*button_cb)(uint8_t index, bool pressed);
static void (*input_cb)(enum rsim_input_type type, uint16_t code, int32_t value);

static int64_t local_now_us(void)
{
	return k_ticks_to_us_floor64(k_uptime_ticks());
}

/* Host time to this node's uptime, in microseconds. */
static int64_t host_to_local_us(uint64_t t_host)
{
	return local_now_us() - ((int64_t)rsim_bottom_now_us() - (int64_t)t_host);
}

static unsigned int current_sf(void)
{
	return profile == RADIO_PROFILE_PAIR ? PAIR_SF : LINK_SF;
}

static void hdr_init(struct rsim_hdr *h, enum rsim_type type)
{
	h->magic = RSIM_MAGIC;
	h->version = RSIM_VERSION;
	h->type = type;
}

static void send_hello(void)
{
	struct rsim_hello m = {
		.role = role,
		.sf = current_sf(),
		.leds = leds,
		.pid = (uint32_t)rsim_bottom_getpid(),
		.uid = uid,
	};

	hdr_init(&m.hdr, RSIM_HELLO);
	memcpy(m.name, name, sizeof(m.name));
	rsim_bottom_send(&m, sizeof(m));
}

static void on_welcome(const struct rsim_welcome *m)
{
	sim_floor_dbm = m->noise_floor_dbm;
	node_id = m->node_id;
	last_welcome_ms = k_uptime_get();
	if (!was_connected) {
		was_connected = true;
		floor_q4 = sim_floor_dbm * 16;
		LOG_INF("simulator: node %u, noise floor %d dBm", node_id, sim_floor_dbm);
	}
}

/*
 * How late received frames are, after they ended on air: when the
 * simulator's message arrived, and when the link thread took the frame.
 * A reply slot starts a few ms after the poll, so either shows up as a
 * missed slot.
 */
struct lateness {
	int64_t max_us;
	uint32_t n;
	uint32_t over_3ms;
};
static struct lateness late_arrive, late_take;

static void note_late(struct lateness *l, int64_t us)
{
	l->max_us = MAX(l->max_us, us);
	l->n++;
	l->over_3ms += us > 3000;
}

static void on_rx(const struct rsim_rx *m, int n)
{
	struct rx_packet pkt;

	if (n < (int)sizeof(*m) || m->len > n - (int)sizeof(*m)) {
		return;
	}

	pkt.info.rssi_dbm = m->rssi_dbm;
	pkt.info.snr_db = m->snr_db;
	pkt.info.time_us = host_to_local_us(m->t_end);
	note_late(&late_arrive, (int64_t)local_now_us() - pkt.info.time_us);
	pkt.len = m->len;
	memcpy(pkt.data, m->data, m->len);

	if (k_msgq_put(&rx_queue, &pkt, K_NO_WAIT)) {
		LOG_WRN("rx queue full, dropped %u-byte packet", m->len);
	}
}

static void on_energy(const struct rsim_energy *m)
{
	uint64_t now = rsim_bottom_now_us();
	struct energy *slot = NULL;

	for (int i = 0; i < ENERGY_SLOTS; i++) {
		struct energy *e = &energy[i];

		if (e->used && e->id == m->id) {
			slot = e;
			break;
		}
		if (!slot && (!e->used ||
			      (e->t_end != RSIM_T_OPEN && e->t_end + ENERGY_KEEP_US < now))) {
			slot = e;
		}
	}
	if (!slot) {
		LOG_WRN("energy table full");
		return;
	}

	*slot = (struct energy){
		.id = m->id,
		.t_start = m->t_start,
		.t_end = m->t_end,
		.rssi = m->rssi_dbm,
		.used = true,
	};
}

static void on_energy_end(const struct rsim_energy_end *m)
{
	for (int i = 0; i < ENERGY_SLOTS; i++) {
		if (energy[i].used && energy[i].id == m->id) {
			energy[i].t_end = m->t_end;
		}
	}
}

static void queue_ui(const struct rsim_input *in)
{
	struct ui_event ev = { .in = *in };

	if (k_msgq_put(&ui_queue, &ev, K_NO_WAIT)) {
		LOG_WRN("input queue full");
	}
}

static void handle(const uint8_t *buf, int n)
{
	const struct rsim_hdr *h = (const void *)buf;

	if (n < (int)sizeof(*h) || h->magic != RSIM_MAGIC || h->version != RSIM_VERSION) {
		return;
	}

	switch (h->type) {
	case RSIM_WELCOME:
		if (n >= (int)sizeof(struct rsim_welcome)) {
			on_welcome((const void *)buf);
		}
		break;
	case RSIM_RX:
		on_rx((const void *)buf, n);
		break;
	case RSIM_ENERGY:
		if (n >= (int)sizeof(struct rsim_energy)) {
			on_energy((const void *)buf);
		}
		break;
	case RSIM_ENERGY_END:
		if (n >= (int)sizeof(struct rsim_energy_end)) {
			on_energy_end((const void *)buf);
		}
		break;
	case RSIM_BUTTON:
		if (n >= (int)sizeof(struct rsim_button)) {
			const struct rsim_button *b = (const void *)buf;
			struct rsim_input in = {
				.type = 0,
				.code = b->index,
				.value = b->pressed,
			};

			queue_ui(&in);
		}
		break;
	case RSIM_INPUT:
		if (n >= (int)sizeof(struct rsim_input)) {
			queue_ui((const void *)buf);
		}
		break;
	default:
		break;
	}
}

/* Take everything the simulator has sent. Call with the lock held. */
static void drain(void)
{
	static uint8_t buf[512];
	int n;

	while ((n = rsim_bottom_recv(buf, sizeof(buf))) > 0) {
		handle(buf, n);
	}
}

static void ui_work_fn(struct k_work *work)
{
	struct ui_event ev;

	while (k_msgq_get(&ui_queue, &ev, K_NO_WAIT) == 0) {
		if (ev.in.type == 0) {
			if (button_cb) {
				button_cb(ev.in.code, ev.in.value != 0);
			}
		} else if (input_cb) {
			input_cb(ev.in.type, ev.in.code, ev.in.value);
		}
	}
}

static K_WORK_DEFINE(ui_work, ui_work_fn);

/*
 * How far the uptime lags the host clock, against the offset at start. The
 * host-to-uptime conversion is off by this much; slot timing needs it small.
 */
static int64_t clock_offset_us;
static int64_t lag_min_us = INT64_MAX, lag_max_us = INT64_MIN, lag_sum_us;
static uint32_t lag_n;

static void measure_lag(void)
{
	int64_t lag = ((int64_t)rsim_bottom_now_us() - local_now_us()) - clock_offset_us;

	lag_min_us = MIN(lag_min_us, lag);
	lag_max_us = MAX(lag_max_us, lag);
	lag_sum_us += lag;
	lag_n++;
}

static void rx_thread(void *p1, void *p2, void *p3)
{
	clock_offset_us = (int64_t)rsim_bottom_now_us() - local_now_us();

	while (true) {
		k_mutex_lock(&lock, K_FOREVER);
		measure_lag();
		drain();
		k_mutex_unlock(&lock);

		if (k_msgq_num_used_get(&ui_queue)) {
			k_work_submit(&ui_work);
		}

		k_sleep(K_USEC(POLL_US));
	}
}

K_THREAD_DEFINE(radio_sim_rx, 2048, rx_thread, NULL, NULL, NULL, K_PRIO_COOP(2), 0, 0);

static void heartbeat_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(heartbeat, heartbeat_fn);

static void heartbeat_fn(struct k_work *work)
{
	if (was_connected && k_uptime_get() - last_welcome_ms > LOST_AFTER_MS) {
		was_connected = false;
		LOG_WRN("simulator lost, retrying");
	}
	send_hello();
	k_work_reschedule(&heartbeat, K_MSEC(HEARTBEAT_MS));
}

bool radio_sim_connected(void)
{
	return k_uptime_get() - last_welcome_ms <= LOST_AFTER_MS;
}

/*
 * The RSSI a receiver here would read at host time @p t: the loudest of
 * the noise floor and the energy on air. Call with the lock held.
 */
static int16_t rssi_at(uint64_t t)
{
	int16_t dbm = sim_floor_dbm;

	for (int i = 0; i < ENERGY_SLOTS; i++) {
		const struct energy *e = &energy[i];

		if (e->used && e->t_start <= t && t < e->t_end) {
			dbm = MAX(dbm, e->rssi);
		}
	}
	return dbm;
}

/* Like radio.c: the median of a burst of samples, here all taken at once. */
static void calibrate_floor(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	drain();
	floor_q4 = rssi_at(rsim_bottom_now_us()) * 16;
	k_mutex_unlock(&lock);
}

static void apply_tx_power(int8_t dbm)
{
	antenna_power_dbm = CLAMP(MIN(dbm, MAX_TX_POWER_DBM), CHIP_POWER_MIN, CHIP_POWER_MAX);
}

int radio_set_tx_power(int8_t dbm)
{
	link_power_dbm = dbm;
	if (profile == RADIO_PROFILE_LINK) {
		apply_tx_power(dbm);
	}
	return 0;
}

int8_t radio_tx_power(void)
{
	return antenna_power_dbm;
}

int radio_set_profile(enum radio_profile p)
{
	struct rsim_profile m;

	if (p == profile) {
		return 0;
	}

	k_mutex_lock(&tx_lock, K_FOREVER);
	profile = p;
	apply_tx_power(p == RADIO_PROFILE_PAIR ? PAIR_TX_POWER_DBM : link_power_dbm);
	hdr_init(&m.hdr, RSIM_PROFILE);
	m.sf = current_sf();
	rsim_bottom_send(&m, sizeof(m));
	k_mutex_unlock(&tx_lock);

	calibrate_floor();

	LOG_INF("%s profile: SF%u, %d dBm", p == RADIO_PROFILE_PAIR ? "pairing" : "link",
		current_sf(), antenna_power_dbm);
	return 0;
}

enum radio_profile radio_profile(void)
{
	return profile;
}

uint32_t radio_frequency(void)
{
	return FREQUENCY_HZ;
}

unsigned int radio_spreading_factor(void)
{
	return current_sf();
}

unsigned int radio_bandwidth_khz(void)
{
	return BANDWIDTH_KHZ;
}

uint32_t radio_airtime_ms(size_t len)
{
	return link_airtime_ms(len, current_sf(), BANDWIDTH_KHZ);
}

int radio_channel_clear(uint32_t listen_us, int margin_db, int16_t *peak_dbm)
{
	int n = listen_us / RSSI_SAMPLE_US + 1;
	int16_t peak = INT16_MIN;
	int16_t quiet = INT16_MAX;
	int threshold;
	uint64_t end;
	int ret;

	k_sleep(K_USEC(listen_us));

	k_mutex_lock(&lock, K_FOREVER);
	drain();

	/* The samples radio.c would have taken during the listen. */
	end = rsim_bottom_now_us();
	for (int i = 0; i < n; i++) {
		int16_t dbm = rssi_at(end - (uint64_t)(n - 1 - i) * RSSI_SAMPLE_US);

		peak = MAX(peak, dbm);
		quiet = MIN(quiet, dbm);
	}

	threshold = CLAMP(floor_q4 / 16 + margin_db, THRESHOLD_MIN_DBM, THRESHOLD_MAX_DBM);
	if (quiet * 16 < floor_q4) {
		floor_q4 = quiet * 16;
	} else {
		floor_q4 += MIN((quiet * 16 - floor_q4) / 32, FLOOR_RISE_Q4);
	}
	ret = peak <= threshold ? 1 : 0;

	k_mutex_unlock(&lock);

	if (peak_dbm) {
		*peak_dbm = peak;
	}
	return ret;
}

int16_t radio_noise_floor(void)
{
	return floor_q4 / 16;
}

int radio_init(const struct radio_frontend *fe)
{
	ARG_UNUSED(fe);

	apply_tx_power(link_power_dbm);
	LOG_INF("simulated radio \"%s\": %u.%03u MHz SF%u BW%u, %d dBm", name,
		FREQUENCY_HZ / 1000000, (FREQUENCY_HZ / 1000) % 1000, current_sf(),
		BANDWIDTH_KHZ, antenna_power_dbm);
	if (!radio_sim_connected()) {
		LOG_WRN("no answer from the radio simulator at %s yet",
			opt_addr ? opt_addr : "127.0.0.1:7700");
	}
	calibrate_floor();
	return 0;
}

int radio_send(const void *buf, size_t len)
{
	static uint8_t msg[sizeof(struct rsim_tx) + RADIO_MAX_PAYLOAD];
	struct rsim_tx *m = (void *)msg;
	uint32_t airtime;

	if (len > RADIO_MAX_PAYLOAD) {
		return -EMSGSIZE;
	}

	k_mutex_lock(&tx_lock, K_FOREVER);

	if (TX_DELAY_US) {
		k_sleep(K_USEC(TX_DELAY_US));
	}

	airtime = link_airtime_us(len, current_sf(), BANDWIDTH_KHZ);
	hdr_init(&m->hdr, RSIM_TX);
	m->t_start = rsim_bottom_now_us();
	m->airtime_us = airtime;
	m->freq_hz = FREQUENCY_HZ;
	m->bw_khz = BANDWIDTH_KHZ;
	m->sf = current_sf();
	m->power_dbm = antenna_power_dbm;
	m->len = len;
	memcpy(m->data, buf, len);
	rsim_bottom_send(m, sizeof(*m) + len);

	/* On air until the last symbol is out; ends at the same host time the
	 * simulator counts with.
	 */
	k_sleep(K_TIMEOUT_ABS_US(host_to_local_us(m->t_start + airtime)));

	k_mutex_unlock(&tx_lock);
	return 0;
}

int radio_recv(void *buf, size_t size, k_timeout_t timeout, struct radio_rx_info *info)
{
	struct rx_packet pkt;

	if (k_msgq_get(&rx_queue, &pkt, timeout)) {
		return -EAGAIN;
	}

	if (pkt.len > size) {
		return -EMSGSIZE;
	}

	note_late(&late_take, (int64_t)local_now_us() - pkt.info.time_us);
	memcpy(buf, pkt.data, pkt.len);
	*info = pkt.info;
	return pkt.len;
}

void radio_sim_report_leds(uint16_t mask)
{
	struct rsim_io m;

	if (mask == leds) {
		return;
	}
	leds = mask;
	hdr_init(&m.hdr, RSIM_IO);
	m.leds = mask;
	rsim_bottom_send(&m, sizeof(m));
}

int radio_sim_send_display(uint16_t width, uint16_t height, enum rsim_pixfmt format,
			   uint16_t stride, const void *buf, size_t len)
{
	static uint8_t msg[DISPLAY_MAX];
	static K_MUTEX_DEFINE(display_lock);
	struct rsim_display *m = (void *)msg;

	if (sizeof(*m) + len > sizeof(msg)) {
		return -EMSGSIZE;
	}

	k_mutex_lock(&display_lock, K_FOREVER);
	hdr_init(&m->hdr, RSIM_DISPLAY);
	m->width = width;
	m->height = height;
	m->format = format;
	m->reserved = 0;
	m->stride = stride;
	memcpy(m->data, buf, len);
	rsim_bottom_send(m, sizeof(*m) + len);
	k_mutex_unlock(&display_lock);
	return 0;
}

void radio_sim_set_button_cb(void (*cb)(uint8_t index, bool pressed))
{
	button_cb = cb;
}

void radio_sim_set_input_cb(void (*cb)(enum rsim_input_type type, uint16_t code, int32_t value))
{
	input_cb = cb;
}

/* The same fold of the hardware id as sat_link.c's read_uid(). */
static uint64_t read_uid(void)
{
	uint64_t v = 0;
#ifdef CONFIG_HWINFO
	uint8_t id[16] = {0};
	ssize_t n = hwinfo_get_device_id(id, sizeof(id));

	for (ssize_t i = 0; i < n; i++) {
		v ^= (uint64_t)id[i] << (8 * (i % 8));
	}
#endif
	return v;
}

static int radio_sim_start(void)
{
	char host[64] = "127.0.0.1";
	int port = RSIM_DEFAULT_PORT;

	if (opt_addr) {
		const char *colon = strrchr(opt_addr, ':');

		if (colon) {
			size_t n = MIN((size_t)(colon - opt_addr), sizeof(host) - 1);

			memcpy(host, opt_addr, n);
			host[n] = '\0';
			port = atoi(colon + 1);
		} else {
			strncpy(host, opt_addr, sizeof(host) - 1);
		}
	}

	if (opt_role) {
		if (strcmp(opt_role, "ctrl") == 0) {
			role = RSIM_ROLE_CTRL;
		} else if (strcmp(opt_role, "sat") == 0) {
			role = RSIM_ROLE_SAT;
		} else {
			role = RSIM_ROLE_OTHER;
		}
	}

	if (opt_name) {
		strncpy(name, opt_name, sizeof(name));
	} else {
		snprintk(name, sizeof(name), "%s-%ld",
			 role == RSIM_ROLE_CTRL ? "ctrl" : role == RSIM_ROLE_SAT ? "sat" : "node",
			 rsim_bottom_getpid());
	}

	uid = read_uid();

	if (rsim_bottom_open(host, port)) {
		LOG_ERR("cannot open a socket to the radio simulator at %s:%d", host, port);
		return -EIO;
	}

	k_work_reschedule(&heartbeat, K_NO_WAIT);
	return 0;
}

SYS_INIT(radio_sim_start, POST_KERNEL, 90);

static void radio_sim_options(void)
{
	static struct args_struct_t opts[] = {
		{
			.option = "radio_sim",
			.name = "host:port",
			.type = 's',
			.dest = (void *)&opt_addr,
			.descript = "Radio network simulator to connect to (default 127.0.0.1:7700)",
		},
		{
			.option = "radio_name",
			.name = "name",
			.type = 's',
			.dest = (void *)&opt_name,
			.descript = "This node's name in the radio simulator",
		},
		{
			.option = "radio_role",
			.name = "ctrl|sat",
			.type = 's',
			.dest = (void *)&opt_role,
			.descript = "Role reported to the radio simulator",
		},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(opts);
}

NATIVE_TASK(radio_sim_options, PRE_BOOT_1, 10);

static void radio_sim_rt_period(void)
{
	hwtimer_set_rt_period(RT_PERIOD_US);
}

/* Before the command line is parsed, so that --rt-period overrides it. */
NATIVE_TASK(radio_sim_rt_period, PRE_BOOT_1, 1);

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>

static int cmd_rsim(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "simulator %s, node %u \"%s\", noise floor %d dBm (tracked %d)",
		    radio_sim_connected() ? "connected" : "NOT connected", node_id, name,
		    sim_floor_dbm, radio_noise_floor());
	if (lag_n) {
		shell_print(sh, "uptime behind host clock: min %lld, avg %lld, max %lld us (%u samples)",
			    lag_min_us, lag_sum_us / lag_n, lag_max_us, lag_n);
	}
	lag_min_us = INT64_MAX;
	lag_max_us = INT64_MIN;
	lag_sum_us = 0;
	lag_n = 0;
	shell_print(sh, "frames late after their end: arrived max %lld us, %u of %u over 3 ms; "
		    "taken max %lld us, %u of %u over 3 ms",
		    late_arrive.max_us, late_arrive.over_3ms, late_arrive.n, late_take.max_us,
		    late_take.over_3ms, late_take.n);
	late_arrive = (struct lateness){0};
	late_take = (struct lateness){0};
	return 0;
}

SHELL_CMD_REGISTER(rsim, NULL, "Simulated radio: connection and clock lag", cmd_rsim);
#endif
