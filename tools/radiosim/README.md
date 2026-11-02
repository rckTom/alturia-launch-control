# radiosim: radio network simulator

This runs several real controller and satellite firmware images under Zephyr's
`native_sim`, with no radio. It relays their LoRa frames over a simulated
channel and shows the whole network in a web UI. From the UI you can see:

- every node on a map
- a timeline of every frame
- who received each frame, and why other nodes did not
- channel utilization
- a shell for each node
- each controller's screen and controls
- the satellites' LEDs and pairing button

You can add jammers, foreign LoRa traffic and an attacker radio that injects
or replays captured frames.

```
zephyr.exe (controller_ui) ─┐  UDP 127.0.0.1:7700    ┌─ FastAPI + WebSockets ── browser
zephyr.exe (satellite) × N ─┼──── radio_sim_proto.h ── radiosim: medium, devices, stats,
                            └── pty per node ─────────── process manager, recorder
```

## Running it

Build the two firmware images once. radiosim looks for them in these build
directories:

```sh
source ~/Tools/zephyrproject/venv/bin/activate
cd firmware
west build -b native_sim/native/64 -d controller_ui/build-radiosim controller_ui -- \
    -DEXTRA_CONF_FILE=radiosim.conf -DEXTRA_DTC_OVERLAY_FILE=radiosim.overlay
west build -b native_sim/native/64 -d satellite/build-sim satellite
```

Build the web UI once, and again after changing it:

```sh
cd tools/radiosim/web && npm install && npm run build
```

Then, from `tools/radiosim`:

```sh
uv run radiosim                  # web UI on http://127.0.0.1:8077
uv run radiosim --help           # ports, work directory, image paths
uv run radiosim --scenario sim-run/scenario.json --autostart
```

Use **+ Controller** and **+ Satellite** to start instances. To pair a
satellite:

1. On the controller's screen (in the dock), hold OK to open the menu.
2. Choose *Satellites*, pick a slot, and press OK.
3. Select the satellite, hold its **Button** for 5 s, then choose it on the
   controller.
4. Compare the LEDs with the controller's rows, and confirm on both.

`sat pair` and `sat pair confirm` in the satellite's shell do the same.

For UI work, `npm run dev` in `web/` serves the page with hot reload and
forwards `/api` and `/ws` to a running radiosim.

The work directory (default `sim-run/`) holds:

- one flash image per node, so pairings survive a restart
- `scenario.json` from **Save**
- a `record-*.jsonl` of every frame and its outcome at each receiver

## How a node talks to it

`firmware/common/lora/radio_sim.c` replaces `radio.c` on `native_sim`
(`CONFIG_RADIO_SIM`, selected in `common/link/link_sec.cmake`). The link layer
above it is unchanged:

- `radio_send()` sends a TX message and sleeps for the airtime.
- Received frames carry the uptime at which they ended.
- `radio_channel_clear()` samples the energy that the simulator reports on air
  at this node. It uses the same noise-floor tracking as the SX126x code.

Every time in the protocol is host `CLOCK_MONOTONIC`, which all processes
share. A node converts a host time to its own uptime at the moment it uses it,
so socket and scheduling latency do not move reply slots.

native_sim only pulls its uptime back to the host clock once per *rt period*.
The default of 10 ms let the uptime run up to 10 ms ahead, and satellites
missed their slots. `radio_sim` sets the period to 500 µs; `--rt-period`
overrides it. The shell command `rsim` shows how closely the uptime follows
the host clock, and how late received frames arrived after they ended on
air (a reply slot starts 6 ms after the poll, so anything over 3 ms late
shows up as a missed slot).

The simulator delivers frames from its event loop, so anything that blocks
the loop delays them. `/api/state` is encoded with `json.dumps` (FastAPI's
encoder took 200 ms for 2000 frames) and takes `?frames_since=<us>` to
return only recent frames; scripts should ask for no more than they need.

Command line of a node (the simulator passes these itself):

