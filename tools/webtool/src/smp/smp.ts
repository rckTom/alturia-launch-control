// SPDX-License-Identifier: GPL-3.0-only
//
// SMP (Simple Management Protocol) requests over a byte link: the header,
// sequence numbers, timeouts and the result codes, plus the handful of
// commands the tool needs from Zephyr's OS and image groups.

import { decode, encode, type CborValue } from './cbor'
import { encodePacket } from './frame'

export const OP_READ = 0
export const OP_WRITE = 2

export const GROUP_OS = 0
export const GROUP_IMAGE = 1

const OS_ECHO = 0
const OS_RESET = 5
const OS_PARAMS = 6
const OS_INFO = 7
const OS_BOOTLOADER = 8
const IMG_STATE = 0
const IMG_UPLOAD = 1

/** SMP version 2 header bit, so errors come back as {err: {group, rc}}. */
const SMP_V2 = 1 << 3

type Map = { [key: string]: CborValue }

// mcumgr_err_t: the generic result codes
const MGMT_ERR: Record<number, string> = {
  1: 'unknown error',
  2: 'out of memory',
  3: 'invalid argument',
  4: 'timed out',
  5: 'no such entry',
  6: 'bad state',
  7: 'response too large',
  8: 'not supported',
  9: 'corrupt',
  10: 'busy',
  11: 'access denied',
}

// img_mgmt group errors (img_mgmt_err_code_t)
const IMG_ERR: Record<number, string> = {
  1: 'unknown image error',
  2: 'flash config query failed',
  3: 'no image',
  4: 'no TLVs',
  5: 'invalid TLV',
  6: 'multiple hashes found',
  7: 'invalid TLV size',
  8: 'hash not found',
  9: 'no free slot',
  10: 'flash area open failed',
  11: 'flash area read failed',
  12: 'flash area write failed',
  13: 'flash area erase failed',
  14: 'invalid slot',
  15: 'no free memory',
  16: 'flash context already set',
  17: 'flash context not set',
  18: 'flash area device is null',
  19: 'invalid page offset',
  20: 'invalid offset',
  21: 'invalid length',
  22: 'invalid image header',
  23: 'invalid image header magic',
  24: 'invalid hash',
  25: 'invalid flash address',
  26: 'version get failed',
  27: 'current version is newer',
  28: 'image already pending',
  29: 'invalid image vector table',
  30: 'image too large',
  31: 'image data overrun',
  32: 'image confirmation denied',
  33: 'setting test to active denied',
  34: 'active slot not known',
}

export class SmpError extends Error {
  constructor(
    message: string,
    readonly rc: number,
    readonly group?: number,
  ) {
    super(message)
  }
}

export class SmpTimeout extends Error {}

export interface ImageSlot {
  image: number
  slot: number
  version: string
  hash: Uint8Array
  bootable: boolean
  pending: boolean
  confirmed: boolean
  active: boolean
  permanent: boolean
}

interface Pending {
  resolve: (m: Map) => void
  reject: (e: Error) => void
  timer: ReturnType<typeof setTimeout>
}

export class SmpClient {
  private seq = 0
  private pending = new globalThis.Map<number, Pending>()

  constructor(private readonly write: (bytes: Uint8Array) => Promise<void>) {}

  /** Feed a packet that StreamDemux took out of the stream. */
  receive(packet: Uint8Array) {
    if (packet.length < 8) return
    const seq = packet[6]
    const p = this.pending.get(seq)
    if (!p) return
    this.pending.delete(seq)
    clearTimeout(p.timer)
    let body: CborValue
    try {
      body = decode(packet.subarray(8))
    } catch (e) {
      p.reject(e as Error)
      return
    }
    const m = (body ?? {}) as Map
    const err = m.err as Map | undefined
    if (err && typeof err.rc === 'number' && err.rc !== 0) {
      const group = err.group as number
      const text = group === GROUP_IMAGE ? IMG_ERR[err.rc] : undefined
      p.reject(new SmpError(text ?? `group ${group} error ${err.rc}`, err.rc, group))
    } else if (typeof m.rc === 'number' && m.rc !== 0) {
      p.reject(new SmpError(MGMT_ERR[m.rc] ?? `error ${m.rc}`, m.rc))
    } else {
      p.resolve(m)
    }
  }

  /** Fail whatever is still waiting, e.g. when the port goes away. */
  abort(why: string) {
    for (const p of this.pending.values()) {
      clearTimeout(p.timer)
      p.reject(new Error(why))
    }
    this.pending.clear()
  }

