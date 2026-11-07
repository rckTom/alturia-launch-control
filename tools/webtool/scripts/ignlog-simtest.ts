// SPDX-License-Identifier: GPL-3.0-only
//
// The ignition log client against a satellite on native_sim, with mcumgr
// and its shell on one pty, the way the browser shares the USB port:
// arm and fire two channels by typing shell commands, then list and read
// the records over SMP while the shell keeps printing (log lines, a
// command typed between the reads).
//
//   west build -b native_sim/native/64 -d satellite/build-smp satellite \
//       -- -DEXTRA_CONF_FILE=../common/mgmt/mcumgr.conf      (in firmware/)
//   npx esbuild scripts/ignlog-simtest.ts --bundle --platform=node --format=esm \
//       --outfile=node_modules/.cache/ignlog-simtest.mjs \
//     && node node_modules/.cache/ignlog-simtest.mjs <build-smp>/zephyr/zephyr.exe

import { spawn, execFileSync } from 'node:child_process'
import { constants, mkdtempSync, openSync } from 'node:fs'
import tty from 'node:tty'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { StreamDemux } from '../src/smp/frame'
import { SmpClient } from '../src/smp/smp'
import { IgnlogClient, OPEN_NONE, openMs } from '../src/smp/ignlog'

const [exe] = process.argv.slice(2)
if (!exe) {
  console.error('usage: ignlog-simtest <zephyr.exe>')
  process.exit(2)
}

function check(ok: boolean, what: string) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`)
  if (!ok) process.exitCode = 1
}

const cwd = mkdtempSync(join(tmpdir(), 'ignlog-simtest-'))
const child = spawn(exe, [], { cwd, stdio: ['ignore', 'pipe', 'inherit'] })
const pty: string = await new Promise((resolve, reject) => {
  let buf = ''
  child.stdout.on('data', (d: Buffer) => {
    buf += d.toString()
    const m = buf.match(/connected to pseudotty: (\S+)/)
    if (m) resolve(m[1])
  })
  child.on('exit', () => reject(new Error('zephyr.exe exited')))
})
execFileSync('stty', ['-F', pty, 'raw', '-echo'])

const fd = openSync(pty, constants.O_RDWR | constants.O_NOCTTY)
const input = new tty.ReadStream(fd)
input.setRawMode(true)
const output = new tty.WriteStream(fd)
let text = ''
let bad = 0
// One write queue for keystrokes and frames, as Link.write does.
let chain = Promise.resolve()
const write = (b: Uint8Array) =>
  (chain = chain.then(() => new Promise<void>((res, rej) => output.write(b, (e) => (e ? rej(e) : res())))))
const client = new SmpClient(write)
const demux = new StreamDemux(
  (t) => (text += new TextDecoder().decode(t)),
  (p) => client.receive(p),
  () => bad++,
)
input.on('data', (d: Buffer) => demux.push(new Uint8Array(d)))
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))
const type = async (cmd: string, ms = 400) => {
  await write(new TextEncoder().encode(cmd + '\r'))
  await sleep(ms)
}

try {
  await sleep(800)
  const log = new IgnlogClient(client)
  const before = await log.list()
  check(before.store?.records.length === 0, `empty store ${before.store?.storeId.toString(16)}`)

  // Fire CH2 (opens after 8 ms in the simulation) and CH6 (never opens)
  await type('sat arm 1 1')
  await type('sat arm 5 1')
  await type('sat fire 1 200', 700)
  await type('sat fire 5 60', 500)

  const l = await log.list()
  const recs = l.store?.records ?? []
  check(recs.length === 2, `two records listed: ${recs.map((r) => `#${r.seq} CH${r.channel + 1}`).join(', ')}`)

  // Read while the shell talks: a command typed between the requests
  const reading = log.read(recs[0].seq)
  void type('sat ignlog', 0)
  const r1 = await reading
  const r2 = await log.read(recs[1].seq, 0, (d, t) => d === t && console.log(`     ${t} bytes`))
  await sleep(500)
  check(text.includes('store ') && text.includes('2 records'), 'the shell answered "sat ignlog" meanwhile')
  check(r1.codes.length === recs[0].samples && r1.channel === 1, `record #1: ${r1.codes.length} samples`)
  check(openMs(r1) === 8, `CH2 opened after ${openMs(r1)} ms`)
  check(Math.abs(Math.max(...r1.ma) - 1770) < 30, `CH2 peak ${Math.round(Math.max(...r1.ma))} mA`)
  check(r2.openIdx === OPEN_NONE && r2.channel === 5, 'CH6 never opened')
  check(r2.vpyroMinMv > 10000 && r2.vpyroMinMv < 11200, `V_PYRO min ${r2.vpyroMinMv} mV`)
  check(bad === 0, `no damaged frames (${bad})`)

  const refused = await log.clear().then(
    () => false,
    () => true,
  )
  check(refused, 'clear refused while armed')
  await type('sat arm 1 0')
  await type('sat arm 5 0')
  await log.clear()
  check((await log.list()).store?.records.length === 0, 'cleared once SAFE')
} catch (e) {
  check(false, String(e))
} finally {
  child.kill()
  input.destroy()
  process.exit()
}
