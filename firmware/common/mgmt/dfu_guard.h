/* SPDX-License-Identifier: GPL-3.0-only */

/*
 * Firmware updates over mcumgr (common/mgmt/mcumgr.conf), as seen by the
 * app: an upload or a reset only goes through while the app says so, and
 * a freshly swapped-in image stays on test until the app vouches for it.
 */

#ifndef DFU_GUARD_H
#define DFU_GUARD_H

#include <stdbool.h>

/*
 * Provided by the app: whether an upload or a reset over mcumgr is fine
 * right now (nothing armed). Called from the shell thread.
 */
bool app_dfu_allowed(void);

#ifdef CONFIG_BOOTLOADER_MCUBOOT
/*
 * The app is up and working: make the running image permanent. Until
 * then MCUboot treats an image it has just swapped in as a test, and the
 * next reset brings the previous one back.
 */
void dfu_image_ok(void);
/* Start-up failed: on a test image, reset so MCUboot reverts to the old one. */
void dfu_image_bad(void);
#else
static inline void dfu_image_ok(void)
{
}
static inline void dfu_image_bad(void)
{
}
#endif

#endif /* DFU_GUARD_H */
