# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lora_bench)

set(COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)

target_include_directories(app PRIVATE ${COMMON_DIR}/lora)

target_sources(app PRIVATE
	src/main.c
	src/heltec_v4_fem.c
	src/oled.c
	src/font_5x8.c
	src/shell_cmds.c
	src/secure.c
	src/trace.c
)

# Either end of the real link, picked at run time by "bench role", plus
# the replay hook for testing.
set(LSEC_LINK sat ctrl)
set(LSEC_RADIO ON)
include(${COMMON_DIR}/link/link_sec.cmake)
target_compile_definitions(app PRIVATE LINK_TEST_HOOKS)
