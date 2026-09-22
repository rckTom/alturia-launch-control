# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(launch_controller_ui)

# Headers LVGL itself includes (1-bit blend fix, see prj.conf)
zephyr_include_directories(src/lvgl)

target_sources(app PRIVATE
	src/main.c
	src/controller.c
	src/pads.c
	src/ui.c
	src/inputs.c
	src/shell_cmds.c
)

# The satellite simulator stands in for the LoRa link. A hardware build
# would replace it with a real link implementation behind the same link.h.
target_sources_ifdef(CONFIG_APP_SAT_SIM app PRIVATE src/sat_sim.c)
