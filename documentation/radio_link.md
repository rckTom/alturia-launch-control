# Radio link

The controller and its satellites talk over one LoRa channel. This document
describes the link as implemented: the radio settings, pairing, the session
and its keys, the frame format, how the channel is shared, and the commands
and status that travel over it.

The code is in [`firmware/common/`](../firmware/common/). The controller, the
satellite firmware and the LoRa bench all run the same link code.

## Overview

- **One controller, up to 8 satellites.** Each satellite is paired to exactly
  one controller and holds one of its slots 1…8, shown as A…H on the
  controller.
- **The controller polls.** Every 1.2 s it broadcasts one `POLL` that carries a
  command for every satellite. Each satellite answers with its status in its
  own time slot after the poll. While a satellite has a session, it sends
  nothing but these replies and its answers to the controller's frames.
- **Everything after pairing is encrypted and authenticated** with
  AES-128-CCM. Replayed and forged frames are dropped.
- **A satellite that is not polled goes SAFE** after 4 s.

```
controller  |== POLL ==|                                       |== POLL == ...
                       |<-turn->|<--- slot 0 --->|<--- slot 1 --->|
satellite A                     |L|== status ==|
satellite C                                      |L|== status ==|

turn = LINK_TURNAROUND_MS   L = carrier sense before the reply
```

The link is built in layers:

| Layer | Files | Does |
|---|---|---|
| Application | `controller/src/controller.c`, `controller/src/link_lsec.c`, `satellite/src/commands.c`, `common/wire/sat_wire.c` | Commands and status, request ids, poll timing |
| Link glue | `common/link/ctrl_link.c`, `common/link/sat_link.c` | Link thread, TX queue, reply slots, radio profile switching, pairing storage |
| Channel access | `common/link/link_sched.c`, `common/link/link_access.c` | Airtime, slot times, channel utilization, CSMA/CA |
| Protocol | `common/link/lsec_*.c` | Pairing, join, group key, sealing and opening frames |
| Transport | `common/link/radio_transport.c`, `common/lora/radio.c` | The SX126x: profiles, RX timestamps, RSSI carrier sense |

The protocol layer (`lsec`) owns no thread, timer or radio. The caller passes
in each received frame and the current time, and gets frames to send and
events back through callbacks. This is what makes it unit-testable on
`native_sim`.

## Radio

| Setting | Link profile | Pairing profile |
|---|---|---|
| Frequency | 869.85 MHz | 869.85 MHz |
| Bandwidth | 250 kHz | 250 kHz |
| Spreading factor | SF7 | SF8 |
| Coding rate | 4/5 | 4/5 |
| Preamble | 8 symbols, explicit header, CRC, private sync word | same |
| TX power at the antenna | 7 dBm (max) | 0 dBm |

- **Band:** 869.85 MHz is the centre of the EU 869.70–870.00 MHz sub-band. It
  allows 5 mW ERP with no duty-cycle limit, so the controller and satellites
  may transmit as often as the channel allows. 7 dBm at the connector is
  5 mW ERP into an antenna of up to 0 dBd. A board with an external front end
  (`struct radio_frontend`) has its gain subtracted from the chip's power.
- **Profiles:** the two profiles differ in spreading factor, so a device on
  one does not hear a device on the other. Pairing runs on the pairing
  profile at low power, so it only works at close range. While the
  controller is scanning or pairing, it does not poll its other satellites.
- **Receiver:** it runs whenever the radio is not transmitting. Every frame
  is time-stamped at RX-done; reply slots are counted from that timestamp.
- **Driver:** the link needs the native SX126x backend with sleep disabled
  (`CONFIG_LORA_MODULE_BACKEND_NATIVE=y`, `CONFIG_LORA_SX126X_NATIVE_SLEEP=n`).
  The controller and satellite board configurations set both.

## Identities and addressing

- **Controller:** a random 32-bit `net_id`, created on first boot and kept in
  flash. Its low 16 bits (`net_tag`) are in every frame header, so frames of
  another controller's network are dropped before any crypto runs.
- **Satellite:** a 64-bit `sat_uid`, the hardware device id folded to 64 bits.
- **Addresses** in the frame header: `0` is the controller, `1…8` a satellite
  slot, `0xFF` the group (all satellites of the network).
