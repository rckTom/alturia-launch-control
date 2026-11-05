/* SPDX-License-Identifier: Apache-2.0 */

#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>

#include "satellite.h"

LOG_MODULE_REGISTER(channels, LOG_LEVEL_INF);

#define EN_SPEC(node) GPIO_DT_SPEC_GET(DT_NODELABEL(node), gpios)

/* High-side enable into the TPS1HB08, low-side gate to the igniter FET.
 * Both must be asserted for current to flow; the arm relay holds the rail.
 * Arming switches neither: both go on together only for a fire pulse or a
 * held valve, so while armed a single stuck switch still fires nothing.
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

/* The ADS124S08 has a single ADC channel whose input mux picks AINn
 * against AINCOM (GND); AIN(n - 1) is channel n. Its codes are 24-bit
 * two's complement, full scale at +-Vref. The native_sim emulator has one
 * ADC channel per input instead.
 */
#define CONT_IS_ADS124S08 DT_NODE_HAS_COMPAT(DT_NODELABEL(adc_continuity), ti_ads124s08)
#define CONT_AINCOM 12

static K_MUTEX_DEFINE(cont_lock);

static bool armed[SAT_CHANNELS];
static int16_t sample_buf;

/* Low sides on: a fire pulse in progress, and valves held open. */
static uint8_t pulsing;
static uint8_t valves;

static void pulse_end_fn(struct k_work *work);
static int update_outputs(int ch);
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

static int cont_setup_ref(int ch, enum adc_reference ref)
{
	struct adc_channel_cfg cfg = {
		.gain = ADC_GAIN_1,
		.reference = ref,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
#if CONT_IS_ADS124S08
		.channel_id = 0,
		.input_positive = ch,
		.input_negative = CONT_AINCOM,
#else
		.channel_id = ch,
#endif
	};

	return adc_channel_setup(adc_cont, &cfg);
}

static int cont_setup(int ch)
{
	return cont_setup_ref(ch, ADC_REF_INTERNAL);
}

#if CONT_IS_ADS124S08
/* One conversion of AIN(ch) against @p ref, as a fraction of full scale
 * (bipolar: +-1 at +-Vref). Caller holds cont_lock.
 */
static int cont_convert(int ch, enum adc_reference ref, float *frac)
{
	int32_t raw;
	struct adc_sequence seq = {
		.channels = BIT(0),
		.buffer = &raw,
		.buffer_size = sizeof(raw),
		.resolution = 24,
	};
	int ret;

	ret = cont_setup_ref(ch, ref);
	if (!ret) {
		ret = adc_read(adc_cont, &seq);
	}
	if (ret) {
		return ret;
	}

	*frac = (float)raw / (float)BIT(23);
	return 0;
}

/* One conversion of AIN(ch) against the internal reference, in microvolts. */
static int cont_convert_uv(int ch, int32_t *uv)
{
	float frac;
	int ret = cont_convert(ch, ADC_REF_INTERNAL, &frac);

	if (!ret) {
		*uv = (int32_t)(frac * (float)adc_ref_internal(adc_cont) * 1000.0f);
	}
	return ret;
}
#endif

static int cont_read(int ch, int32_t *mv)
{
#if CONT_IS_ADS124S08
	int32_t uv = 0;
	int ret;

	k_mutex_lock(&cont_lock, K_FOREVER);
	ret = cont_convert_uv(ch, &uv);
	k_mutex_unlock(&cont_lock);

	if (!ret) {
		*mv = uv / 1000;
	}
	return ret;
#else
	return adc_read_ch(adc_cont, ch, mv);
#endif
}

