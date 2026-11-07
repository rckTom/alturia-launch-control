# SPDX-License-Identifier: GPL-3.0-only

# With MCUboot in front (the board's Kconfig.sysbuild picks it), the app
# gets mcumgr on its USB shell for the browser tool (tools/webtool). Any
# -D<app>_EXTRA_CONF_FILE given on the command line is kept.
if(SB_CONFIG_BOOTLOADER_MCUBOOT)
  sysbuild_cache_set(VAR ${DEFAULT_IMAGE}_EXTRA_CONF_FILE APPEND REMOVE_DUPLICATES
                     ${CMAKE_CURRENT_LIST_DIR}/../common/mgmt/mcumgr.conf)
endif()