  async request(op: number, group: number, id: number, body: Map = {}, timeoutMs = 2500): Promise<Map> {
    const payload = encode(body)
    const seq = this.seq
    this.seq = (this.seq + 1) & 0xff
    const hdr = new Uint8Array(8)
    hdr[0] = SMP_V2 | op
    hdr[1] = 0
    hdr[2] = payload.length >> 8
    hdr[3] = payload.length & 0xff
    hdr[4] = group >> 8
    hdr[5] = group & 0xff
    hdr[6] = seq
    hdr[7] = id
    const packet = new Uint8Array(8 + payload.length)
    packet.set(hdr)
    packet.set(payload, 8)

    const reply = new Promise<Map>((resolve, reject) => {
      const timer = globalThis.setTimeout(() => {
        this.pending.delete(seq)
        reject(new SmpTimeout('no answer from the device'))
      }, timeoutMs)
      this.pending.set(seq, { resolve, reject, timer })
    })
    try {
      await this.write(encodePacket(packet))
    } catch (e) {
      const p = this.pending.get(seq)
      if (p) {
        clearTimeout(p.timer)
        this.pending.delete(seq)
      }
      throw e
    }
    return reply
  }

  // ---- OS group ----

  async echo(text: string): Promise<string> {
    const r = await this.request(OP_WRITE, GROUP_OS, OS_ECHO, { d: text }, 1000)
    return String(r.r ?? '')
  }

  /** Board target, e.g. "alturia_satellite/stm32g474xx". */
  async board(): Promise<string> {
    const r = await this.request(OP_READ, GROUP_OS, OS_INFO, { format: 'i' })
    return String(r.output ?? '')
  }

  async info(format = 'a'): Promise<string> {
    const r = await this.request(OP_READ, GROUP_OS, OS_INFO, { format })
    return String(r.output ?? '')
  }

  async bootloader(): Promise<string> {
    const r = await this.request(OP_READ, GROUP_OS, OS_BOOTLOADER)
    return String(r.bootloader ?? '')
  }

  /** The device's SMP buffer size, which bounds an upload chunk. */
  async params(): Promise<{ bufSize: number; bufCount: number }> {
    const r = await this.request(OP_READ, GROUP_OS, OS_PARAMS)
    return { bufSize: Number(r.buf_size ?? 0), bufCount: Number(r.buf_count ?? 0) }
  }

  async reset(): Promise<void> {
    await this.request(OP_WRITE, GROUP_OS, OS_RESET, {})
  }

  // ---- image group ----

  async images(): Promise<ImageSlot[]> {
    const r = await this.request(OP_READ, GROUP_IMAGE, IMG_STATE)
    const list = (r.images ?? []) as Map[]
    return list.map((m) => ({
      image: Number(m.image ?? 0),
      slot: Number(m.slot ?? 0),
      version: String(m.version ?? ''),
      hash: (m.hash as Uint8Array) ?? new Uint8Array(),
      bootable: Boolean(m.bootable),
      pending: Boolean(m.pending),
      confirmed: Boolean(m.confirmed),
      active: Boolean(m.active),
      permanent: Boolean(m.permanent),
    }))
  }

  /** Mark an image for a test boot, or (confirm) make it permanent. */
  async setImageState(hash: Uint8Array, confirm: boolean): Promise<ImageSlot[]> {
    await this.request(OP_WRITE, GROUP_IMAGE, IMG_STATE, { hash, confirm }, 10000)
    return this.images()
  }

  /**
   * Upload an image to the secondary slot. The first chunk erases the
   * slot, which takes a few seconds on the satellite. A chunk without an
   * answer is sent again from the offset the device last reported.
   */
  async upload(
    data: Uint8Array,
    sha: Uint8Array,
    chunk: number,
    onProgress: (sent: number) => void,
    signal?: AbortSignal,
  ): Promise<void> {
    let off = 0
    let retries = 0
    while (off < data.length) {
      if (signal?.aborted) throw new Error('upload cancelled')
      const n = Math.min(chunk, data.length - off)
      const body: Map = { image: 0, off, data: data.subarray(off, off + n) }
      if (off === 0) {
        body.len = data.length
        body.sha = sha
      }
      let r: Map
      try {
        r = await this.request(OP_WRITE, GROUP_IMAGE, IMG_UPLOAD, body, off === 0 ? 30000 : 5000)
      } catch (e) {
        if (e instanceof SmpTimeout && retries < 5) {
          retries++
          continue
        }
        throw e
      }
      retries = 0
      off = typeof r.off === 'number' ? r.off : off + n
      onProgress(off)
    }
  }
}

/** How much image data fits one upload request on a device with this buffer. */
export function uploadChunk(bufSize: number): number {
  // SMP header, the CBOR map around the data and the serial framing's
  // length and CRC take some 40 bytes; stay below 512 for the satellite.
  const room = (bufSize || 384) - 64
  return Math.max(64, Math.min(512, Math.floor(room / 4) * 4))
}
