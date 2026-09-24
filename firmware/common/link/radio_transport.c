/* SPDX-License-Identifier: Apache-2.0 */

#include "link_transport.h"
#include "radio.h"

static int send(const void *buf, size_t len)
{
	return radio_send(buf, len);
}

static int recv(void *buf, size_t size, k_timeout_t timeout, int16_t *rssi_dbm, int8_t *snr_db)
{
	struct radio_rx_info info;
	int len = radio_recv(buf, size, timeout, &info);

	if (len >= 0) {
		*rssi_dbm = info.rssi_dbm;
		*snr_db = info.snr_db;
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

const struct link_transport link_radio_transport = {
	.send = send,
	.recv = recv,
	.set_profile = set_profile,
	.profile = profile,
};
