/* SPDX-License-Identifier: GPL-3.0-only */

#include "link_sched.h"
#include "link_transport.h"
#include "radio.h"

static int send(const void *buf, size_t len)
{
	return radio_send(buf, len);
}

static int recv(void *buf, size_t size, k_timeout_t timeout, struct link_rx_info *info)
{
	struct radio_rx_info rx;
	int len = radio_recv(buf, size, timeout, &rx);

	if (len >= 0) {
		info->rssi_dbm = rx.rssi_dbm;
		info->snr_db = rx.snr_db;
		info->time_us = rx.time_us;
	}
	return len;
}

static int set_profile(enum link_profile p)
{
	return radio_set_profile(p == LINK_PROFILE_PAIR ? RADIO_PROFILE_PAIR : RADIO_PROFILE_LINK);
}

static enum link_profile profile(void)
{
	return radio_profile() == RADIO_PROFILE_PAIR ? LINK_PROFILE_PAIR : LINK_PROFILE_LINK;
}

static int channel_clear(uint32_t listen_us, int16_t *peak_dbm)
{
	return radio_channel_clear(listen_us, LINK_LBT_MARGIN_DB, peak_dbm);
}

const struct link_transport link_radio_transport = {
	.send = send,
	.recv = recv,
	.wake = radio_wake,
	.set_profile = set_profile,
	.profile = profile,
	.channel_clear = channel_clear,
	.airtime_ms = radio_airtime_ms,
};
