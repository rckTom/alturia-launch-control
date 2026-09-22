#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "controller.h"
#include "inputs.h"

LOG_MODULE_REGISTER(inputs, LOG_LEVEL_INF);

/* Levels, written from the input thread and read from the LVGL thread */
static atomic_t key_on;
static atomic_t fire_held;

/* Select (encoder push or keypad) held this long opens the label editor */
#define LONG_PRESS_MS 600

static atomic_t long_press_fired;

static void long_press(struct k_work *work)
{
	ARG_UNUSED(work);
	atomic_set(&long_press_fired, 1);
	ctrl_post(CTRL_REQ_EDIT, 0);
}

static K_WORK_DELAYABLE_DEFINE(long_press_work, long_press);

static void on_input(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	/* Rotary encoder: gpio-qdec on hardware, mouse wheel on native_sim */
	if (evt->type == INPUT_EV_REL && evt->code == INPUT_REL_WHEEL) {
		ctrl_post(CTRL_REQ_CURSOR, -evt->value);
		return;
	}
	if (evt->type != INPUT_EV_KEY) {
		return;
	}

	switch (evt->code) {
	case INPUT_KEY_K:
		/* The real part is a latching key switch; emulate the latch. */
		if (evt->value) {
			bool on = !atomic_get(&key_on);

			atomic_set(&key_on, on);
			LOG_INF("controller key %s", on ? "ON" : "OFF");
		}
		break;
	case INPUT_KEY_SPACE:
		atomic_set(&fire_held, evt->value != 0);
		break;
	case INPUT_KEY_A:
		if (evt->value) {
			ctrl_post(CTRL_REQ_ARM, 0);
		}
		break;
	case INPUT_KEY_ESC:
		if (evt->value) {
			ctrl_post(CTRL_REQ_SAFE, 0);
		}
		break;
	case INPUT_KEY_F:
		if (evt->value) {
			ctrl_post(CTRL_REQ_FILL, 0);
		}
		break;
	case INPUT_KEY_V:
		if (evt->value) {
			ctrl_post(CTRL_REQ_VENT, 0);
		}
		break;
	case INPUT_KEY_UP:
	case INPUT_KEY_DOWN:
		if (evt->value) {
			ctrl_post(CTRL_REQ_CURSOR, evt->code == INPUT_KEY_UP ? -1 : 1);
		}
		break;
	case INPUT_KEY_ENTER:  /* keypad select */
	case INPUT_BTN_TOUCH:  /* native_sim: mouse click, stands in for the encoder push */
		if (evt->value) {
			atomic_clear(&long_press_fired);
			k_work_reschedule(&long_press_work, K_MSEC(LONG_PRESS_MS));
		} else {
			/* Released before the long press fired: a normal select */
			k_work_cancel_delayable(&long_press_work);
			if (!atomic_clear(&long_press_fired)) {
				ctrl_post(CTRL_REQ_SELECT, 0);
			}
		}
		break;
	default:
		break;
	}
}

/* All input devices: the button keys, the encoder and the keypad */
INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);

bool inputs_key_on(void)
{
	return atomic_get(&key_on) != 0;
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
