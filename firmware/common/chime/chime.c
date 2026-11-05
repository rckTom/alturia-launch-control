/* SPDX-License-Identifier: GPL-3.0-only */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/buzzer.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/policy.h>
#include <zephyr/sys/atomic.h>

#include "chime.h"

LOG_MODULE_REGISTER(chime, LOG_LEVEL_INF);

#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(buzzer))

static const struct device *const buzzer = DEVICE_DT_GET(DT_ALIAS(buzzer));

struct note {
	uint16_t hz;
	uint16_t ms;
};

/* A rising C major arpeggio, C7 E7 G7 then C8 held. It sits in the
 * 2-4 kHz band where the magnetic transducer is loudest.
 */
static const struct note startup[] = {
	{ 2093, 90 }, { 2637, 90 }, { 3136, 90 }, { 4186, 260 },
};

/* Falling, slower and lower: something did not come up, or a fault. */
static const struct note error[] = {
	{ 3136, 200 }, { 2349, 200 }, { 1568, 450 },
};

/* Up a fourth: armed. Down again: disarmed. */
static const struct note armed[] = {
	{ 2637, 70 }, { 3520, 150 },
};
static const struct note disarm[] = {
	{ 3520, 70 }, { 2637, 150 },
};

/* A click every couple of seconds while armed */
static const struct note armed_tick[] = {
	{ 3136, 30 },
};

/* Two quick and a long high one: the satellites confirmed the launch */
static const struct note launch[] = {
	{ 4186, 60 }, { 4186, 60 }, { 4186, 250 },
};

/* One short low buzz: that button does nothing now */
static const struct note refused[] = {
	{ 1568, 100 },
};

#define NOTE_GAP_MS 25

/* The tune playing; a new one replaces it */
static struct k_spinlock lock;
static const struct note *tune;
static size_t tune_len;
static size_t tune_pos;
/* A tune holds off low-power states, which would stop the PWM's clock */
static atomic_t playing;

static void chime_step(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(chime_work, chime_step);

static void chime_step(struct k_work *work)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct note n;

	if (tune_pos >= tune_len) {
		k_spin_unlock(&lock, key);
		if (atomic_cas(&playing, 1, 0)) {
			pm_policy_state_all_lock_put();
		}
		return;
	}
	n = tune[tune_pos++];
	k_spin_unlock(&lock, key);

	(void)buzzer_tone(buzzer, n.hz, n.ms);
	k_work_reschedule(&chime_work, K_MSEC(n.ms + NOTE_GAP_MS));
}

static void chime_play(const struct note *notes, size_t len)
{
	if (!device_is_ready(buzzer)) {
		LOG_WRN("buzzer not ready");
		return;
	}

	(void)buzzer_set_volume(buzzer, BUZZER_VOLUME_MAX);
	if (atomic_cas(&playing, 0, 1)) {
		pm_policy_state_all_lock_get();
	}
	K_SPINLOCK(&lock) {
		tune = notes;
		tune_len = len;
		tune_pos = 0;
	}
	k_work_reschedule(&chime_work, K_NO_WAIT);
}

void chime_startup(void)
{
	chime_play(startup, ARRAY_SIZE(startup));
}

void chime_error(void)
{
	chime_play(error, ARRAY_SIZE(error));
}

#define DEFINE_CHIME(name)                              \
	void chime_##name(void)                         \
	{                                               \
		chime_play(name, ARRAY_SIZE(name));     \
	}

#else

void chime_startup(void)
{
}

void chime_error(void)
{
}

#define DEFINE_CHIME(name)        \
	void chime_##name(void)   \
	{                         \
	}

#endif

DEFINE_CHIME(armed)
DEFINE_CHIME(disarm)
DEFINE_CHIME(armed_tick)
DEFINE_CHIME(launch)
DEFINE_CHIME(refused)
