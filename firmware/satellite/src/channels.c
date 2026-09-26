/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>

#include "satellite.h"

LOG_MODULE_REGISTER(channels, LOG_LEVEL_INF);

#define EN_SPEC(node) GPIO_DT_SPEC_GET(DT_NODELABEL(node), gpios)

/* High-side enable into the TPS1HB08, low-side gate to the igniter FET.
 * Both must be asserted for current to flow; the arm relay holds the rail.
 */
static const struct gpio_dt_spec enable_high[SAT_CHANNELS] = {
	EN_SPEC(en_h1), EN_SPEC(en_h2), EN_SPEC(en_h3),
	EN_SPEC(en_h4), EN_SPEC(en_h5), EN_SPEC(en_h6),
};

static const struct gpio_dt_spec enable_low[SAT_CHANNELS] = {
	EN_SPEC(en_l1), EN_SPEC(en_l2), EN_SPEC(en_l3),
	EN_SPEC(en_l4), EN_SPEC(en_l5), EN_SPEC(en_l6),
};

static const struct gpio_dt_spec hs_latch = EN_SPEC(hs_latch);
static const struct gpio_dt_spec hs_dia_en = EN_SPEC(hs_dia_en);

/* Continuity is the 24-bit ADC across the igniter; the 12-bit part carries
 * the rail dividers.
 */
static const struct device *const adc_cont = DEVICE_DT_GET(DT_NODELABEL(adc_continuity));
static const struct device *const adc_rails = DEVICE_DT_GET(DT_NODELABEL(adc_rails));

static bool armed[SAT_CHANNELS];
static int16_t sample_buf;

/* Low sides on: a fire pulse in progress, and valves held open. */
static uint8_t pulsing;
static uint8_t valves;

static void pulse_end_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(pulse_end, pulse_end_fn);

static int adc_setup(const struct device *dev, uint8_t ch, uint8_t ref)
{
	struct adc_channel_cfg cfg = {
		.gain = ADC_GAIN_1,
		.reference = ref,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.channel_id = ch,
		.differential = 0,
	};

	return adc_channel_setup(dev, &cfg);
}

static int adc_read_ch(const struct device *dev, uint8_t ch, int32_t *mv)
{
	struct adc_sequence seq = {
		.channels = BIT(ch),
		.buffer = &sample_buf,
		.buffer_size = sizeof(sample_buf),
		.resolution = 12,
	};
	int ret;

	ret = adc_read(dev, &seq);
	if (ret) {
		return ret;
	}

	*mv = sample_buf;
	return adc_raw_to_millivolts(adc_ref_internal(dev), ADC_GAIN_1, seq.resolution, mv);
}

int channels_init(void)
{
	int ret;

	for (int i = 0; i < SAT_CHANNELS; i++) {
		if (!gpio_is_ready_dt(&enable_high[i]) || !gpio_is_ready_dt(&enable_low[i])) {
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&enable_high[i], GPIO_OUTPUT_INACTIVE);
		if (ret) {
			return ret;
		}

		ret = gpio_pin_configure_dt(&enable_low[i], GPIO_OUTPUT_INACTIVE);
		if (ret) {
			return ret;
		}
	}

	ret = gpio_pin_configure_dt(&hs_latch, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		return ret;
	}

	ret = gpio_pin_configure_dt(&hs_dia_en, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		return ret;
	}

	if (!device_is_ready(adc_cont) || !device_is_ready(adc_rails)) {
		return -ENODEV;
	}

	for (int ch = 0; ch < SAT_CHANNELS; ch++) {
		ret = adc_setup(adc_cont, ch, ADC_REF_INTERNAL);
		if (ret) {
			LOG_ERR("continuity adc setup, channel %d: %d", ch + 1, ret);
			return ret;
		}
	}

	ret = adc_setup(adc_rails, 0, ADC_REF_EXTERNAL0);
	if (ret) {
		LOG_ERR("rail adc setup: %d", ret);
		return ret;
	}

	return 0;
}

/* A channel's low side is on while it pulses or holds a valve, and armed. */
static int update_low(int ch)
{
	bool on = armed[ch] && ((pulsing | valves) & BIT(ch));

	return gpio_pin_set_dt(&enable_low[ch], on);
}

int channels_arm(int ch, bool on)
{
	int ret;

	if (ch < 0 || ch >= SAT_CHANNELS) {
		return -EINVAL;
	}

	armed[ch] = on;
	if (!on) {
		pulsing &= ~BIT(ch);
		valves &= ~BIT(ch);
	}
	/* Low side first when disarming, high side first when arming. */
	ret = on ? gpio_pin_set_dt(&enable_high[ch], 1) : update_low(ch);
	if (ret) {
		return ret;
	}
	return on ? update_low(ch) : gpio_pin_set_dt(&enable_high[ch], 0);
}

uint8_t channels_armed_mask(void)
{
	uint8_t mask = 0;

	for (int ch = 0; ch < SAT_CHANNELS; ch++) {
		if (armed[ch]) {
			mask |= BIT(ch);
		}
	}
	return mask;
}

static void pulse_end_fn(struct k_work *work)
{
	uint8_t ended = pulsing;

	pulsing = 0;
	for (int ch = 0; ch < SAT_CHANNELS; ch++) {
		if (ended & BIT(ch)) {
			(void)update_low(ch);
		}
	}
}

int channels_pulse(uint8_t mask, uint32_t ms)
{
	if (mask & ~channels_armed_mask()) {
		return -EPERM;
	}
	if (pulsing) {
		return -EBUSY;
	}

	pulsing = mask;
	for (int ch = 0; ch < SAT_CHANNELS; ch++) {
		if (mask & BIT(ch)) {
			(void)update_low(ch);
		}
	}
	k_work_reschedule(&pulse_end, K_MSEC(ms));
	return 0;
}

uint8_t channels_pulsing(void)
{
	return pulsing;
}

int channels_valves(uint8_t mask)
{
	uint8_t changed;

	mask &= channels_armed_mask();
	changed = mask ^ valves;
	valves = mask;
	for (int ch = 0; ch < SAT_CHANNELS; ch++) {
		if (changed & BIT(ch)) {
			(void)update_low(ch);
		}
	}
	return 0;
}

uint8_t channels_valve_mask(void)
{
	return valves;
}

bool channels_all_safe(void)
{
	for (int i = 0; i < SAT_CHANNELS; i++) {
		if (armed[i]) {
			return false;
		}
	}

	return true;
}

int channels_fire(int ch, uint32_t ms)
{
	int ret;

	if (ch < 0 || ch >= SAT_CHANNELS) {
		return -EINVAL;
	}

	if (!armed[ch]) {
		return -EPERM;
	}

	ret = gpio_pin_set_dt(&enable_low[ch], 1);
	if (ret) {
		return ret;
	}

	k_msleep(ms);

	return gpio_pin_set_dt(&enable_low[ch], 0);
}

int channels_continuity(int ch, int32_t *mv)
{
	if (ch < 0 || ch >= SAT_CHANNELS) {
		return -EINVAL;
	}

	return adc_read_ch(adc_cont, ch, mv);
}

int channels_rail_mv(int input, int32_t *mv)
{
	return adc_read_ch(adc_rails, input, mv);
}

int channels_diagnostics(bool on)
{
	return gpio_pin_set_dt(&hs_dia_en, on);
}

int channels_latch_clear(void)
{
	int ret = gpio_pin_set_dt(&hs_latch, 1);

	if (ret) {
		return ret;
	}

	k_msleep(1);
	return gpio_pin_set_dt(&hs_latch, 0);
}
