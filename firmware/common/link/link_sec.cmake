# SPDX-License-Identifier: Apache-2.0
#
# Link security (pairing, sessions, group key) and channel access timing. Include from an app's
# CMakeLists.txt after find_package(Zephyr). Needs CONFIG_PSA_CRYPTO and the
# PSA_WANT set listed in design/radio_security.md; lsec_store.c also needs
# CONFIG_SETTINGS.
#
# Set LSEC_LINK to "sat" and/or "ctrl" before including to also build that
# end's link glue (sat_link.c / ctrl_link.c), and LSEC_RADIO to ON to build
# the LoRa transport for it (../lora/radio.c, radio_transport.c).

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
	target_sources(app PRIVATE
		${LSEC_DIR}/../lora/radio.c
		${LSEC_DIR}/radio_transport.c
	)
endif()
