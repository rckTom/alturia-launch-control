# Satellite firmware

Zephyr application for the pad-side satellite board. The operator console
lives in [`../controller_ui/`](../controller_ui/).

## Status

Skeleton. Every subsystem the satellite needs is wired up and calling its
driver API, but the command protocol, the arm state machine and the real
continuity maths are not written. It exists to answer one question — does a
full-featured build fit the candidate MCU — and to be the starting point for
the real firmware.

## Building

There is no board definition for the satellite yet, so it builds against a
Nucleo-G474RE carrying the Semtech SX1262 shield. The overlay in
[`boards/nucleo_g474re.overlay`](boards/nucleo_g474re.overlay) adds the
external devices the real board has, with stand-in pin assignments, so their
drivers are compiled and exercised:

| Device | Bus | Role |
|---|---|---|
| SX1262 (Wio-SX1262 on the board) | SPI1 (shield) | LoRa link to the console |
| ADS124S08 | SPI2 | 24-bit continuity measurement |
| MCP3208 | SPI2 | 12-bit rail dividers |
| INA226 | I2C1 | Fire-rail current across the 2 mΩ shunt |
| USB FS | — | Console over CDC ACM |

```sh
source ~/Tools/zephyrproject/venv/bin/activate
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
[`design/radio_security.md`](../../design/radio_security.md)). A new
satellite pairs with the console over the air: hold the user button (the
continuity button on the real board) for 5 s with every channel SAFE, compare
the pattern on the channel LEDs with the console's, and press the button
if they match. The Nucleo has no channel LEDs, so the pattern is also in the
log, and `sat pair confirm` / `sat pair reject` answer from the shell.
`sat link` shows the link state; `sat unpair` forgets the console.

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