- **One controller per satellite, one satellite per slot.** A new pairing
  replaces the satellite's previous one. On the controller, pairing a
  satellite into a slot clears that slot and any other slot the same
  satellite held.

## Frame format

Every frame starts with one byte: version (2 bits, `1`) and kind (6 bits).
Multi-byte fields are little-endian.

| Kinds | Layout |
|---|---|
| Pairing, 1–8 | `[ver|kind][sat_uid 8][body]` |
| Join, in the clear: `HELLO` 16, `JOIN_REQ` 17, `JOIN_RSP` 18 | `[ver|kind][net_tag 2][src][dst][body]` |
| Sealed: `JOIN_OK` 32, `JOIN_ACK` 33, `GROUP_UPDATE` 34, `GROUP_ACK` 35, `DATA` 36, `POLL` 37 | `[ver|kind][net_tag 2][src][dst][kid][ctr 4][ciphertext][tag 8]` |

Sealed frames:

```
0   u8     ver(2) | kind(6)
1   u16    net_tag          low 16 bits of net_id
3   u8     src              0 = controller, 1..8 = satellite slot
4   u8     dst              slot, or 0xFF = group
5   u8     kid              group key id for group frames, 0 otherwise
6   u32    ctr              per sender and key, strictly increasing
10  ...    AES-128-CCM ciphertext
N   u8[8]  tag
```

- **Associated data:** bytes 0–9. The kind is authenticated, so a frame
  cannot be relabelled (a `JOIN_OK` cannot pass as `DATA`).
- **Nonce:** 13 bytes, `net_id(4) ‖ src ‖ dst ‖ kid ‖ ctr(4) ‖ 00 00`. Each
  key belongs to one session and one direction, or to one group key epoch,
  and `ctr` only increases, so a nonce never repeats under a key.
- **Overhead:** 18 bytes per sealed frame. The largest frame is 255 bytes, so
  at most 237 bytes of payload.
- **Replay protection:** a frame is accepted only if its `ctr` is greater
  than the last one accepted under that key. Frames are not reordered. A
  sender never wraps its counter; the session has to be rejoined first.
- **Broadcasts:** only `POLL` and `DATA` may be sent to the group. `POLL` is
  always a group frame; `GROUP_UPDATE` is always per satellite.

## Crypto

All crypto goes through PSA Crypto on both MCUs.

| Use | Primitive |
|---|---|
| Key exchange | ECDH on P-256 |
| Commitment, SAS, key confirmation, join proof | HMAC-SHA256, truncated |
| Key derivation | HKDF-SHA256 |
| Frames | AES-128-CCM, 8-byte tag, deterministic nonce |
| Randomness | `psa_generate_random` (STM32 RNG, ESP32 TRNG, native_sim RNG) |

Keys are imported into PSA only for the duration of each operation, and
secrets are wiped after use.

## Pairing

Pairing works without a shared secret. The two devices run an
unauthenticated ECDH exchange, then the operator checks that both show the
same 24-bit pattern (numeric comparison, as in Bluetooth LE Secure
Connections). The satellite has no display, so it shows the pattern on its
six channel LEDs.

### Operating it

**On the satellite:**

1. All channels must be SAFE.
2. Press the button 3 times within 1 s (or `sat pair` on the USB shell). The
   satellite enters pairing mode for 120 s, and its status LED blinks fast.
3. Once the controller has connected, the channel LEDs show the pattern as 4
   frames of 1.5 s each, with the status LED lit on the first frame and 0.5 s
   dark before the sequence repeats.
4. A short press confirms that the pattern matches the controller, holding
   the button for 2 s rejects it (`sat pair confirm` / `sat pair reject`).

A failed or rejected attempt ends pairing mode; the next attempt needs the
gesture again.

**On the controller** (only in SAFE):

1. Settings → Satellites, pick a slot and press OK. The controller switches to
   the pairing profile and lists satellites in pairing mode with their RSSI.
2. Pick the satellite and press OK.
3. The controller shows the pattern as four rows of six dots. Row 1 is the
   frame shown with the status LED lit. Press OK if it matches the
   satellite's LEDs, BACK to reject. BACK cancels at any stage.

Holding OK on a slot in Settings → Satellites forgets that satellite.
`sat unpair` on the satellite's shell makes it forget its controller.

