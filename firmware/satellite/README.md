# Satellite firmware

Zephyr application for the pad-side satellite board. The operator console
lives in [`../controller/`](../controller/).

## Status

Skeleton. Every subsystem the satellite needs is wired up and calling its
driver API, but the command protocol, the arm state machine and the real
continuity maths are not written. It exists to answer one question — does a
full-featured build fit the candidate MCU — and to be the starting point for
the real firmware.

## Building

The satellite board itself is `alturia_satellite`, defined out of tree in
[`../boards/others/alturia_satellite/`](../boards/others/alturia_satellite/)
(the app's `CMakeLists.txt` adds `firmware/` to `BOARD_ROOT`). Its
devicetree carries every device on the board with the pin assignments from
the schematic netlist; channel n there is the one labelled n on the front
panel, which is PyroStage(7 − n):

| Device | Bus | Role |
|---|---|---|
| Wio-SX1262 | SPI1 | LoRa link to the console |
| ADS124S08 | SPI2 | 24-bit continuity measurement, REFP0 = V_TEST |
| MCP3208 | SPI2 | SENSE_I per channel, V_PYRO_SENSE (CH6), VBAT_SENSE (CH7) |
| Buzzer | TIM4 CH4 | Passive transducer, PWM tone |
| USB FS | — | Console and shell over CDC ACM |
| LPUART1 | J2 | Spare UART header |

```sh
source ~/zephyrproject/venv/bin/activate
west build -b alturia_satellite -d build-board -p always .
west flash -d build-board                # ST-LINK on J3
west flash -d build-board -r dfu-util    # or USB DFU: hold BOOT0 (SW1) through a reset
```

The app-side settings for the board (LoRa, GPIO and ADC shells) are in
[`boards/alturia_satellite.conf`](boards/alturia_satellite.conf). For
bring-up, `gpio info`, `gpio get gpioc 15`, `gpio set gpiob 3 1` and
`adc ads124s08@0 read 0` work from the USB shell.

There is no HSE crystal, so USB runs from HSI48; the board's `board.c`
turns on CRS so HSI48 is trimmed against USB SOF.

### Nucleo bench

It also builds against a Nucleo-G474RE carrying the Semtech SX1262 shield.
The overlay in [`boards/nucleo_g474re.overlay`](boards/nucleo_g474re.overlay)
adds the same external devices with stand-in pin assignments, so their
drivers are compiled and exercised:

```sh
west build -b nucleo_g474re --shield semtech_sx1262mb2das -p always .
```

The hardware-only settings (LoRa module, USB console) are in
[`boards/nucleo_g474re.conf`](boards/nucleo_g474re.conf).

It also builds for `native_sim/native/64`, as a node of the radio network
simulator ([`tools/radiosim`](../../tools/radiosim/README.md)). There the
radio is `common/lora/radio_sim.c`. The outputs, LEDs and button are
emulated GPIOs that the simulator's web UI shows and presses
([`boards/native_sim.overlay`](boards/native_sim.overlay)):

```sh
west build -b native_sim/native/64 -d build-sim .
```

## Link

The LoRa link is the shared secure link in
[`../common/link/`](../common/link/) (spec:
[`documentation/radio_link.md`](../../documentation/radio_link.md)). A new
satellite pairs with the console over the air: press the user button (the
continuity button on the real board) three times within 1 s with every
channel SAFE, compare
the pattern on the channel LEDs with the console's, and press the button
if they match. The Nucleo has no channel LEDs, so the pattern is also in the
log, and `sat pair confirm` / `sat pair reject` answer from the shell.
`sat link` shows the link state; `sat unpair` forgets the console.

A single press of the continuity button shows continuity on the channel
LEDs for 5 s: lit for an output that reads OK, fast blinking when open,
slow blinking for a short or a failed measurement. The status LED blinks every 2 s while
the satellite is on and stays lit while it has a session with the console.

## Footprint

The point of the skeleton. With shell, logging, USB CDC, ZMS-backed settings,
the secure link (PSA: AES-CCM, HMAC/HKDF-SHA256, ECDH P-256, and the link
thread) and every external driver enabled:

| | Used | Region | |
|---|---|---|---|
| FLASH | 179 KB | 512 KB | 34% |
| RAM | 56 KB | 128 KB | 43% |

That is the stack floor, not the finished firmware — the application logic on
top is small by comparison.
