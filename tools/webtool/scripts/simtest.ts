// SPDX-License-Identifier: GPL-3.0-only
//
// Drives the SMP client against Zephyr's own SMP server: a native_sim
// build of an app with common/mgmt/mcumgr.conf, its shell on a pty.
//
//   west build -b native_sim/native/64 -d <dir> firmware/satellite \
//       -- -DEXTRA_CONF_FILE=../common/mgmt/mcumgr.conf
//   npm run simtest -- <dir>/zephyr/zephyr.exe <signed image>
//
// Echo (also one spanning six console frames), os info, buffer size, an
// upload of the image to slot 1, marking it for test, and the app's guard
// refusing an upload and a reset while a channel is armed; all through the
// demux and framing the browser uses, with the shell's output going past.

import { spawn, execFileSync } from 'node:child_process'
import { constants, mkdtempSync, openSync, readFileSync } from 'node:fs'
import tty from 'node:tty'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { createHash } from 'node:crypto'
import { StreamDemux } from '../src/smp/frame'
import { SmpClient, SmpError, uploadChunk } from '../src/smp/smp'
import { hex, parseImage, sameBytes } from '../src/smp/mcuboot'

const [exe, imagePath] = process.argv.slice(2)
if (!exe || !imagePath) {
  console.error('usage: simtest <zephyr.exe> <zephyr.signed.bin>')
  process.exit(2)
}

function check(ok: boolean, what: string) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`)
  if (!ok) process.exitCode = 1
}

const cwd = mkdtempSync(join(tmpdir(), 'simtest-'))
const child = spawn(exe, [], { cwd, stdio: ['ignore', 'pipe', 'pipe'] })
process.on('exit', () => child.kill())
let devlog = ''
child.stdout.on('data', (d: Buffer) => (devlog += d.toString()))
const pty: string = await new Promise((resolve, reject) => {
  let buf = ''
  child.stdout.on('data', (d: Buffer) => {
    buf += d.toString()
    const m = buf.match(/UART_0 connected to pseudotty: (\S+)/) ?? buf.match(/connected to pseudotty: (\S+)/)
    if (m) resolve(m[1])
  })
  child.on('exit', () => reject(new Error('zephyr.exe exited')))
})
console.log(`pty ${pty}`)
// Raw: no echo, no CR/LF games, which would break the frames.
execFileSync('stty', ['-F', pty, 'raw', '-echo'])

const fd = openSync(pty, constants.O_RDWR | constants.O_NOCTTY)
// tty streams poll the pty through libuv instead of blocking reads.
const input = new tty.ReadStream(fd)
input.setRawMode(true)
const output = new tty.WriteStream(fd)
let text = ''
const client = new SmpClient(
  (b) => new Promise<void>((resolve, reject) => output.write(b, (e) => (e ? reject(e) : resolve()))),
)
const demux = new StreamDemux(
  (t) => (text += new TextDecoder().decode(t)),
  (p) => client.receive(p),
  (why) => console.log(`bad frame: ${why}`),
)
input.on('data', (d: Buffer) => demux.push(new Uint8Array(d)))

try {
  await new Promise((r) => setTimeout(r, 500))
  check((await client.echo('hello')) === 'hello', 'echo')
  const board = await client.board()
  check(board.startsWith('native_sim'), `os info board: ${board}`)
  const { bufSize, bufCount } = await client.params()
  check(bufSize >= 512, `params: buf_size ${bufSize}, count ${bufCount}`)
  // Several console frames back to back (CONFIG_MCUMGR_TRANSPORT_SHELL_RX_BUF_COUNT)
  const long = 'x'.repeat(400)
  check((await client.echo(long)) === long, 'echo of 400 characters (6 frames)')

  const data = new Uint8Array(readFileSync(imagePath))
  const img = parseImage(data)
  const sha = new Uint8Array(createHash('sha256').update(data).digest())
  const chunk = process.env.CHUNK ? Number(process.env.CHUNK) : uploadChunk(bufSize)
  const t0 = Date.now()
  let last = 0
  await client.upload(data, sha, chunk, (sent) => {
    if (sent - last > 50000 || sent === data.length) {
      last = sent
      process.stdout.write(`  ${sent}/${data.length}\n`)
    }
  })
  const s = (Date.now() - t0) / 1000
  check(true, `upload ${data.length} B in ${s.toFixed(1)} s (${(data.length / 1024 / s).toFixed(0)} KB/s, ${chunk} B chunks)`)

  const slots = await client.images()
  const s1 = slots.find((x) => x.slot === 1)
  check(!!s1 && s1.version === img.version, `slot 1 version ${s1?.version}`)
  check(!!s1 && sameBytes(s1.hash, img.hash), `slot 1 hash ${s1 ? hex(s1.hash, 8) : '-'} = image ${hex(img.hash, 8)}`)

  const after = await client.setImageState(img.hash, false)
  check(!!after.find((x) => x.slot === 1 && x.pending), 'slot 1 pending for test')

  check(text.length > 0, `shell text got through (${text.length} chars)`)

  // Armed: uploads and resets are refused (common/mgmt/dfu_guard.c).
  // Without a console polling it, the satellite disarms itself at its next
  // 100 ms tick, so the request goes out the moment the shell confirms the
  // arm; a few tries catch the window.
  const whileArmed = async (what: string, request: () => Promise<unknown>) => {
    for (let attempt = 0; attempt < 8; attempt++) {
      const from = text.length
      output.write('sat arm 0 1\r')
      const t0 = Date.now()
      while (!/arm ch0: 0/.test(text.slice(from)) && Date.now() - t0 < 3000) {
        await new Promise((r) => setTimeout(r, 2))
      }
      try {
        await request()
      } catch (e) {
        if (e instanceof SmpError) {
          check(e.rc === 10, `${what} while armed refused (${e.message})`)
          output.write('sat arm 0 0\r')
          return
        }
      }
    }
    console.log(`skip ${what} while armed: never caught the device armed`)
  }
  await whileArmed('upload', () => client.upload(data.subarray(0, 512), sha, chunk, () => {}))
  // Last: if the reset does go through, the simulator exits.
  await whileArmed('reset', () => client.reset())
} catch (e) {
  check(false, String(e))
  console.log('shell text:', JSON.stringify(text.slice(-600)))
  console.log('device log:\n' + devlog.slice(-1500))
} finally {
  input.destroy()
  output.destroy()
  child.kill()
}

process.exit(process.exitCode ?? 0)