#if CONT_IS_ADS124S08 && DT_NODE_EXISTS(DT_NODELABEL(cont_en))
/*
 * The continuity source (hardware/satellite output_channel, 06_sense_ui):
 *
 *   +3.3VA - Q1 (CONT_EN) - R12 270R - V_TEST - 1N4148 - 1k - PYRO_H - R - PYRO_L - low side - GND
 *                                        |                        `- 3k3 - tap - 1k - GND
 *                                      REFP0                                `- 10k - AINn
 *
 * The ADC reads the tap, PYRO_H / 4.3. The current through R is what the
 * 1k carries, (V_TEST - diode - PYRO_H) / 1k, less the divider's share.
 * V_TEST is REFP0, so converting one tap against both REFP0 and the
 * internal reference gives V_TEST itself (design/satellite_schematic_spec.md
 * §11.1 has the reference on V_TEST). The diode drop does not scale with
 * V_TEST and is measured instead:
 *
 *   A  low side off: this channel draws through diode, 1k and divider
 *      only. Its tap against both references gives V_TEST, PYRO_H and the
 *      current, hence the diode drop at that current.
 *   B  low side on: this channel's tap gives PYRO_H; an idle channel's tap
 *      against both references gives V_TEST under this load. The diode
 *      drop is A's plus the diode-law step to the higher current.
 *   Z  V_TEST off, low side still on: the output is at 0 V, so what the
 *      ADC reads is its own zero, tens of uV against ~0.4 mV per ohm at
 *      the tap; it is subtracted from A and B.
 *
 * If no other channel is idle, V_TEST under load is solved from R12's sag
 * instead. What is left is the 1 % resistors and the diode law's ideality
 * and temperature, a few mV against the ~1.7 V across the 1k.
 */
#define CONT_R_SRC 270.0f	/* R12 */
#define CONT_R_SER 1000.0f	/* 1k into PYRO_H */
#define CONT_R_DIV 4300.0f	/* 3k3 + 1k sense divider */
#define CONT_DIV_RATIO 4.3f	/* PYRO_H / tap */
#define CONT_R_OPEN (CONT_R_SER + CONT_R_DIV)
#define CONT_N_OTHER ((float)(SAT_CHANNELS - 1))
/* 1N4148: ideality ~1.9, thermal voltage at 25 C. */
#define CONT_DIODE_NVT (1.9f * 0.0257f)
/* Below this the resistor carries too little of the test current to tell from open. */
#define CONT_OPEN_BELOW_A 20e-6f
/* V_TEST and the sense filter (10k x 100n) settle well inside this. */
#define CONT_SETTLE_MS 20

static const struct gpio_dt_spec cont_en = GPIO_DT_SPEC_GET(DT_NODELABEL(cont_en), gpios);

static int cont_en_init(void)
{
	if (!gpio_is_ready_dt(&cont_en)) {
		return -ENODEV;
	}
	return gpio_pin_configure_dt(&cont_en, GPIO_OUTPUT_INACTIVE);
}

struct cont_samples {
	float xa;		/* A: tap / V_TEST */
	int32_t tap_a_uv;	/* A: tap */
	int32_t tap_b_uv;	/* B: tap */
	float x_idle;		/* B: idle channel's tap / V_TEST */
	int32_t idle_uv;	/* B: idle channel's tap */
	int32_t zero_uv;	/* Z: tap with the output at 0 V */
};

/* A, B and Z above. Entered with V_TEST on; leaves V_TEST and the
 * channel's low side off. @p idle < 0: no idle channel to read.
 */
static int cont_sample(int ch, int idle, struct cont_samples *s)
{
	int ret;

	k_msleep(CONT_SETTLE_MS);
	ret = cont_convert(ch, ADC_REF_EXTERNAL0, &s->xa);
	if (!ret) {
		ret = cont_convert_uv(ch, &s->tap_a_uv);
	}
	if (ret) {
		return ret;
	}

	ret = gpio_pin_set_dt(&enable_low[ch], 1);
	if (!ret) {
		k_msleep(CONT_SETTLE_MS);
		ret = cont_convert_uv(ch, &s->tap_b_uv);
	}
	if (!ret && idle >= 0) {
		ret = cont_convert(idle, ADC_REF_EXTERNAL0, &s->x_idle);
		if (!ret) {
			ret = cont_convert_uv(idle, &s->idle_uv);
		}
	}
	(void)gpio_pin_set_dt(&cont_en, 0);
	if (!ret) {
		k_msleep(CONT_SETTLE_MS);
		ret = cont_convert_uv(ch, &s->zero_uv);
	}
	(void)update_outputs(ch);
	return ret;
}

