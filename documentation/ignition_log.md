# Ignition log

The satellite records the current through each channel during its fire
pulses and keeps the last few records. The controller downloads them over
LoRa and shows them in a diagnostics view, and the browser tool
(`tools/webtool`) draws them from either device on USB. The records show
whether an igniter drew current, how much, and when it burnt through.

## Measurement

Each channel's high-side switch, a TPS1HB08-Q1, mirrors its load current
out of its SNS pin: 1/5000 of it (K_SNS). The SNS pin drives 560 Ω to
ground. The MCP3208 reads the voltage across it: SENSE_I_P*n* on input
CH(*n* − 1), so firmware channel *ch* (panel *ch* + 1) is on CH(5 − *ch*).

| Quantity | Value |
|---|---|
| Load current per ADC code | 3.3 V / 4096 / 560 Ω × 5000 = 7.19 mA |
| Full scale | 29.5 A, above the current limit (≈ 23.5 A typical at R_ILIM 6k8) |
| Fault level | SNS sources at least 4 mA (I_SNSFH) on a fault: codes ≥ 2780 (≈ 20 A) are flagged `FAULT` |
| Accuracy | ±5 % from 0.3 A up; ±15 % at 100 mA; meaningless below 50 mA |

- **DIA_EN:** the SNS output is on only while DIA_EN is high. DIA_EN is
  common to all six switches. The satellite raises it before it switches
  the outputs on, and restores the `sat diag` setting afterwards. SEL1 is
  tied low, so SNS reports current, not temperature.
- **Sampling:** the fired channels and V_PYRO are sampled every 1 ms
  (`CONFIG_SAT_IGNLOG_PERIOD_US`). Sampling starts when the outputs go on
  and stops 5 samples after they go off. The first sample is taken before
  SNS has settled (165 µs) and usually reads 0.
- **One record per channel and pulse:** a FIRE on two channels makes two
  records. At most `CONFIG_SAT_IGNLOG_MAX_SAMPLES` (1024) samples are kept;
  a longer pulse is cut short.

Each record carries a short summary:

- **Peak:** the highest sample.
- **Opened:** the first sample after which the current stayed below
  max(peak / 8, 100 mA). If the current only stops when the pulse ends, the
  igniter never opened.
- **V_PYRO min:** the lowest fire rail voltage during the pulse.
- **Flags:** `FAULT` (the TPS1HB08 reported a fault or hit its current
  limit), `cut short`, `overrun` (the ADC missed sampling intervals), and
  no current at all.

## Storage

The satellite keeps `CONFIG_SAT_IGNLOG_RECORDS` (8) records in a ring in
RAM, about 16.6 KB, and the oldest goes first.

- **What it survives:** the RAM is not cleared at boot (`__noinit`), so
  records survive a reset, a watchdog trip, `kernel reboot` and the reboot
  through MCUboot. They do not survive switching the satellite off.
- **CRCs:** every record has a CRC32, and the store has a header check. A
  damaged record is dropped at boot. A damaged header starts the store over
  with a new random **store id**.
- **Time stamps:** records carry the satellite's uptime and its **boot
  count** at the time, so "5 min ago" is shown only for records from the
  current boot.
- **MCUboot's RAM:** MCUboot runs in the same RAM before the app, but it
  ends below 0x20004400. The app's noinit section, which holds the store,
  starts above 0x20008C00. Check this after changing either image:

  ```sh
  arm-none-eabi-size -A build-dfu/mcuboot/zephyr/zephyr.elf | grep noinit
  arm-none-eabi-nm build-dfu/satellite/zephyr/zephyr.elf | grep ' store$'
  ```

  If they ever overlap, the CRCs drop the records instead of reporting
  garbage.

## Record format

The same byte blob is used everywhere: the satellite's store, the LoRa
chunks, the controller's cache and the mcumgr reads. It is defined in
[`firmware/common/ignlog/ignlog_rec.h`](../firmware/common/ignlog/ignlog_rec.h)
and parsed in the browser by `tools/webtool/src/smp/ignlog.ts`.
Little-endian, version 1:

| Offset | Field |
|---|---|
| 0 | u8 version (1), u8 channel (0–5) |
| 2 | u8 flags: bit 0 fault, 1 cut short, 2 overrun, 3 no current; u8 reserved |
| 4 | u32 seq, per store, from 1 |
| 8 | u32 uptime at the start of the pulse, ms |
| 12 | u16 boot count, u16 store id |
| 16 | u16 sample period µs, u16 sample count *n* |
| 20 | u16 pulse length asked for, ms; u16 lowest V_PYRO, mV |
| 24 | u32 scale: nA of load current per ADC code |
| 28 | u16 peak code, u16 index of the opening sample (0xFFFF: never opened) |
| 32 | u16 × *n* raw ADC codes |