### Exchange

| # | Dir | Frame | Contents |
|---|---|---|---|
| 1 | S → all | `PAIR_ADV` | `sat_uid`, about every second while in pairing mode |
| 2 | C → S | `PAIR_REQ` | `net_id`, `PKc` (65-byte uncompressed point) |
| 3 | S → C | `PAIR_RSP` | `PKs`, commitment `Cs = HMAC(Ns, PKs.x ‖ PKc.x)[:16]` |
| 4 | C → S | `PAIR_NC` | `Nc` (16 bytes) |
| 5 | S → C | `PAIR_NS` | `Ns` (16 bytes). The controller checks `Cs` and aborts on a mismatch |
| — | both | pattern | `HMAC(Nc, PKc.x ‖ PKs.x ‖ Ns)[:3]`, shown to the operators |
| 6 | C → S | `PAIR_CHK_C` | `slot`, `Ec = HMAC(mac_key, "C" ‖ Nc ‖ Ns ‖ ids)[:16]`, sent when the controller's operator confirms |
| 7 | S → C | `PAIR_CHK_S` | `Es = HMAC(mac_key, "S" ‖ Ns ‖ Nc ‖ ids)[:16]`, sent when `Ec` is valid **and** the satellite's operator has confirmed |
| — | either | `PAIR_ABORT` | reason: rejected, timed out or a failed check |

`ids = net_id(4) ‖ sat_uid(8) ‖ slot(1)`. Keys:

```
DHKey          = ECDH(own private key, peer public key)
mac_key ‖ LTK  = HKDF-SHA256(salt = Nc ‖ Ns, ikm = DHKey,
                             info = "lc-pair-v1" ‖ net_id ‖ sat_uid ‖ PKc.x ‖ PKs.x)
```

`mac_key` is 32 bytes, the long-term key `LTK` 16 bytes.

**The pattern** is 24 bits, most significant first, split into four 6-bit
frames. In each frame, bit n is channel LED n+1.

**Storing:** the satellite stores the pairing once `Ec` checks out and its
operator has confirmed, then sends `PAIR_CHK_S`. The controller stores it on
a valid `PAIR_CHK_S`. The slot travels inside `PAIR_CHK_C` and is covered by
`Ec`.

**Loss and timeouts:** the controller resends its last frame every second
until the satellite answers; `PAIR_CHK_C` keeps repeating until the
satellite's operator has confirmed. The satellite answers a repeated frame
with its cached reply, and keeps answering late `PAIR_CHK_C`s for 3 s after it
has paired. The controller gives up 90 s after the start, the satellite when
its 120 s pairing mode ends.

### What it protects against

- **A passive eavesdropper** learns nothing; ECDH keeps the keys secret.
- **An active man in the middle** runs one ECDH with each side. Both
  patterns then differ, unless they match by chance (2⁻²⁴ per attempt). The
  commitment binds the satellite to `Ns` before it sees `Nc`, so an attacker
  cannot try nonces until the patterns match.
- **Swapped public keys** are caught by the commitment check on the
  controller.
- **A changed slot, or any mismatch in the derived keys**, is caught by
  `Ec` / `Es`.

The comparison by the operator is the security step: confirming a pattern
that does not match pairs the attacker.

## Session

The session is set up fresh on every boot of either end, with new keys and
counters. Counters are therefore never written to flash.

| # | Dir | Frame | Contents |
|---|---|---|---|
| 1 | S → C | `HELLO` | In the clear, no body. Sent about every 2 s while the satellite is paired but has no session. It only prompts a join |
| 2 | C → S | `JOIN_REQ` | `Nc` (8 bytes) |
| 3 | S → C | `JOIN_RSP` | `Ns` (8 bytes), `HMAC(auth, "J" ‖ Nc ‖ Ns ‖ ids)[:8]` with `auth = HKDF(ikm = LTK, info = "lc-auth-v1")`: proves the satellite holds the LTK and answers this `Nc` |
| — | both | keys | `K_c2s ‖ K_s2c = HKDF(salt = Nc ‖ Ns, ikm = LTK, info = "lc-sess-v1" ‖ net_id ‖ slot)`, one key per direction |
| 4 | C → S | `JOIN_OK` | Sealed with `K_c2s`: group key, `kid`, next group counter. Proves the controller |
| 5 | S → C | `JOIN_ACK` | Sealed with `K_s2c`, empty. The controller now counts the satellite as joined |

