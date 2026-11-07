# SPDX-License-Identifier: GPL-3.0-only

# Ignition log records (ignlog_rec.h), used by both apps and the wire
# format; their mcumgr group only with mcumgr, i.e. in MCUboot builds. The
# app provides the backend, ignlog_mgmt.h.
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/ignlog_rec.c)
target_sources_ifdef(CONFIG_MCUMGR app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/ignlog_mgmt.c)
