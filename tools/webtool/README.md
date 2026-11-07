# Web tool

A static page that talks to the controller or a satellite over USB:
it shows the firmware version, updates the firmware and opens the shell.
It runs in Chrome or Edge on a computer (Web Serial). Nothing is installed
and nothing leaves the page. GitHub Pages serves it together with the
firmware built from the same commit
([workflow](../../.github/workflows/webtool.yml)).

## How it works

Both boards boot through MCUboot when built with sysbuild, and answer
mcumgr's SMP protocol on their USB shell (`firmware/common/mgmt`). SMP
frames travel inside the shell's byte stream. The page keeps one port
open, shows the shell in a terminal and picks the frames out of the same
stream, so the shell stays usable while an update runs.

An update goes like this:

1. Upload the signed image to slot 1. The first chunk erases the slot.
2. Mark the image for a test boot and reset.
3. MCUboot swaps the slots and starts the new image.
4. The page reconnects. A few seconds after start-up the app confirms the
   image itself. If it doesn't start, or doesn't confirm, the next reset
   brings the old image back.

The firmware refuses uploads and mcumgr resets while anything is armed.
Signed images carry their board name (TLV 0xa0), and the page won't put a
controller image on a satellite or the other way round.

"Go back to …" boots the image in the other slot again. That is the
previous firmware after an update.

## Development

```sh
npm install
npm run firmware   # copy firmware/*/build-dfu images into public/firmware
npm run dev        # http://localhost:5173 (Web Serial works on localhost)
npm test           # CBOR, framing, image parser
npm run build      # dist/, what Pages serves
```

`index.html#demo` shows the connected view with made-up data.

The firmware images come from sysbuild builds:

```sh
cd firmware/satellite
west build --sysbuild -b alturia_satellite -d build-dfu . -- -DBOARD_ROOT=$PWD/..
cd ../controller
west build --sysbuild -b alturia_controller/esp32s3/procpu -d build-dfu . -- -DBOARD_ROOT=$PWD/..
```

### Against Zephyr's SMP server, without hardware

`scripts/simtest.ts` drives the same client against a native_sim build of
the satellite, over its pty. It runs echo, a 400-byte multi-frame request,
os info, a full image upload to slot 1 and test-marking, and checks that
uploads and resets are refused while a channel is armed:

```sh
west build -b native_sim/native/64 -d /tmp/sat-smp firmware/satellite \
    -- -DEXTRA_CONF_FILE=../common/mgmt/mcumgr.conf
npm run simtest -- /tmp/sat-smp/zephyr/zephyr.exe public/firmware/alturia_satellite-0.1.0.bin
```

## Files

```text
src/smp/cbor.ts      the CBOR subset SMP uses
src/smp/frame.ts     mcumgr console framing, and the demux of shell text from SMP
src/smp/smp.ts       SMP requests: OS and image groups, upload
src/smp/mcuboot.ts   version, hash and board tag of a signed image
src/link.ts          the Web Serial port
src/device.ts        connection and update sequence
src/App.vue          the panel; components/ for the LCD, caps, lamps
scripts/firmware.ts  images -> firmware/manifest.json
```

## Look

The page borrows from the controller's front panel. The plate is
charcoal with warm white print in heavy, widely tracked Noto Sans, and
the buttons are round grey caps with their labels engraved. Status shows
as LED dots like STATUS and CHG. The terminal is drawn as the Sharp memory
LCD, with the controller UI's inverted title bar in Noto Sans Condensed
Bold. Everything is monochrome, like the white-only panel print. The
fonts are subset Noto (SIL OFL 1.1, `public/fonts/OFL.txt`).