- **Spoofing does not break a running session.** A forged `HELLO` or a
  replayed `JOIN_REQ` leaves it up. The satellite keeps its current keys
  until a `JOIN_OK` sealed with the new ones arrives; the controller switches
  only on a `JOIN_RSP` that answers its own pending `Nc`.
- **Without a session a satellite accepts no command**, so it stays SAFE. An
  unpaired satellite sends nothing outside pairing mode.
- **Retries:** the controller answers a `HELLO` at most once a second, and
  sends `JOIN_REQ` to an unjoined slot on its own every 10 s. It resends
  `JOIN_OK` up to 3 times, 1 s apart. A resent `JOIN_OK` that reaches a
  satellite already on the new session is answered with another `JOIN_ACK`.
- **Jitter:** `HELLO`, `PAIR_ADV`, the `JOIN_REQ` retry and the `JOIN_OK` and
  `GROUP_UPDATE` resends are each moved by up to a quarter of their interval
  either way (`lsec_jittered()`), so that repeated frames do not stay in step
  with another network's cycle.

## Group key

The group key seals the controller's broadcasts (`dst = 0xFF`), above all the
`POLL`. Satellites never broadcast.

- **Lifetime:** random, created on every controller boot, and handed to each
  satellite in its `JOIN_OK`. There is no long-term group secret.
- **Rotation:** forgetting a satellite, or pairing a satellite into a slot
  that replaces an existing pairing, creates a new key and increments `kid`.
  The controller sends a sealed `GROUP_UPDATE` to every joined satellite and
  resends it every second until that satellite's `GROUP_ACK` arrives.
- **Replay window:** a resent `JOIN_OK` or `GROUP_UPDATE` with the same key
  never moves the group replay window back.
- **Broadcasts may carry any command, ARM and FIRE included.** Anyone holding
  the group key can therefore command every satellite of the network until
  the key is rotated or the controller reboots. A satellite stolen or
  tampered with during a session puts the whole network at risk for that
  session.

## Channel access

### The poll cycle

