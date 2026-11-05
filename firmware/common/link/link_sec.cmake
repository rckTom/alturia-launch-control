# SPDX-License-Identifier: GPL-3.0-only
#
# Link security (pairing, sessions, group key) and channel access timing. Include from an app's
# CMakeLists.txt after find_package(Zephyr). Needs CONFIG_PSA_CRYPTO and the
# PSA_WANT set listed in documentation/radio_link.md; lsec_store.c also needs
# CONFIG_SETTINGS.
#
# Set LSEC_LINK to "sat" and/or "ctrl" before including to also build that
# end's link glue (sat_link.c / ctrl_link.c), and LSEC_RADIO to ON to build
# the LoRa transport for it (../lora/radio.c, radio_transport.c). On
# native_sim (CONFIG_RADIO_SIM) that is ../lora/radio_sim.c instead, and
# LSEC_RADIO_SIM is set for the app's own simulator glue.

set(LSEC_DIR ${CMAKE_CURRENT_LIST_DIR})

target_include_directories(app PRIVATE ${LSEC_DIR})

target_sources(app PRIVATE
	${LSEC_DIR}/lsec_crypto.c
	${LSEC_DIR}/lsec_frame.c
	${LSEC_DIR}/lsec_pair.c
	${LSEC_DIR}/lsec_ctrl.c
	${LSEC_DIR}/lsec_sat.c
	${LSEC_DIR}/link_sched.c
)

if(CONFIG_SETTINGS)
	target_sources(app PRIVATE ${LSEC_DIR}/lsec_store.c)
endif()

foreach(end ${LSEC_LINK})
	target_sources(app PRIVATE ${LSEC_DIR}/${end}_link.c)
endforeach()
if(LSEC_LINK)
	target_sources(app PRIVATE ${LSEC_DIR}/link_access.c)
endif()

if(LSEC_RADIO)
	target_include_directories(app PRIVATE ${LSEC_DIR}/../lora)
	target_sources(app PRIVATE ${LSEC_DIR}/radio_transport.c)
	if(CONFIG_RADIO_SIM)
		# native_sim: the radio network simulator stands in for the air,
		# see tools/radiosim. The bottom half runs against the host libc.
		set(LSEC_RADIO_SIM ON)
		target_sources(app PRIVATE ${LSEC_DIR}/../lora/radio_sim.c)
		target_sources(native_simulator INTERFACE ${LSEC_DIR}/../lora/radio_sim_bottom.c)
		if(LSEC_LINK STREQUAL "ctrl")
			target_compile_definitions(app PRIVATE RADIO_SIM_ROLE=RSIM_ROLE_CTRL)
		elseif(LSEC_LINK STREQUAL "sat")
			target_compile_definitions(app PRIVATE RADIO_SIM_ROLE=RSIM_ROLE_SAT)
		endif()
	else()
		target_sources(app PRIVATE ${LSEC_DIR}/../lora/radio.c)
	endif()
endif()
