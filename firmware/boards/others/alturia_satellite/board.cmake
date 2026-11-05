# SPDX-License-Identifier: GPL-3.0-only

# SWD on J3, or the ROM bootloader's USB DFU: hold BOOT0 (SW1) through a
# reset, then `west flash -r dfu-util`.
board_runner_args(stm32cubeprogrammer "--port=swd" "--reset-mode=hw")
board_runner_args(pyocd "--target=stm32g474retx")
board_runner_args(jlink "--device=STM32G474RE" "--speed=4000")
board_runner_args(dfu-util "--pid=0483:df11" "--alt=0" "--dfuse")

# keep first
include(${ZEPHYR_BASE}/boards/common/stm32cubeprogrammer.board.cmake)
include(${ZEPHYR_BASE}/boards/common/openocd-stm32.board.cmake)
include(${ZEPHYR_BASE}/boards/common/pyocd.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
include(${ZEPHYR_BASE}/boards/common/dfu-util.board.cmake)
