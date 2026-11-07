// SPDX-License-Identifier: GPL-3.0-only
//
// `npm test`: the ignition log record parser and the list/read client,
// against a record built here in the firmware's layout
// (firmware/common/ignlog/ignlog_rec.h) and a fake device.

import { test } from 'node:test'
import assert from 'node:assert/strict'
import { decode, encode, type CborValue } from './cbor'
import { SmpClient } from './smp'
import { GROUP_IGNLOG, IgnlogClient, OPEN_NONE, openMs, parseListing, parseRecord, toCsv } from './ignlog'

function blob(codes: number[], open = 3): Uint8Array {
  const b = new Uint8Array(32 + 2 * codes.length)
  const dv = new DataView(b.buffer)
  b[0] = 1 // version
  b[1] = 1 // CH2
  b[2] = 0
  dv.setUint32(4, 7, true) // seq
  dv.setUint32(8, 123456, true)
  dv.setUint16(12, 2, true) // boot
  dv.setUint16(14, 0xbeef, true)
  dv.setUint16(16, 1000, true) // period
  dv.setUint16(18, codes.length, true)
  dv.setUint16(20, 1000, true) // pulse
  dv.setUint16(22, 11800, true)
  dv.setUint32(24, 7193359, true) // nA per code
  dv.setUint16(28, Math.max(...codes), true)
  dv.setUint16(30, open, true)
  codes.forEach((c, i) => dv.setUint16(32 + 2 * i, c, true))
  return b
}

test('parse a record', () => {
  const r = parseRecord(blob([0, 245, 240, 0, 0]))
  assert.equal(r.seq, 7)
  assert.equal(r.channel, 1)
  assert.equal(r.storeId, 0xbeef)
  assert.equal(r.codes.length, 5)
  assert.equal(Math.round(r.ma[1]), 1762) // 245 * 7.193 mA
  assert.equal(openMs(r), 3)
  assert.equal(openMs({ openIdx: OPEN_NONE, periodUs: 1000 }), undefined)
  assert.match(toCsv(r), /^t_ms,code,ma\n0\.000,0,0\n1\.000,245,1762\n/)
})

test('a truncated record is refused', () => {
  assert.throws(() => parseRecord(blob([1, 2, 3]).subarray(0, 36)))
})

test('parse a listing', () => {
  const l = parseListing({
    busy: 0,
    sats: [[1, 0x1234, 2, 1]],
    sat: 1,
    store: 0x1234,
    boots: 0,
    now: 50000,
    scale: 7193359,
    records: [[7, 1, 0, 5, 1000, 245, 3, 40000, 0, 42, true]],
  })
  assert.deepEqual(l.sats, [{ sat: 1, storeId: 0x1234, count: 2, have: 1 }])
  assert.equal(l.store?.records[0].seq, 7)
  assert.equal(l.store?.records[0].have, true)
  assert.equal(l.store?.records[0].size, 42)
})

test('read a record in chunks from a fake device', async () => {
  const rec = blob(Array.from({ length: 300 }, (_, i) => (i < 8 ? 240 : 0)), 8)
  let client: SmpClient
  const reads: number[] = []
  // The device: answer read requests 256 bytes at a time.
  client = new SmpClient(async (bytes) => {
    const text = new TextDecoder().decode(bytes)
    const b64 = text
      .split('\n')
      .filter(Boolean)
      .map((l) => l.slice(2))
      .join('')
    const body = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0))
    const packet = body.subarray(2, body.length - 2)
    assert.equal((packet[4] << 8) | packet[5], GROUP_IGNLOG)
    const req = decode(packet.subarray(8)) as Record<string, CborValue>
    const off = req.off as number
    reads.push(off)
    const data = rec.subarray(off, off + 256)
    const payload = encode({ off, total: rec.length, data })
    const hdr = new Uint8Array([packet[0] | 1, 0, payload.length >> 8, payload.length & 0xff, 0, 64, packet[6], packet[7]])
    const reply = new Uint8Array(8 + payload.length)
    reply.set(hdr)
    reply.set(payload, 8)
    setTimeout(() => client.receive(reply), 1)
  })
  const r = await new IgnlogClient(client).read(7)
  assert.deepEqual(reads, [0, 256, 512])
  assert.equal(r.codes.length, 300)
  assert.equal(openMs(r), 8)
})
