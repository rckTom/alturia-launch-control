# SPDX-License-Identifier: GPL-3.0-only

# With MCUboot in front (the board's Kconfig.sysbuild picks it), the app
# gets mcumgr on its USB shell for the browser tool (tools/webtool). Any
# -D<app>_EXTRA_CONF_FILE given on the command line is kept.
#
# The signed image carries the board name in custom TLV 0xa0, inside the
# signed (protected) area, so the browser tool can refuse a controller
# image for a satellite and the other way round.
if(SB_CONFIG_BOOTLOADER_MCUBOOT)
  sysbuild_cache_set(VAR ${DEFAULT_IMAGE}_EXTRA_CONF_FILE APPEND REMOVE_DUPLICATES
                     ${CMAKE_CURRENT_LIST_DIR}/../common/mgmt/mcumgr.conf)
  set_config_string(${DEFAULT_IMAGE} CONFIG_MCUBOOT_EXTRA_IMGTOOL_ARGS
                    "--custom-tlv 0xa0 ${SB_CONFIG_BOARD}")
endif()
