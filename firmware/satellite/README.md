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
| SX1262 (RA-01SH) | SPI1 (shield) | LoRa link to the console |
| ADS124S08 | SPI2 | 24-bit continuity measurement |
| MCP3208 | SPI2 | 12-bit rail dividers |
| INA226 | I2C1 | Fire-rail current across the 2 mΩ shunt |
| USB FS | — | Key provisioning over CDC ACM |

```sh
source ~/Tools/zephyrproject/venv/bin/activate
west build -b nucleo_g474re --shield semtech_sx1262mb2das -p always .
```

## Footprint

The point of the skeleton. With shell, logging, USB CDC, ZMS-backed settings,
PSA crypto (AES-GCM, HMAC-SHA256, ECDSA P-256, ECDH) and every external
driver enabled:

| | Used | Region | |
|---|---|---|---|
| FLASH | 164 KB | 512 KB | 31% |
| RAM | 45 KB | 128 KB | 35% |

That is the stack floor, not the finished firmware — the application logic on
top is small by comparison.