int channels_continuity_mohm(int ch, struct cont_result *res)
{
	struct cont_samples s = {0};
	int idle = -1;
	int ret;

	if (ch < 0 || ch >= SAT_CHANNELS) {
		return -EINVAL;
	}

	/*
	 * The low side goes on below: never with the high side enabled. The
	 * check is under cont_lock, which channels_arm() takes too, so an ARM
	 * cannot come in between it and the low side.
	 */
	k_mutex_lock(&cont_lock, K_FOREVER);
	if (armed[ch]) {
		k_mutex_unlock(&cont_lock);
		return -EBUSY;
	}
	/* The neighbour whose tap shows V_TEST under load: unarmed, so its
	 * output is driven by the test source alone.
	 */
	for (int i = 1; i < SAT_CHANNELS; i++) {
		int j = (ch + i) % SAT_CHANNELS;

		if (!armed[j] && !((pulsing | valves) & BIT(j))) {
			idle = j;
			break;
		}
	}

	ret = gpio_pin_set_dt(&cont_en, 1);
	if (!ret) {
		ret = cont_sample(ch, idle, &s);
	}
	(void)gpio_pin_set_dt(&cont_en, 0);
	k_mutex_unlock(&cont_lock);
	if (ret) {
		return ret;
	}

	/* No V_TEST, so no reference: nothing below means anything. */
	if (s.xa < 0.01f) {
		return -EIO;
	}

	const float z = (float)s.zero_uv * 1e-6f;

	/* A: idle. The ratio and the absolute reading share the offset, so
	 * V_TEST is exact; all of this channel's current is the divider's.
	 */
	const float vt_a = (float)s.tap_a_uv * 1e-6f / s.xa;
	const float vh_a = ((float)s.tap_a_uv * 1e-6f - z) * CONT_DIV_RATIO;
	const float i_a = vh_a / CONT_R_DIV;
	const float vd_a = vt_a - i_a * CONT_R_OPEN;
	/* Six channels idle behind R12: the +3.3VA it hangs from. */
	const float v_sup = vt_a + CONT_R_SRC * (CONT_N_OTHER + 1.0f) * i_a;

	/* B: loaded. */
	const float vh_b = ((float)s.tap_b_uv * 1e-6f - z) * CONT_DIV_RATIO;
	const bool measured = idle >= 0 && s.x_idle > 0.01f && s.x_idle < 0.95f;
	const float vt_meas = measured ? (float)s.idle_uv * 1e-6f / s.x_idle : 0.0f;
	float vd_b = vd_a + CONT_DIODE_NVT * logf(5.0f);
	float vt_model = vt_a, vt_b = vt_a, i_b = i_a;

	for (int it = 0; it < 4; it++) {
		/* R12's sag: this channel loaded, the other five idle. */
		vt_model = (v_sup + CONT_R_SRC * (vd_b + vh_b) / CONT_R_SER +
			    CONT_N_OTHER * CONT_R_SRC * vd_a / CONT_R_OPEN) /
			   (1.0f + CONT_R_SRC / CONT_R_SER + CONT_N_OTHER * CONT_R_SRC / CONT_R_OPEN);
		vt_b = measured ? vt_meas : vt_model;
		i_b = (vt_b - vd_b - vh_b) / CONT_R_SER;
		vd_b = vd_a + (i_b > i_a ? CONT_DIODE_NVT * logf(i_b / i_a) : 0.0f);
	}

	const float i_r = i_b - vh_b / CONT_R_DIV;

	res->pyro_h_uv = (int32_t)(vh_b * 1e6f);
	res->v_test_uv = (int32_t)(vt_b * 1e6f);
	res->v_test_model_uv = (int32_t)(vt_model * 1e6f);
	res->v_test_from = measured ? idle : -1;
	res->diode_uv = (int32_t)(vd_b * 1e6f);
	res->zero_uv = s.zero_uv;
	res->current_na = (int32_t)(i_r * 1e9f);
	res->open = i_r < CONT_OPEN_BELOW_A;
	res->mohm = res->open ? INT32_MAX : (int32_t)MIN(vh_b / i_r * 1000.0f, (float)INT32_MAX);

	/* A diode drop outside this means the circuit is not the one modelled. */
	if (vd_a < 0.3f || vd_a > 0.85f) {
		return -ERANGE;
	}
	return 0;
}

