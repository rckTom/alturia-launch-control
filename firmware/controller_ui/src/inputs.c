#include <string.h>

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>

#include "controller.h"
#include "inputs.h"

LOG_MODULE_REGISTER(inputs, LOG_LEVEL_INF);

/* Levels, written from the input thread and read from the LVGL thread */
static atomic_t key_on;
static atomic_t fire_held;

/* OK or BACK held this long is a long press */
#define LONG_PRESS_MS 600

static atomic_t ok_long_fired;

static void ok_long_press(struct k_work *work)
{
	ARG_UNUSED(work);
	atomic_set(&ok_long_fired, 1);
	ctrl_post(CTRL_REQ_EDIT, 0);
}

static K_WORK_DELAYABLE_DEFINE(ok_long_work, ok_long_press);

static void back_long_press(struct k_work *work)
{
	ARG_UNUSED(work);
	ctrl_post(CTRL_REQ_BACK_LONG, 0);
}

static K_WORK_DELAYABLE_DEFINE(back_long_work, back_long_press);

/* Up or down held: the cursor keeps moving, for long lists */
#define REPEAT_DELAY_MS  400
#define REPEAT_PERIOD_MS 120

static atomic_t repeat_dir;   /* -1 up, 1 down, 0 neither held */

static void repeat_step(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(repeat_work, repeat_step);

static void repeat_step(struct k_work *work)
{
	int dir = (int)atomic_get(&repeat_dir);

	ARG_UNUSED(work);
	if (dir) {
		ctrl_post(CTRL_REQ_CURSOR, dir);
		k_work_reschedule(&repeat_work, K_MSEC(REPEAT_PERIOD_MS));
	}
}

/* Key codes per button, as the board devicetree (or native_sim) gives them */
static const struct {
	uint16_t code;
	const char *name;
} buttons[BTN_COUNT] = {
	[BTN_ARM] = {INPUT_KEY_A, "ARM"},
	[BTN_FIRE] = {INPUT_KEY_SPACE, "FIRE"},
	[BTN_FILL] = {INPUT_KEY_F, "FILL"},
	[BTN_VENT] = {INPUT_KEY_V, "VENT"},
	[BTN_UP] = {INPUT_KEY_UP, "UP"},
	[BTN_DOWN] = {INPUT_KEY_DOWN, "DOWN"},
	[BTN_LEFT] = {INPUT_KEY_LEFT, "LEFT"},
	[BTN_RIGHT] = {INPUT_KEY_RIGHT, "RIGHT"},
	[BTN_OK] = {INPUT_KEY_ENTER, "OK"},
	[BTN_BACK] = {INPUT_KEY_BACK, "BACK"},
	[BTN_POWER] = {INPUT_KEY_POWER, "POWER"},
	[BTN_KEY] = {INPUT_KEY_K, "KEY"},
};

const char *button_name(enum button b)
{
	return b < BTN_COUNT ? buttons[b].name : "?";
}

/* Button test state, written from the input thread */
static struct k_spinlock diag_lock;
static struct inputs_diag diag;
static int64_t pressed_at[BTN_COUNT];

static int button_of(uint16_t code)
{
	for (int b = 0; b < BTN_COUNT; b++) {
		if (buttons[b].code == code) {
			return b;
		}
	}
	return -1;
}

static void record(int b, bool pressed)
{
	k_spinlock_key_t k = k_spin_lock(&diag_lock);
	int64_t now = k_uptime_get();
	struct button_event ev = {.button = b, .pressed = pressed, .time = now};

	if (pressed) {
		diag.pressed |= BIT(b);
		diag.count[b]++;
		pressed_at[b] = now;
	} else {
		diag.pressed &= ~BIT(b);
		ev.held_ms = (int32_t)(now - pressed_at[b]);
	}
	memmove(&diag.log[1], &diag.log[0], sizeof(diag.log[0]) * (INPUTS_LOG_LEN - 1));
	diag.log[0] = ev;
	diag.n_log = MIN(diag.n_log + 1, INPUTS_LOG_LEN);
	k_spin_unlock(&diag_lock, k);
}

void inputs_diag(struct inputs_diag *out)
{
	k_spinlock_key_t k = k_spin_lock(&diag_lock);

	*out = diag;
	k_spin_unlock(&diag_lock, k);
}

bool inputs_pressed(enum button b)
{
	k_spinlock_key_t k = k_spin_lock(&diag_lock);
	bool pressed = (diag.pressed & BIT(b)) != 0;

	k_spin_unlock(&diag_lock, k);
	return pressed;
}

static void on_ok(bool pressed)
{
	if (pressed) {
		atomic_clear(&ok_long_fired);
		k_work_reschedule(&ok_long_work, K_MSEC(LONG_PRESS_MS));
	} else {
		/* Released before the long press fired: a normal select */
		k_work_cancel_delayable(&ok_long_work);
		if (!atomic_clear(&ok_long_fired)) {
			ctrl_post(CTRL_REQ_SELECT, 0);
		}
	}
}

static void on_input(struct input_event *evt, void *user_data)
{
	int b;

	ARG_UNUSED(user_data);

	/* Mouse wheel and click on native_sim, as the D-pad's up/down and OK */
	if (evt->type == INPUT_EV_REL && evt->code == INPUT_REL_WHEEL) {
		ctrl_post(CTRL_REQ_CURSOR, -evt->value);
		return;
	}
	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	if (evt->code == INPUT_BTN_TOUCH) {
		on_ok(evt->value != 0);
		return;
	}

	b = button_of(evt->code);
	if (b < 0) {
		return;
	}
	record(b, evt->value != 0);

	switch (b) {
	case BTN_KEY:
		/* The real part is a latching key switch; emulate the latch. */
		if (evt->value) {
			bool on = !atomic_get(&key_on);

			atomic_set(&key_on, on);
			LOG_INF("controller key %s", on ? "ON" : "OFF");
		}
		break;
	case BTN_FIRE:
		atomic_set(&fire_held, evt->value != 0);
		break;
	case BTN_ARM:
		if (evt->value) {
			ctrl_post(CTRL_REQ_ARM, 0);
		}
		break;
	case BTN_BACK:
		/* On press, so that disarming never waits for a release */
		if (evt->value) {
			ctrl_post(CTRL_REQ_BACK, 0);
			k_work_reschedule(&back_long_work, K_MSEC(LONG_PRESS_MS));
		} else {
			k_work_cancel_delayable(&back_long_work);
		}
		break;
	case BTN_FILL:
		if (evt->value) {
			ctrl_post(CTRL_REQ_FILL, 0);
		}
		break;
	case BTN_VENT:
		if (evt->value) {
			ctrl_post(CTRL_REQ_VENT, 0);
		}
		break;
	case BTN_UP:
	case BTN_DOWN: {
		int dir = b == BTN_UP ? -1 : 1;

		if (evt->value) {
			ctrl_post(CTRL_REQ_CURSOR, dir);
			atomic_set(&repeat_dir, dir);
			k_work_reschedule(&repeat_work, K_MSEC(REPEAT_DELAY_MS));
		} else if (atomic_cas(&repeat_dir, dir, 0)) {
			k_work_cancel_delayable(&repeat_work);
		}
		break;
	}
	case BTN_LEFT:
	case BTN_RIGHT:
		if (evt->value) {
			ctrl_post(CTRL_REQ_ADJUST, b == BTN_LEFT ? -1 : 1);
		}
		break;
	case BTN_OK:
		on_ok(evt->value != 0);
		break;
	case BTN_POWER:
		if (evt->value) {
			ctrl_post(CTRL_REQ_POWER, 0);
		}
		break;
	default:
		break;
	}
}

/* All input devices: the buttons, and the mouse on native_sim */
INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);

bool inputs_key_on(void)
{
	/* Without a key switch, ARM alone arms */
	return !IS_ENABLED(CONFIG_APP_KEY_SWITCH) || atomic_get(&key_on) != 0;
}

bool inputs_fire_held(void)
{
	return atomic_get(&fire_held) != 0;
}

void inputs_set_key(bool on)
{
	atomic_set(&key_on, on);
}

void inputs_set_fire(bool held)
{
	atomic_set(&fire_held, held);
}