The codes travel raw, together with the scale, so a calibration change
does not invalidate stored records. A one-second pulse makes a record of
2042 bytes.

## Download over LoRa

See [radio_link.md](radio_link.md#ignition-log-download) for the frames.

- **When:** the controller downloads only while it is SAFE and no command
  waits for its acknowledgement. Arming, or any request, pauses the
  download at once.
- **How:** each **log poll** addresses one satellite. It asks for the index
  of its records, or for one chunk of up to 192 bytes of a record. The
  satellite answers in its reply slot instead of with its status.
- **Speed:** one log poll is out at a time, between the regular 1.2 s
  polls, which keep every satellite's heartbeat going. A chunk takes about
  250 ms, so a one-second record takes about 3 s.
- **Retries:** an unanswered log poll is asked again after 900 ms, up to 6
  times. A chunk request for a record the satellite no longer holds is
  answered with the index.
- **Cache:** the controller keeps up to `CONFIG_APP_IGNLOG_CACHE` (8)
  records, one satellite's whole log in about 17 KB of DRAM, and evicts the
  least recently used. DRAM is nearly full on the ESP32-S3, and its PSRAM
  is not enabled yet. It drops a
  satellite's records when that satellite's store id changes.

## On the controller

*Menu → Diagnostics → Ignition log*:

1. **Satellite:** pick one; OK asks it for its index.
2. **Records:** newest first: channel and pad, peak current, when the
   igniter opened, and how long ago. OK opens the chart and downloads the
   record if needed; holding OK asks for the index again.
3. **Chart:** current over the whole pulse; LEFT/RIGHT switch to the first
   50 ms. UP/DOWN step to the next newer or older record. The line under it
   gives the peak, the opening time, V_PYRO min and the flags.

## mcumgr group 64

Both boards serve the records over mcumgr, group 64
(`MGMT_GROUP_ID_PERUSER`), on the same USB shell as the terminal
([`firmware/common/ignlog/ignlog_mgmt.c`](../firmware/common/ignlog/ignlog_mgmt.c)).

- **Stores:** the satellite has one store, *sat* 0, its own. The controller
  has one store per satellite slot, *sat* 1–8, holding what it downloaded.

| Id | Op | Request | Response |
|---|---|---|---|
| 0 list | read | `{sat?}` | `{busy, sats?, sat, store, boots, now, scale, records}` |
| 1 read | read | `{sat?, seq, off}` | `{off, total, data}`, at most 256 bytes of data |
| 2 clear | write | `{sat?}` | satellite: erase its records (refused while armed); controller: drop its copies of slot *sat*, or of all |
| 3 fetch | write | `{sat}` | controller only: download slot *sat*'s index and every record it lacks. `EBUSY` while armed |

The fields of a list response:

- **records:** one array per record, oldest first:
  `[seq, channel, flags, samples, period_us, peak_code, open_idx, t_fire_ms, boot, size, have]`.
  `have` is false on the controller for a record not downloaded yet.
- **busy:** on the satellite, 1 while a pulse is being recorded. On the
  controller, the slot being downloaded, or 0.
- **sats:** on the controller, when no `sat` is given:
  `[sat, store, count, have_mask]` for every slot it holds an index of.

**Sharing the port with the shell:** every response fits a few 127-byte
console frames. The browser tool sends one request at a time, and
keystrokes go out between frames, so the terminal keeps working during a
download. If a log line lands inside a frame, the frame's CRC fails, the
request times out, and the client asks again. Reads are idempotent.

## Testing

- **Satellite on native_sim:** `sim_io.c` feeds the rails ADC an
  e-match-like current while a channel's switches are on. Channels 1–5
  carry about 1.8 A and open after 5, 8, 11, 14 and 17 ms. Channel 6 draws
  3.2 A and never opens. To try it, run `sat arm 1 1`, `sat fire 1 200`,
  then `sat ignlog` and `sat ignlog dump 1`.
- **Controller on native_sim:** the simulated satellites (`sat_sim.c`) make
  records from their loads on FIRE and answer log polls.
- **mcumgr:** build the native_sim satellite with mcumgr, then run the
  test. It fires through the shell and reads the records over SMP on the
  same pty, with the shell answering in between:

  ```sh
  west build -b native_sim/native/64 -d satellite/build-smp satellite \
      -- -DEXTRA_CONF_FILE=../common/mgmt/mcumgr.conf
  ```

  then `tools/webtool/scripts/ignlog-simtest.ts`; its header has the
  command.
- **Hardware, still to do:** compare a trace on a known resistor load with
  a clamp meter. This checks K_SNS, the channel mapping and DIA_EN.
