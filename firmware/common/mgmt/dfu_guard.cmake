# SPDX-License-Identifier: GPL-3.0-only

# The app's side of firmware updates (dfu_guard.h). The header is always
# there; the code only with mcumgr, i.e. in MCUboot builds.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_sources_ifdef(CONFIG_MCUMGR app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/dfu_guard.c)
