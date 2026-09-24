# LoRa bench test

Ping/pong between two Heltec WiFi LoRa 32 V4 boards, to bring up the LoRa
backend in [`../common/lora/`](../common/lora/) before the satellite
hardware exists, and to run the secure link between a controller and a
satellite over the air (see [below](#secure-link-one-controller-one-satellite)).

Both boards run the same image. A board listens and answers every PING
with a PONG that carries the RSSI/SNR it heard the PING at. The pinging
board logs the round-trip time and the signal in both directions.

## Building and flashing

Zephyr has no V4 board, so this builds for the V3 and
[`boards/heltec_wifi_lora32_v3_esp32s3_procpu.overlay`](boards/heltec_wifi_lora32_v3_esp32s3_procpu.overlay)
adds what the V4 changes: the console on the native USB port, and the
RF front-end's control pins.

```sh
source ~/Tools/zephyrproject/venv/bin/activate
export ZEPHYR_BASE=~/Tools/zephyrproject/zephyr
west build -b heltec_wifi_lora32_v3/esp32s3/procpu -p always .
west flash --esp-device /dev/ttyACMx
```

The console and shell are on the same USB-C port, at any baud rate.

## Using it

Press **PRG** on one board to send 10 pings, 2 s apart; press it again to
stop. Or from the shell:

| Command | |
|---|---|
| `bench info` | Node id, frequency, SF, TX power, airtime |
| `bench ping [count] [interval_ms]` | Start a ping run (default 10, 2000 ms) |
| `bench stop` | End the run |
| `bench power [dBm]` | Show or set TX power at the antenna, up to 14 dBm |

A successful run looks like:

```
<inf> bench: ping 3: pong from 079c in 342 ms, there -41 dBm/10 dB, back -43 dBm/9 dB
```

"There" is the PING as heard by the other board, "back" is its PONG as
heard here. The white LED flashes on every transmission.

The same is on the OLED, so the boards can be tested away from a laptop:

```
079c  KCT8103L (V4.3)      node id, detected front-end
869.525 SF9 +10dBm         radio settings
RX ping #12 from 3a41      last packet heard
  -45dBm 9dB  3s ago
PING 7/10  ok 6            ping run: sent/total, answered
  there -41dBm  9dB        last answered ping, both directions
  back  -43dBm 10dB
  rtt 342ms
```

If bring-up fails, the display shows which step and the error code. The
5x8 font is converted from X11's public-domain misc-fixed `5x8.bdf` by
[`tools/bdf_to_cfb.py`](tools/bdf_to_cfb.py); the built-in CFB fonts are
too large for more than four lines.

Keep the boards a metre or so apart. At short range the V4's LNA can push
the SX1262 input into compression, which shows up as lost packets with
good RSSI.

## Secure link: one controller, one satellite

The same image can also run either end of the real link, the code the
controller and satellite firmware use
([`../common/link/`](../common/link/), spec in
[`design/radio_security.md`](../../design/radio_security.md)). Give each
board a role once; it is kept in flash and the board reboots into it:

```
bench role ctrl      # on one board
bench role sat       # on the other; "bench role off" goes back to ping/pong
```

Changing the role erases the pairing. Then:

1. **Pair.** Hold **PRG** for a second on the satellite (pairing mode, 120 s),
   then on the controller (it pairs with the first satellite it hears).
   Both OLEDs show four rows of six boxes. If they are the same, press
   **PRG** on each board; if not, hold it. The log has the pattern too.
   Both switch to SF7 at low power while pairing.
2. **Join.** Automatic, and again after either board reboots. The status
   line reads `link: joined`.
3. **Ping.** Press **PRG** on the controller, or `bench ping`. Odd pings go
   to the satellite alone, even ones as group broadcasts; the satellite
   answers each with a sealed pong.

| Command | |
|---|---|
| `bench role [off\|ctrl\|sat]` | Show or set the role (reboots) |
| `bench pair [uid]` | Satellite: pairing mode. Controller: pair with `uid`, or the first one heard |
| `bench scan [on\|off]` | Controller: log satellites in pairing mode |
| `bench confirm` / `bench reject` | The patterns match / differ (or cancel) |
| `bench unpair` | Forget the pairing |
| `bench replay` | Resend the last sealed DATA frame byte for byte, as an attacker would |
| `bench link` | Link state and why received frames were dropped |
| `bench ping`, `bench stop` | Sealed ping run |

Things worth trying: `bench replay` on the controller must count as
`replay` on the satellite and not answer; `bench unpair` on the satellite
must turn the controller's pings into drops there; a third board in
pairing mode shows a different pattern and must be rejected.

## Radio settings

Set in [`../common/lora/radio.c`](../common/lora/radio.c): 869.525 MHz,
SF9, 125 kHz, CR 4/5, 10 dBm at the antenna. 869.525 MHz is in the EU
869.40–869.65 MHz sub-band (500 mW ERP, 10 % duty cycle); ping runs are
slowed down if needed to stay under the 10 %. No channel plan for the
real link has been chosen yet.

## The V4's RF front-end

The V4 puts a PA/LNA between the SX1262 and the antenna, and the part
depends on the board revision:

| Revision | Front-end | Pin driven high during TX | Net TX gain |
|---|---|---|---|
| V4.2 | GC1109 | IO46 (CPS, PA on) | ~11 dB |
| V4.3 | KCT8103L | IO5 (CTX) | ~13 dB |

On both, IO7 powers the front-end and IO2 enables it; DIO2 on the SX1262
switches the other TX/RX line itself. The firmware tells the revisions
apart at boot by the pull on IO2 and logs which one it found.
`radio_set_tx_power()` takes the power at the antenna and subtracts the
front-end gain. The gains are Meshtastic's measurements and hold only
below about 15 dBm into the PA.
