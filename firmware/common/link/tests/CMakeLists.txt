# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lsec_tests)

include(${CMAKE_CURRENT_SOURCE_DIR}/../link_sec.cmake)

target_sources(app PRIVATE src/main.c src/sched.c)

# The command and status payloads that travel inside the link's frames.
target_sources(app PRIVATE src/wire.c ${CMAKE_CURRENT_SOURCE_DIR}/../../wire/sat_wire.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../wire)
