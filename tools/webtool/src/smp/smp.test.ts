// SPDX-License-Identifier: GPL-3.0-only
//
// `npm test`: CBOR, the console framing and the image parser, without a
// device. IMAGE=path/to/zephyr.signed.bin IMAGE_HASH=<hex> also checks the
// parser against a real image (imgtool dumpinfo prints the hash).

import { test } from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { decode, encode } from './cbor'
import { crc16, encodePacket, StreamDemux } from './frame'
import { hex, parseImage, compareVersions } from './mcuboot'
import { SmpClient } from './smp'

test('crc16 is CRC-16/XMODEM', () => {
  assert.equal(crc16(new TextEncoder().encode('123456789')), 0x31c3)
})

test('cbor round trip', () => {
  const v = {
    a: 0,
    b: 23,
    c: 24,
    d: 65535,
    e: 70000,
    f: -1,
    g: -1000,
    s: 'héllo',
    t: true,
    n: null,
    bytes: new Uint8Array([1, 2, 3]),
    list: [1, 'x', [2]],
    m: { nested: false },
  }
  assert.deepEqual(decode(encode(v)), v)
})

test('cbor decodes indefinite lengths', () => {
  // {_ "a": [_ 1, 2]}
  const b = new Uint8Array([0xbf, 0x61, 0x61, 0x9f, 0x01, 0x02, 0xff, 0xff])
  assert.deepEqual(decode(b), { a: [1, 2] })
})

function demux() {
  const text: number[] = []
  const packets: Uint8Array[] = []
  const bad: string[] = []
  const d = new StreamDemux(
    (t) => text.push(...t),
    (p) => packets.push(p),
    (w) => bad.push(w),
  )
  return { d, text, packets, bad }
}

test('frames: a long packet survives split reads amid shell text', () => {
  const packet = new Uint8Array(600).map((_, i) => (i * 7) & 0xff)
  const frames = encodePacket(packet)
  for (const line of new TextDecoder().decode(frames).split('\n').slice(0, -1)) {
    assert.ok(line.length + 1 <= 127, 'frame too long')
  }
  const { d, text, packets, bad } = demux()
  const before = new TextEncoder().encode('uart:~$ ')
  const after = new TextEncoder().encode('\r\nlog line\r\n')
  const all = new Uint8Array([...before, ...frames, ...after])
  for (let i = 0; i < all.length; i += 13) d.push(all.subarray(i, i + 13))
  assert.deepEqual(bad, [])
  assert.equal(packets.length, 1)
  assert.deepEqual(packets[0], packet)
  assert.equal(new TextDecoder().decode(new Uint8Array(text)), 'uart:~$ \r\nlog line\r\n')
})

test('frames: a corrupted packet is dropped, text still flows', () => {
  const frames = encodePacket(new Uint8Array([1, 2, 3, 4, 5]))
  frames[5] = frames[5] === 0x41 ? 0x42 : 0x41
  const { d, packets, bad, text } = demux()
  d.push(frames)
  d.push(new TextEncoder().encode('ok'))
  assert.equal(packets.length, 0)
  assert.equal(bad.length, 1)
  assert.equal(new TextDecoder().decode(new Uint8Array(text)), 'ok')
})

test('smp client pairs answers by sequence number', async () => {
  let sent: Uint8Array | undefined
  const client = new SmpClient(async (b) => {
    sent = b
  })
  const { d } = demux()
  void d
  const reply = client.echo('hi')
  // Decode what was sent and answer it like the device would.
  let req: Uint8Array | undefined
  new StreamDemux(
    () => {},
    (p) => (req = p),
  ).push(sent!)
  assert.ok(req)
  assert.equal(req![0] & 0x07, 2) // write
  assert.deepEqual(decode(req!.subarray(8)), { d: 'hi' })
  const body = encode({ r: 'hi' })
  const rsp = new Uint8Array(8 + body.length)
  rsp.set(req!.subarray(0, 8))
  rsp[0] = (req![0] & ~0x07) | 3
  rsp[2] = body.length >> 8
  rsp[3] = body.length & 0xff
  rsp.set(body, 8)
  client.receive(rsp)
  assert.equal(await reply, 'hi')
})

test('smp client turns rc into errors', async () => {
  let sent: Uint8Array | undefined
  const client = new SmpClient(async (b) => {
    sent = b
  })
  const reply = client.reset()
  let req: Uint8Array | undefined
  new StreamDemux(
    () => {},
    (p) => (req = p),
  ).push(sent!)
  const body = encode({ rc: 10 })
  const rsp = new Uint8Array(8 + body.length)
  rsp.set(req!.subarray(0, 8))
  rsp[3] = body.length
  rsp.set(body, 8)
  client.receive(rsp)
  await assert.rejects(reply, /busy/)
})

test('versions compare numerically', () => {
  assert.ok(compareVersions('0.10.0', '0.9.3') > 0)
  assert.equal(compareVersions('1.2.0', '1.2'), 0)
})

test('image parser matches imgtool', { skip: !process.env.IMAGE }, () => {
  const img = parseImage(new Uint8Array(readFileSync(process.env.IMAGE!)))
  if (process.env.IMAGE_VERSION) assert.equal(img.version, process.env.IMAGE_VERSION)
  if (process.env.IMAGE_HASH) assert.equal(hex(img.hash), process.env.IMAGE_HASH.toLowerCase())
  if (process.env.IMAGE_BOARD) assert.equal(img.board, process.env.IMAGE_BOARD)
})
