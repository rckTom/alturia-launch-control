/* SPDX-License-Identifier: GPL-3.0-only */

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/sys/reboot.h>

#include "dfu_guard.h"

LOG_MODULE_REGISTER(dfu, LOG_LEVEL_INF);

/*
 * Image upload chunks and os reset requests: refused with EBUSY while the
 * app has something armed. On the satellite a reset or the flash stalls
 * of an upload would drop the arm relay anyway, but nothing should change
 * under an operator who is about to launch.
 */
static enum mgmt_cb_return guard_cb(uint32_t event, enum mgmt_cb_return prev_status, int32_t *rc,
				    uint16_t *group, bool *abort_more, void *data, size_t data_size)
{
	ARG_UNUSED(prev_status);
	ARG_UNUSED(group);
	ARG_UNUSED(data);
	ARG_UNUSED(data_size);

	if (app_dfu_allowed()) {
		return MGMT_CB_OK;
	}

	LOG_WRN("%s refused: armed", event == MGMT_EVT_OP_OS_MGMT_RESET ? "reset" : "upload");
	*rc = MGMT_ERR_EBUSY;
	*abort_more = true;
	return MGMT_CB_ERROR_RC;
}

static struct mgmt_callback upload_guard = {
	.callback = guard_cb,
	.event_id = MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK,
};

static struct mgmt_callback reset_guard = {
	.callback = guard_cb,
	.event_id = MGMT_EVT_OP_OS_MGMT_RESET,
};

static int dfu_guard_init(void)
{
	mgmt_callback_register(&upload_guard);
	mgmt_callback_register(&reset_guard);
	return 0;
}

SYS_INIT(dfu_guard_init, APPLICATION, 0);

#ifdef CONFIG_BOOTLOADER_MCUBOOT
void dfu_image_ok(void)
{
	if (boot_is_img_confirmed()) {
		return;
	}

	int ret = boot_write_img_confirmed();

	if (ret) {
		LOG_ERR("confirming the image: %d", ret);
	} else {
		LOG_INF("new firmware confirmed");
	}
}

void dfu_image_bad(void)
{
	if (boot_is_img_confirmed()) {
		return;
	}

	LOG_ERR("new firmware failed to start, back to the previous one");
	k_sleep(K_MSEC(500));
	sys_reboot(SYS_REBOOT_COLD);
}
#endif
