# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(launch_satellite)

target_sources(app PRIVATE
	src/main.c
	src/pair_ui.c
	src/channels.c
	src/rail.c
	src/shell_cmds.c
)

# The satellite's end of the secure LoRa link, shared with the bench.
set(LSEC_LINK sat)
set(LSEC_RADIO ON)
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/link/link_sec.cmake)
