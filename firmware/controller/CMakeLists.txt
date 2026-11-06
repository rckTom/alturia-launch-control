# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.20.0)

# The controller board itself: firmware/boards/others/alturia_controller.
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(launch_controller)

# Headers LVGL itself includes (1-bit blend fix, see prj.conf)
zephyr_include_directories(src/lvgl)

target_sources(app PRIVATE
	src/main.c
	src/controller.c
	src/pads.c
	src/game.c
	src/ui.c
	src/lvgl/i1_blend_fix.c
	src/ui/widgets.c
	src/ui/list.c
	src/ui/armed.c
	src/ui/launch.c
	src/ui/menu.c
	src/ui/pads.c
	src/ui/sats.c
	src/ui/diag.c
	src/ui/game.c
	src/ui/dialogs.c
	src/inputs.c
	src/power.c
	src/battery.c
	src/shell_cmds.c
	src/link_lsec.c
	src/fonts/ui_font_18.c
	src/fonts/ui_font_20.c
	src/fonts/ui_font_28.c
	src/logo/ui_logo.c
)
# The views in src/ui include the app's headers from src
target_include_directories(app PRIVATE src)

# The controller's end of the secure link (../common/link). The satellite
# simulator stands in for the LoRa radio and the satellites; without it the
# link runs over the radio.
set(LSEC_LINK ctrl)
if(NOT CONFIG_APP_SAT_SIM)
	set(LSEC_RADIO ON)
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/link/link_sec.cmake)

# Command and status payloads, shared with the satellite.
target_include_directories(app PRIVATE ../common/wire)
target_sources(app PRIVATE ../common/wire/sat_wire.c)

# Buzzer tunes, shared with the satellite.
target_include_directories(app PRIVATE ../common/chime)
target_sources(app PRIVATE ../common/chime/chime.c)

target_sources_ifdef(CONFIG_APP_SAT_SIM app PRIVATE src/sat_sim.c)
target_sources_ifdef(CONFIG_PM app PRIVATE src/sleep.c)

# A node of the radio network simulator (radiosim.conf, radiosim.overlay):
# the screen and the controls live in the simulator's web UI.
if(LSEC_RADIO_SIM)
	target_sources(app PRIVATE src/sim_display.c src/sim_panel.c)
endif()
