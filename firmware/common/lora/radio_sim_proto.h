/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Wire format between a native_sim node (radio_sim.c) and the radio
 * network simulator (tools/radiosim). One message per UDP datagram,
 * packed little-endian. tools/radiosim/radiosim/proto.py mirrors this file;
 * change both together and bump RSIM_VERSION.
 *
 * Every time is host CLOCK_MONOTONIC in microseconds, which all nodes and
 * the simulator share. A node converts it to its own uptime when needed.
 */

#ifndef RADIO_SIM_PROTO_H_
#define RADIO_SIM_PROTO_H_

#include <stdint.h>

#define RSIM_MAGIC   0x5352u   /* "RS" */
#define RSIM_VERSION 1
#define RSIM_DEFAULT_PORT 7700

/* An ENERGY entry that lasts until an ENERGY_END. */
#define RSIM_T_OPEN UINT64_MAX

enum rsim_type {
	/* node -> simulator */
	RSIM_HELLO = 1,     /* registration, and a heartbeat every second */
	RSIM_TX = 2,        /* a frame goes on air */
	RSIM_PROFILE = 3,   /* the receiver now listens on another SF */
	RSIM_IO = 4,        /* LED state changed */
	RSIM_DISPLAY = 5,   /* a whole display frame */

	/* simulator -> node */
	RSIM_WELCOME = 64,  /* reply to HELLO */
	RSIM_RX = 65,       /* a frame this node received */
	RSIM_ENERGY = 66,   /* RF energy at this node, for listen-before-talk */
	RSIM_ENERGY_END = 67,
	RSIM_BUTTON = 68,   /* a button of the node's own */
	RSIM_INPUT = 69,    /* an input event (controller keys, encoder) */
};

enum rsim_role {
	RSIM_ROLE_OTHER = 0,
	RSIM_ROLE_CTRL = 1,
	RSIM_ROLE_SAT = 2,
};

struct rsim_hdr {
	uint16_t magic;
	uint8_t version;
	uint8_t type;
} __attribute__((packed));

struct rsim_hello {
	struct rsim_hdr hdr;
	uint8_t role;
	uint8_t sf;          /* spreading factor the receiver is on */
	uint16_t leds;
	uint32_t pid;
	uint64_t uid;
	char name[16];       /* NUL-padded */
} __attribute__((packed));

struct rsim_tx {
	struct rsim_hdr hdr;
	uint64_t t_start;
	uint32_t airtime_us;
	uint32_t freq_hz;
	uint16_t bw_khz;
	uint8_t sf;
	int8_t power_dbm;
	uint8_t len;
	uint8_t data[];
} __attribute__((packed));

struct rsim_profile {
	struct rsim_hdr hdr;
	uint8_t sf;
} __attribute__((packed));

struct rsim_io {
	struct rsim_hdr hdr;
	uint16_t leds;
} __attribute__((packed));

enum rsim_pixfmt {
	/* 1 bit per pixel, rows of bytes, leftmost pixel in the MSB. */
	RSIM_PIX_MONO01 = 1,  /* 1 = white, as Zephyr's PIXEL_FORMAT_MONO01 */
	RSIM_PIX_MONO10 = 2,  /* 1 = black */
};

struct rsim_display {
	struct rsim_hdr hdr;
	uint16_t width;
	uint16_t height;
	uint8_t format;
	uint8_t reserved;
	uint16_t stride;      /* bytes per row */
	uint8_t data[];       /* stride * height bytes */
} __attribute__((packed));

struct rsim_welcome {
	struct rsim_hdr hdr;
	uint16_t node_id;
	int16_t noise_floor_dbm;
} __attribute__((packed));

struct rsim_rx {
	struct rsim_hdr hdr;
	uint64_t t_end;
	int16_t rssi_dbm;
	int8_t snr_db;
	uint8_t sf;
	uint8_t len;
	uint8_t data[];
} __attribute__((packed));

struct rsim_energy {
	struct rsim_hdr hdr;
	uint32_t id;
	uint64_t t_start;
	uint64_t t_end;       /* RSIM_T_OPEN until an ENERGY_END */
	int16_t rssi_dbm;
} __attribute__((packed));

struct rsim_energy_end {
	struct rsim_hdr hdr;
	uint32_t id;
	uint64_t t_end;
} __attribute__((packed));

struct rsim_button {
	struct rsim_hdr hdr;
	uint8_t index;
	uint8_t pressed;
} __attribute__((packed));

enum rsim_input_type {
	RSIM_IN_PIN = 1,    /* code = gpio0 pin, value = level */
	RSIM_IN_WHEEL = 2,  /* value = detents, signed */
	RSIM_IN_TOUCH = 3,  /* value = pressed */
};

struct rsim_input {
	struct rsim_hdr hdr;
	uint8_t type;
	uint8_t reserved;
	uint16_t code;
	int32_t value;
} __attribute__((packed));

#endif /* RADIO_SIM_PROTO_H_ */
