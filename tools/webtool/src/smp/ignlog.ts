// SPDX-License-Identifier: GPL-3.0-only
//
// The ignition log over mcumgr: group 64 on the satellite (its own records)
// and on the controller (the records it downloaded over LoRa, per
// satellite slot). See documentation/ignition_log.md and
// firmware/common/ignlog/ignlog_mgmt.c.
//
// One request at a time, each answer a few frames long: the shell's
// terminal on the same port stays usable while records come in.

import type { CborValue } from './cbor'
import { OP_READ, OP_WRITE, SmpClient, SmpTimeout } from './smp'

export const GROUP_IGNLOG = 64
const ID_LIST = 0
const ID_READ = 1
const ID_CLEAR = 2
const ID_FETCH = 3

type Map = { [key: string]: CborValue }

export const FLAG_FAULT = 1 << 0
export const FLAG_TRUNCATED = 1 << 1
export const FLAG_OVERRUN = 1 << 2
export const FLAG_NO_CURRENT = 1 << 3
export const OPEN_NONE = 0xffff

const HDR_LEN = 32

/** One record as listed. */
export interface Entry {
  seq: number
  channel: number   // 0..5, panel 1..6
  flags: number
  samples: number
  periodUs: number
  peakCode: number
  openIdx: number   // OPEN_NONE: never opened
  tFireMs: number   // satellite uptime
  boot: number
  size: number
  have: boolean     // held whole (always on a satellite)
}

/** A store: the satellite's own (sat 0), or the controller's copy of slot sat (1..8). */
export interface Store {
  sat: number
  storeId: number
  boots: number
  nowMs: number
  scaleNa: number
  records: Entry[]
}

/** The controller's slots that have an index. */
export interface SatSummary {
  sat: number
  storeId: number
  count: number
  have: number
}

export interface Listing {
  /** Satellite: 1 while a pulse is being recorded. Controller: slot being downloaded, 0 for none. */
  busy: number
  sats: SatSummary[]
  store?: Store
}

export interface IgnitionRecord {
  seq: number
  channel: number
  flags: number
  boot: number
  storeId: number
  tFireMs: number
  periodUs: number
  pulseMs: number
  vpyroMinMv: number
  scaleNa: number
  peakCode: number
  openIdx: number
  codes: Uint16Array
  /** Load current per sample, mA */
  ma: Float64Array
}

const num = (v: CborValue | undefined) => (typeof v === 'number' ? v : 0)

function entry(v: CborValue): Entry {
  const a = (Array.isArray(v) ? v : []) as CborValue[]
  return {
    seq: num(a[0]),
    channel: num(a[1]),
    flags: num(a[2]),
    samples: num(a[3]),
    periodUs: num(a[4]),
    peakCode: num(a[5]),
    openIdx: num(a[6]),
    tFireMs: num(a[7]),
    boot: num(a[8]),
    size: num(a[9]),
    have: a[10] === true,
  }
}

/** The answer of the list command. */
export function parseListing(m: Map): Listing {
  const sats = (Array.isArray(m.sats) ? m.sats : []).map((v) => {
    const a = (Array.isArray(v) ? v : []) as CborValue[]
    return { sat: num(a[0]), storeId: num(a[1]), count: num(a[2]), have: num(a[3]) }
  })
  const listing: Listing = { busy: num(m.busy), sats }
  if (Array.isArray(m.records)) {
    listing.store = {
      sat: num(m.sat),
      storeId: num(m.store),
      boots: num(m.boots),
      nowMs: num(m.now),
      scaleNa: num(m.scale),
      records: m.records.map(entry),
    }
  }
  return listing
}

export class RecordError extends Error {}