int channels_check_high(int ch, int32_t *off_uv, int32_t *on_uv)
{
	int ret;

	if (ch < 0 || ch >= SAT_CHANNELS) {
		return -EINVAL;
	}

	k_mutex_lock(&cont_lock, K_FOREVER);
	if (armed[ch]) {
		k_mutex_unlock(&cont_lock);
		return -EBUSY;
	}

	/* The low side stays off throughout: no current through the igniter. */
	ret = gpio_pin_set_dt(&enable_low[ch], 0);
	if (!ret) {
		k_msleep(CONT_SETTLE_MS);
		ret = cont_convert_uv(ch, off_uv);
	}
	if (!ret) {
		ret = gpio_pin_set_dt(&enable_high[ch], 1);
		if (!ret) {
			k_msleep(CONT_SETTLE_MS);
			ret = cont_convert_uv(ch, on_uv);
		}
		(void)gpio_pin_set_dt(&enable_high[ch], 0);
	}
	k_mutex_unlock(&cont_lock);

	return ret;
}

#else

static int cont_en_init(void)
{
	return 0;
}

int channels_continuity_mohm(int ch, struct cont_result *res)
{
	return -ENOTSUP;
}

int channels_check_high(int ch, int32_t *off_uv, int32_t *on_uv)
{
	return -ENOTSUP;
}

#endif

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

	ret = cont_en_init();
	if (ret) {
		return ret;
	}

	if (!device_is_ready(adc_cont) || !device_is_ready(adc_rails)) {
		return -ENODEV;
	}

	for (int ch = 0; ch < SAT_CHANNELS; ch++) {
		ret = cont_setup(ch);
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

/*
 * A channel's switches are on while it pulses or holds a valve, and armed:
 * high side first going on, low side first going off.
 */
static int update_outputs(int ch)
{
	bool on = armed[ch] && ((pulsing | valves) & BIT(ch));
	int ret;

	if (on) {
		ret = gpio_pin_set_dt(&enable_high[ch], 1);
		return ret ? ret : gpio_pin_set_dt(&enable_low[ch], 1);
	}

	ret = gpio_pin_set_dt(&enable_low[ch], 0);
	int ret_h = gpio_pin_set_dt(&enable_high[ch], 0);

	return ret ? ret : ret_h;
}

int channels_arm(int ch, bool on)
{
	int ret;

	if (ch < 0 || ch >= SAT_CHANNELS) {
		return -EINVAL;
	}

	if (!on) {
		armed[ch] = false;
		pulsing &= ~BIT(ch);
		valves &= ~BIT(ch);
		return update_outputs(ch);
	}

	/*
	 * A continuity measurement has the low side on: let it end (up to
	 * some 0.55 s) before the channel counts as armed and may fire.
	 */
	k_mutex_lock(&cont_lock, K_FOREVER);
	armed[ch] = true;
	ret = update_outputs(ch);
	k_mutex_unlock(&cont_lock);
	return ret;
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
			(void)update_outputs(ch);
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
			(void)update_outputs(ch);
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
			(void)update_outputs(ch);
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

	ret = channels_pulse(BIT(ch), ms);
	if (ret) {
		return ret;
	}

	/* Blocking, for the shell: until the pulse timer has switched it off. */
	while (channels_pulsing() & BIT(ch)) {
		k_msleep(10);
	}
	return 0;
}

int channels_continuity(int ch, int32_t *mv)
{
	if (ch < 0 || ch >= SAT_CHANNELS) {
		return -EINVAL;
	}

	return cont_read(ch, mv);
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
