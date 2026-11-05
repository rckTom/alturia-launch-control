/* SPDX-License-Identifier: GPL-3.0-only */

#undef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "radio_sim_bottom.h"

static int sock = -1;

int rsim_bottom_open(const char *host, int port)
{
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_DGRAM,
	};
	struct addrinfo *res;
	char service[8];
	int fd;

	snprintf(service, sizeof(service), "%d", port);
	if (getaddrinfo(host, service, &hints, &res) != 0) {
		return -1;
	}

	fd = socket(res->ai_family, res->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		freeaddrinfo(res);
		return -1;
	}

	/* Connected: send() goes to the simulator, recv() only hears it. */
	if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
		close(fd);
		freeaddrinfo(res);
		return -1;
	}
	freeaddrinfo(res);

	/* A display frame is some 12 kB; leave room for a burst of them. */
	int size = 1 << 20;

	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));

	sock = fd;
	return 0;
}

int rsim_bottom_send(const void *buf, int len)
{
	ssize_t n;

	if (sock < 0) {
		return -1;
	}
	n = send(sock, buf, len, 0);
	/* ECONNREFUSED: the simulator is not up (yet); the heartbeat retries. */
	return n < 0 ? -1 : (int)n;
}

int rsim_bottom_recv(void *buf, int size)
{
	ssize_t n;

	if (sock < 0) {
		return -1;
	}
	n = recv(sock, buf, size, 0);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
		    errno == ECONNREFUSED) {
			return 0;
		}
		return -1;
	}
	return (int)n;
}

uint64_t rsim_bottom_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

long rsim_bottom_getpid(void)
{
	return (long)getpid();
}
