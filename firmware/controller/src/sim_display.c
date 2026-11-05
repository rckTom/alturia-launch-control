/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The controller's screen in the radio network simulator's web UI. A 1-bit
 * framebuffer that LVGL draws into like any display; whenever it changes,
 * the whole frame goes to the simulator (12 kB at 400x240), at most
 * MAX_FPS times a second, and again once a second so that a simulator or
 * browser that attaches later gets a picture.
 */

#define DT_DRV_COMPAT zephyr_sim_display

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/dt-bindings/display/panel.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "radio_sim.h"

LOG_MODULE_REGISTER(sim_display, LOG_LEVEL_INF);

#define WIDTH  DT_INST_PROP(0, width)
#define HEIGHT DT_INST_PROP(0, height)
#define STRIDE (WIDTH / 8)

#define MAX_FPS      30
#define REFRESH_MS   1000

BUILD_ASSERT(WIDTH % 8 == 0, "rows must be whole bytes");

static uint8_t fb[STRIDE * HEIGHT];
static enum display_pixel_format format = PIXEL_FORMAT_MONO01;
static bool blanked = true;
static int64_t sent_at;

static void send_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(send_work, send_work_fn);

static void send_work_fn(struct k_work *work)
{
	/* What went out last; LVGL redraws a lot that does not change. */
	static uint8_t sent[STRIDE * HEIGHT];
	static uint8_t blank[STRIDE * HEIGHT];
	static int64_t refresh_at;
	enum rsim_pixfmt fmt = format == PIXEL_FORMAT_MONO01 ? RSIM_PIX_MONO01 : RSIM_PIX_MONO10;
	int64_t now = k_uptime_get();
	const uint8_t *frame = fb;

	/* Blanked shows as all white, as it would on the panel. */
	if (blanked) {
		memset(blank, fmt == RSIM_PIX_MONO01 ? 0xFF : 0x00, sizeof(blank));
		frame = blank;
	}

	if (memcmp(sent, frame, sizeof(sent)) != 0 || now >= refresh_at) {
		memcpy(sent, frame, sizeof(sent));
		radio_sim_send_display(WIDTH, HEIGHT, fmt, STRIDE, sent, sizeof(sent));
		sent_at = now;
		refresh_at = now + REFRESH_MS;
	}

	k_work_reschedule(&send_work, K_MSEC(refresh_at - now));
}

/* Send soon, but no more often than MAX_FPS. */
static void changed(void)
{
	int64_t due = sent_at + 1000 / MAX_FPS;
	int64_t now = k_uptime_get();

	k_work_reschedule(&send_work, K_MSEC(due > now ? due - now : 0));
}

static int sim_display_write(const struct device *dev, const uint16_t x, const uint16_t y,
			     const struct display_buffer_descriptor *desc, const void *buf)
{
	const uint8_t *src = buf;
	uint16_t src_stride = DIV_ROUND_UP(desc->pitch, 8);

	/* LVGL's mono rounder keeps x and the width on whole bytes. */
	if (x % 8 || x + desc->width > WIDTH || y + desc->height > HEIGHT) {
		LOG_WRN_ONCE("write at %u,%u size %ux%u refused", x, y, desc->width, desc->height);
		return -EINVAL;
	}

	for (uint16_t row = 0; row < desc->height; row++) {
		memcpy(&fb[(y + row) * STRIDE + x / 8], &src[row * src_stride],
		       DIV_ROUND_UP(desc->width, 8));
	}

	if (!desc->frame_incomplete) {
		changed();
	}
	return 0;
}

static int sim_display_blanking_on(const struct device *dev)
{
	blanked = true;
	changed();
	return 0;
}

static int sim_display_blanking_off(const struct device *dev)
{
	blanked = false;
	changed();
	return 0;
}

static void sim_display_get_capabilities(const struct device *dev,
					 struct display_capabilities *caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = WIDTH;
	caps->y_resolution = HEIGHT;
	caps->supported_pixel_formats = PIXEL_FORMAT_MONO01 | PIXEL_FORMAT_MONO10;
	caps->current_pixel_format = format;
	/* Rows of bytes, leftmost pixel in the MSB: what the web UI draws. */
	caps->screen_info = SCREEN_INFO_MONO_MSB_FIRST;
	caps->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

static int sim_display_set_pixel_format(const struct device *dev,
					const enum display_pixel_format pf)
{
	if (pf != PIXEL_FORMAT_MONO01 && pf != PIXEL_FORMAT_MONO10) {
		return -ENOTSUP;
	}
	format = pf;
	return 0;
}

static DEVICE_API(display, sim_display_api) = {
	.write = sim_display_write,
	.blanking_on = sim_display_blanking_on,
	.blanking_off = sim_display_blanking_off,
	.get_capabilities = sim_display_get_capabilities,
	.set_pixel_format = sim_display_set_pixel_format,
};

static int sim_display_init(const struct device *dev)
{
	format = DT_INST_PROP(0, pixel_format) == PANEL_PIXEL_FORMAT_MONO10 ? PIXEL_FORMAT_MONO10
									    : PIXEL_FORMAT_MONO01;
	memset(fb, format == PIXEL_FORMAT_MONO01 ? 0xFF : 0x00, sizeof(fb));
	return 0;
}

DEVICE_DT_INST_DEFINE(0, sim_display_init, NULL, NULL, NULL, POST_KERNEL,
		      CONFIG_DISPLAY_INIT_PRIORITY, &sim_display_api);