The controller broadcasts a `POLL` every 1.2 s (`APP_POLL_PERIOD_MS`), and
every 300–400 ms while a command waits for its acknowledgement (see
[Commands and status](#commands-and-status)). A new command is polled at
once. A regular poll is scheduled one period after the previous poll actually
went out.

- **Body** (inside the sealed frame): `[mask u8][slot_ms u8][app payload]`.
  Bit n of `mask` addresses slot n+1. The controller addresses every joined
  satellite.
- **Reply slots:** each addressed satellite replies in the slot given by its
  rank among the set bits of the mask. Slot k starts
  `LINK_TURNAROUND_MS + k × slot_ms` after the end of the poll, as each
  receiver time-stamps it. No clock synchronisation is needed: every
  receiver hears the poll end at the same moment.
- **Slot length:** `slot_ms = airtime(reply) + carrier sense + LINK_SLOT_GUARD_MS`,
  computed by the controller from the reply size it asks for. A satellite
  refuses a reply that does not fit its slot.
- **Empty slots:** a satellite that missed the poll, or whose bit is set but
  that has gone, leaves its slot empty. It cannot collide with anyone.
- **Missed slots:** a satellite that gets to its slot more than half the guard
  time late lets it pass. If its carrier sense at the start of the slot finds
  the channel busy, it skips the slot too.
- **The controller transmits from one thread**, frame by frame in the order
  they were sealed. After a poll it stays quiet until the last reply slot has
  ended. After any other frame it leaves a gap for the satellite's answer:
  turnaround, 10 ms for the satellite's carrier sense, the answer's airtime
  and the guard.
- **Polls are queued, never replaced.** The queue holds 4; when it is full a
  poll is refused (`poll_q_full`). While the controller scans or pairs, polls
  are refused.
- **A satellite with a reply pending** sends nothing else, so its own frames
  cannot land in another satellite's slot.

With short statuses, a poll to two satellites takes about 34 ms on air and
each reply slot 43 ms, so the whole cycle takes about 130 ms of each 1.2 s.

### CSMA/CA

Every frame from either end goes through CSMA/CA (`link_csma_*` in
`link_sched.c`, `link_csma_wait()` in `link_access.c`):

1. **Backoff:** wait a random 0 … 2^cw − 1 slots of `LINK_CSMA_SLOT_US` (1 ms).
2. **Sense:** if the channel is clear, send. If it is busy, sense again every
   slot until it clears, then draw a new backoff.
3. **Deadline:** after the full contention window plus the class's maximum
   deferral, either send anyway or drop the frame.

The contention window exponent `cw` grows linearly from the class's minimum
on an idle channel to its maximum on a fully busy one, using the device's
measured [channel utilization](#channel-utilization).

| Class | Frames | cw | Deadline | At the deadline |
|---|---|---|---|---|
| `link_csma_ctrl` | all controller frames, polls included | 3–7 | window + 50 ms (`LINK_LBT_MAX_DEFER_MS`) | send anyway: a poll carries SAFE and FIRE |
| `link_csma_slot` | satellite: its reply in a poll slot | 0 | one sense at the slot start | skip the slot |
| `link_csma_answer` | satellite: `JOIN_RSP`, `JOIN_ACK`, `GROUP_ACK`, pairing answers | 0–1 | 10 ms, the gap the controller leaves | drop; the controller asks again |
| `link_csma_unsolicited` | satellite: `HELLO`, `PAIR_ADV`, unscheduled data | 4–8 | window + 200 ms | drop; they are repeated anyway |

### Carrier sense

Carrier sense is done on RSSI (`radio_channel_clear()` in `radio.c`):

- The radio samples its RSSI every 200 µs for `LINK_LBT_LISTEN_US` (1 ms).
- The channel is busy if the loudest sample exceeds the noise floor by
  `LINK_LBT_MARGIN_DB` (10 dB). The threshold is clamped to −120…−60 dBm.
- **Noise floor:** the median of 32 samples, measured at start-up and on each
  profile change. After each sense it follows a quieter channel at once and
  a louder one by at most 0.25 dB, so bursts of traffic barely move it while
  a steady interferer is absorbed over some tens of senses.
- **Settling:** samples are taken only once the receiver has settled after
  restarting (the TCXO start-up delay from the devicetree plus 1 ms).
- A sense that fails counts as clear; it never blocks a frame.

RSSI sensing cannot see LoRa signals below the noise floor, although SF7
still decodes them. The reply slots are what keeps one network's frames
apart; carrier sensing catches other systems and mistakes. A slot's sense
also only sees what is already on the air when the slot starts.

### Channel utilization

Each device measures how busy the channel is over the last
`LINK_UTIL_WINDOW_S` (10 s), in 1 s buckets (`link_util_*`): the airtime of
every frame it receives from any network, its own transmissions, and every
sense that found the channel busy. The figure is capped at 100 %. A
satellite uses its own measurement, so it only backs off for traffic it can
hear.

The controller shows the utilization in `ctrl link`, and warns "Channel
crowded" on its screen from 70 % until it drops below 55 %.

### Timeouts

| Kconfig | Default | Who | What happens |
|---|---|---|---|
| `LINK_SAT_SAFE_TIMEOUT_MS` | 4000 | satellite | No poll addressed to it: disarm every channel. The session is kept, so the next poll brings it back. The controller shows the satellite as disconnected after the same time without a status |
| `LINK_CTRL_PEER_TIMEOUT_MS` | 8000 | controller | No frame from the satellite: drop its session (keep the pairing), stop addressing it, send `JOIN_REQ` at once |
| `LINK_SAT_SESSION_TIMEOUT_MS` | 10000 | satellite | No poll addressed to it: drop the session and send `HELLO` |

- At the 1.2 s poll period, the satellite goes SAFE after three polls in a
  row are lost.
- Only frames addressed to the satellite keep it alive: polls with its bit
  set, and unicast frames. A satellite the controller has stopped addressing
  therefore times out, even while it hears other satellites being polled.
- The pad goes SAFE first, the sessions drop later.
- After a satellite reconnects, the controller lists it as connected once its
  status has arrived without a gap for 2 s.

### Optional: other networks on the channel

`LINK_FOREIGN` (off by default, not enabled in any build) makes the
controller track other controllers' networks from the clear headers of their
frames: their poll period, the length of their cycles and the answer windows
after their join and key frames (`link_foreign_*` in `link_sched.c`). With it,
the controller holds each frame for up to `LINK_FOREIGN_MAX_WAIT_MS` (400 ms)
until it fits between the other networks' predicted cycles
(`LINK_FOREIGN_HOLD`), moves its regular poll toward the middle of the gap
between the neighbouring networks' cycles (`link_foreign_shift_us()`, at most
300 ms earlier or 150 ms later), and shows the number of other networks with
the "Channel crowded" warning.

Without it, networks sharing the channel are kept apart by the `net_tag`
(no frame of one is accepted by the other) and share airtime through CSMA/CA
alone.

`LINK_REPLY_CSMA` is an experiment for the simulator: satellites contend for
the whole reply window with CSMA/CA instead of using their rank's slot.

## Commands and status

The application payloads are defined in
[`common/wire/sat_wire.h`](../firmware/common/wire/sat_wire.h).

### Poll payload

`[type u8][one 4-byte record per addressed satellite]`, the records in the
order of the set bits of the mask (the order of the reply slots).

| Field | Contents |
|---|---|
| type | `3`: short status wanted, `5`: full status wanted |
| record byte 0 | command: `0` heartbeat, `1` ARM, `2` DISARM, `3` FIRE |
| record byte 1 | channel mask: ARM channels to arm, FIRE channels to pulse |
| record byte 2 | valve outputs wanted |
| record byte 3 | request id, `0` for none |

### Status reply

| Bytes | Short (15) | Full (44) adds |
|---|---|---|
| 0 | type: `4` short, `2` full | |
| 1–2 | seq (16 bits) | |
| 3 | state: SAFE, ARMED, FIRING | |
| 4 | flags: bit 0 = pad key switch in ARM | |
| 5–7 | armed, firing and valve channel masks | |
| 8 | fire count | |
| 9 | channels refused at ARM | |
| 10 | id of the last request executed | |
| 11–12 | continuity, 2 bits per channel: OK, open, short | |
| 13–14 | battery, mV | |
| 15–38 | | resistance per channel, mΩ (s32 each) |
| 39–40 | | rail current, mA |
| 41–42 | | RSSI, dBm |
| 43 | | SNR, dB |

- **Short status on every poll.** It holds everything the controller acts on
  or displays. The controller asks for the full status once after each
  join; a poll that asks for it gets full statuses from every satellite, with
  longer slots (about 66 ms instead of 43 ms).
- The controller merges a short status into the last full one, and extends
  the 16-bit seq against the previous value.

### Requests

- **Every command is a request with an 8-bit id** (never 0; 0 is a plain
  heartbeat). The satellite executes a request once. A command with the id
  it executed last is answered but not executed again. Its status reports
  the id of the last request it executed.
- **The controller repeats a request until the status reports its id**: a
  poll every `APP_CMD_RETRY_MS` (300 ms) plus up to 100 ms of jitter, for up
  to 5 s. ARM must be confirmed within 5 s and FIRE within 3 s, else the
  controller's fault handling takes over. A new request replaces an
  unacknowledged one.
- **FIRE is repeated like any other request.** The id keeps a repeat from
  firing again; the status's fire count confirms that the pulse happened.
- **Valves are state, not requests.** Every command carries the valve
  outputs the controller wants; the satellite applies them to armed channels
  on every poll. The controller polls at the retry rate until the status
  shows them.
- **Request ids start at a random value** on every controller boot, so the
  first request after a reboot is not taken for the satellite's last one. A
  rebooted satellite starts SAFE.
- **Disagreement:** if a satellite reports a state other than SAFE while the
  controller has nothing armed on it, the controller sends DISARM.

## Storage

Pairings are kept in Zephyr settings:

| Key | Board | Contents |
|---|---|---|
| `lsec/net` | controller | `net_id` |
| `lsec/p/<slot>` | controller | per paired satellite: `sat_uid`, `net_id`, `slot`, LTK |
| `lsec/p/0` | satellite | its own pairing |

The records, the LTK included, are stored in the clear. The flash is not yet
read-protected (RDP on the STM32, flash encryption on the ESP32-S3), so
anyone with a debugger can read the keys.

## Not covered

- **Jamming** is cheap on LoRa. A jammed satellite goes SAFE through its
  timeout.
- **Traffic analysis:** frame sizes and timing are visible.
- **A compromised satellite** holds the group key for the running session,
  see [Group key](#group-key).
- **Keys at rest**, see [Storage](#storage).

## Diagnostics

| Where | Command | Shows |
|---|---|---|
| Controller | `ctrl link` | network id, joined satellites, polls sent, CSMA counters, channel utilization, drop counters |
| Satellite | `sat link` | slot, session, polls, replies, late and busy slots, utilization, drop counters |
| Satellite | `sat pair [confirm\|reject]`, `sat unpair` | pairing from the USB shell |
| Controller on `native_sim` | `sat pair`, `sat confirm`, `sat link <a-h> off`, `sat loss` | drive the simulated satellites |

Received frames that are dropped are counted by reason: malformed, foreign
(another network, not addressed to us), bad tag, replay, no session.

## Configuration

The link's Kconfig options are in
[`common/link/Kconfig`](../firmware/common/link/Kconfig), which both apps
include; the poll and retry periods are in the controller's
[`Kconfig`](../firmware/controller/Kconfig).

| Option | Default | Meaning |
|---|---|---|
| `APP_POLL_PERIOD_MS` | 1200 | regular poll period |
| `APP_CMD_RETRY_MS` | 300 | poll period while a request is unacknowledged (plus 0–100 ms) |
| `LINK_SAT_SAFE_TIMEOUT_MS` | 4000 | see [Timeouts](#timeouts) |
| `LINK_CTRL_PEER_TIMEOUT_MS` | 8000 | see [Timeouts](#timeouts) |
| `LINK_SAT_SESSION_TIMEOUT_MS` | 10000 | see [Timeouts](#timeouts); keep it above the peer timeout |
| `LINK_TURNAROUND_MS` | 6 | end of the poll to the first slot |
| `LINK_SLOT_GUARD_MS` | 6 | guard time in every slot |
| `LINK_LBT` | y | carrier sense and CSMA/CA |
| `LINK_LBT_LISTEN_US` | 1000 | length of one carrier sense |
| `LINK_LBT_MARGIN_DB` | 10 | busy threshold above the noise floor |
| `LINK_LBT_MAX_DEFER_MS` | 50 | controller: send anyway this long after its contention window |
| `LINK_CSMA_SLOT_US` | 1000 | backoff unit |
| `LINK_CSMA_CTRL_CW_MIN` / `_MAX` | 3 / 7 | controller contention window exponent |
| `LINK_CSMA_SAT_CW_MIN` / `_MAX` | 4 / 8 | satellite `HELLO` and `PAIR_ADV` contention window exponent |
| `LINK_UTIL_WINDOW_S` | 10 | utilization window |
| `LINK_FOREIGN` | n | track other networks, see [above](#optional-other-networks-on-the-channel) |
| `LINK_REPLY_CSMA` | n | experiment: contended replies instead of slots |

Fixed protocol timing is in [`lsec.h`](../firmware/common/link/lsec.h)
(`LSEC_*_MS`).

## Tests and tools

- **Unit tests** for the protocol, the channel access policy and the wire
  format (`firmware/common/link/tests`, ztest on `native_sim`):

  ```sh
  west build -b native_sim/native/64 firmware/common/link/tests -t run
  ```

  They cover pairing and its failure cases (loss of every frame, a man in the
  middle, a bad commitment or confirmation, an altered slot), join, replay,
  tampering, foreign frames, reboots of either end, group key rotation,
  polls and slots, timeouts, CSMA/CA, utilization and the status codec.

- **Controller on `native_sim`** runs the real link against simulated
  satellites that run the real `lsec_sat` behind a simulated channel
  (`CONFIG_APP_SAT_SIM`, `controller/src/sat_sim.c`).
- **Radio network simulator:** real controller and satellite images on a
  simulated channel, with a web UI, see
  [`tools/radiosim/README.md`](../tools/radiosim/README.md).
- **Over the air:** two Heltec WiFi LoRa 32 V4 boards as controller and
  satellite, see
  [`firmware/lora_bench/README.md`](../firmware/lora_bench/README.md).