/** Parse a record blob (firmware/common/ignlog/ignlog_rec.h). */
export function parseRecord(b: Uint8Array): IgnitionRecord {
  if (b.length < HDR_LEN || b[0] !== 1) throw new RecordError('not an ignition log record')
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength)
  const n = dv.getUint16(18, true)
  if (b.length !== HDR_LEN + 2 * n) throw new RecordError(`record is ${b.length} bytes, expected ${HDR_LEN + 2 * n}`)
  const scaleNa = dv.getUint32(24, true)
  const codes = new Uint16Array(n)
  const ma = new Float64Array(n)
  for (let i = 0; i < n; i++) {
    codes[i] = dv.getUint16(HDR_LEN + 2 * i, true)
    ma[i] = (codes[i] * scaleNa) / 1e6
  }
  return {
    channel: b[1],
    flags: b[2],
    seq: dv.getUint32(4, true),
    tFireMs: dv.getUint32(8, true),
    boot: dv.getUint16(12, true),
    storeId: dv.getUint16(14, true),
    periodUs: dv.getUint16(16, true),
    pulseMs: dv.getUint16(20, true),
    vpyroMinMv: dv.getUint16(22, true),
    scaleNa,
    peakCode: dv.getUint16(28, true),
    openIdx: dv.getUint16(30, true),
    codes,
    ma,
  }
}

/** ms after the start of the pulse at which the igniter opened, or undefined. */
export function openMs(r: { openIdx: number; periodUs: number }): number | undefined {
  return r.openIdx === OPEN_NONE ? undefined : (r.openIdx * r.periodUs) / 1000
}

/** Time (ms), current (mA) rows. */
export function toCsv(r: IgnitionRecord): string {
  const rows = ['t_ms,code,ma']
  for (let i = 0; i < r.codes.length; i++) {
    rows.push(`${((i * r.periodUs) / 1000).toFixed(3)},${r.codes[i]},${r.ma[i].toFixed(0)}`)
  }
  return rows.join('\n') + '\n'
}

export class IgnlogClient {
  constructor(private readonly smp: SmpClient) {}

  async list(sat?: number): Promise<Listing> {
    return parseListing(await this.smp.request(OP_READ, GROUP_IGNLOG, ID_LIST, sat ? { sat } : {}))
  }

  /**
   * A whole record, in requests of a few hundred bytes. Timeouts are asked
   * again (a log line can land inside a frame and spoil it).
   */
  async read(seq: number, sat = 0, onProgress?: (done: number, total: number) => void): Promise<IgnitionRecord> {
    let data: Uint8Array | undefined
    let off = 0
    let total = 1
    let tries = 0
    while (off < total) {
      let r: Map
      try {
        r = await this.smp.request(OP_READ, GROUP_IGNLOG, ID_READ, sat ? { sat, seq, off } : { seq, off })
      } catch (e) {
        if (e instanceof SmpTimeout && ++tries < 5) continue
        throw e
      }
      tries = 0
      const chunk = r.data instanceof Uint8Array ? r.data : new Uint8Array()
      total = num(r.total)
      if (num(r.off) !== off || total === 0 || (chunk.length === 0 && off < total)) {
        throw new RecordError('the device answered out of turn')
      }
      if (!data) data = new Uint8Array(total)
      data.set(chunk, off)
      off += chunk.length
      onProgress?.(off, total)
    }
    return parseRecord(data ?? new Uint8Array())
  }

  /** Satellite: forget its records. Controller: drop the copies (of one slot, or all). */
  async clear(sat = 0): Promise<void> {
    await this.smp.request(OP_WRITE, GROUP_IGNLOG, ID_CLEAR, sat ? { sat } : {})
  }

  /**
   * Controller: download satellite slot @p sat's records over LoRa, then
   * wait until it is done (it takes a few seconds per record).
   */
  async fetch(sat: number, onBusy?: (l: Listing) => void, timeoutMs = 120000): Promise<Listing> {
    await this.smp.request(OP_WRITE, GROUP_IGNLOG, ID_FETCH, { sat })
    const until = Date.now() + timeoutMs
    // The controller starts it on its next tick.
    await new Promise((r) => setTimeout(r, 300))
    for (;;) {
      const l = await this.list(sat)
      onBusy?.(l)
      if (l.busy !== sat) return l
      if (Date.now() > until) throw new RecordError('the download did not finish')
      await new Promise((r) => setTimeout(r, 700))
    }
  }
}
