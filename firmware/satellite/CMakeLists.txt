# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.20.0)

# The satellite board itself: firmware/boards/others/alturia_satellite.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(launch_satellite)

target_sources(app PRIVATE
	src/main.c
	src/commands.c
	src/pair_ui.c
	src/channels.c
	src/rail.c
	src/shell_cmds.c
	src/arm.c
	src/fault.c
	src/ignlog.c
)

# The satellite's end of the secure LoRa link, shared with the bench.
set(LSEC_LINK sat)
set(LSEC_RADIO ON)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/link/link_sec.cmake)

# Command and status payloads, shared with the controller.
target_include_directories(app PRIVATE ../common/wire)
target_sources(app PRIVATE ../common/wire/sat_wire.c)

# Ignition log records, and their mcumgr group, shared with the controller.
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/ignlog/ignlog.cmake)

# Buzzer tunes, shared with the controller.
target_include_directories(app PRIVATE ../common/chime)
target_sources(app PRIVATE ../common/chime/chime.c)

# native_sim: LEDs and pairing button in the radio network simulator's UI.
if(LSEC_RADIO_SIM)
	target_sources(app PRIVATE src/sim_io.c)
endif()

# Firmware updates: refuse while armed, confirm a new image once up.
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/mgmt/dfu_guard.cmake)