| Option | |
|---|---|
| `--radio_sim=host:port` | simulator address (default `127.0.0.1:7700`) |
| `--radio_name=name` | the node's name in the simulator |
| `--radio_role=ctrl\|sat` | role; defaults from the app |
| `--device_id=n` | hwinfo id, which becomes the satellite's uid |
| `--seed=n` | entropy seed. Without it every instance draws the same "random" keys |
| `--flash=file` | settings storage (the pairing) |
| `--uart_stdinout` | shell on stdin/stdout |

The protocol is `common/lora/radio_sim_proto.h`, mirrored by `radiosim/proto.py`.
The tests check that the two agree.

Glue for the web UI:

- **Controller** (`radiosim.overlay`): the SDL display is replaced by
  `src/sim_display.c`, which sends the 1-bit framebuffer. `src/sim_panel.c`
  puts the browser's keys on the same `gpio0` pins that the SDL keyboard
  drives.
- **Satellite** (`boards/native_sim.overlay`): the pyro outputs, LEDs and
  pairing button are emulated GPIOs, which `src/sim_io.c` mirrors to the
  simulator. The ADCs are emulators, and `rail.c` reads a quiet rail because
  there is no INA226.

## The channel model

`radiosim/medium.py`, with parameters in the UI when nothing is selected:

- **Path loss**: log-distance, 31.2 dB at 1 m (free space at 869.85 MHz)
  plus 10·n dB per decade. The default n = 3.
- **Per-link overrides**: extra attenuation, random loss, or cut.
- **Noise floor**: −174 dBm/Hz + 10·log10(250 kHz) + 6 dB NF = −114 dBm.
- **Demodulation**: needs SNR ≥ −7.5 dB at SF7 and ≥ −10 dB at SF8.
- **Collisions**: interference is summed over the frame's overlap.
  - A same-SF frame survives when it is at least 6 dB above the interference
    (capture).
  - A different-SF interferer destroys a frame only when it is 16 dB stronger.
  - Wideband jammer energy adds to the noise.
- **Half duplex**: a node that transmits during a frame does not hear it.
- **Profiles**: a receiver on the other profile's SF (link SF7, pairing SF8)
  does not decode the frame.

The outcome per receiver is one of: `ok`, `below_sens`, `collision`, `jammed`,
`half_duplex`, `wrong_sf`, `random_loss`, `cut`, `offline`.

Utilization is the union of LoRa on-air time over 1, 10 and 60 s, with
jammer occupancy reported separately. The *replies per poll* figure per slot
counts sealed frames of the poll's network (`net_tag`) that arrive in the
reply window after each POLL.

## Measurements

`scripts/coexist.py` starts its own radiosim on separate ports, so a running
instance is not disturbed. It builds two networks next to each other and
times how long a satellite of the second one takes to join after a
restart. It also reports both networks' replies per poll. With `--commands`
it times ARM and FIRE acknowledgement, and with `--loss` it runs them under
random loss so that commands have to be repeated:

```sh
uv run python scripts/coexist.py --runs 5
uv run python scripts/coexist.py --ctrl-exe old/ctrl.exe --sat-exe old/sat.exe   # a baseline
uv run python scripts/coexist.py --commands --loss 30
```

With `--nets 2..4` it builds that many networks instead, `--sats` (1 or 2)
satellites each, with every satellite out on the far side of its
controller, hidden from the other networks. It reports per network the
polls its satellites decoded and the replies its controller decoded, with
the reasons for the losses, each controller's `ctrl link` view, and the
satellites' missed slots and `rsim` lateness. `--aligned` restarts every
controller at once first; `--commands` then arms and fires on network 1.

```sh
uv run python scripts/coexist.py --nets 4 --sats 2 --aligned --commands
```

The web UI's channel panel also lists joins per network and satellite slot
over the last minute: tries, successes, and how long the last one took.

## Tests

```sh
cd tools/radiosim && uv run pytest
```

The tests cover:

- the protocol against the C header (struct sizes and constants)
- airtime against the Semtech values
- the medium: capture, collision, other SF, half duplex, jamming, cut links
  and liveness
- utilization of a known poll cycle, and join counting
- header decoding
