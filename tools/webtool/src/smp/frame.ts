// SPDX-License-Identifier: GPL-3.0-only
//
// mcumgr's console framing (Zephyr subsys/mgmt/mcumgr/transport/src/
// serial_util.c), which the shell transport uses on the satellite's and
// the controller's USB shell:
//
//   body  = len (u16 BE, packet + 2) | packet | crc16 (u16 BE)
//   frame = 06 09 | base64(first chunk) | \n      first frame
//           04 14 | base64(next chunk)  | \n      continuation frames
//
// Each frame is at most 127 bytes and base64-encoded on its own, so every
// chunk but the last is a multiple of 3 bytes. The CRC is CRC-16/XMODEM
// (poly 0x1021, init 0) over the packet; over packet + CRC it is 0.

export const PKT_1 = 0x06
export const PKT_2 = 0x09
export const FRAG_1 = 0x04
export const FRAG_2 = 0x14
const MAX_FRAME = 127
// Raw bytes per frame: (127 - 2 marker - 1 newline) / 4 * 3
const CHUNK = Math.floor((MAX_FRAME - 3) / 4) * 3

export function crc16(data: Uint8Array, crc = 0): number {
  for (const b of data) {
    crc ^= b << 8
    for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff
  }
  return crc
}

function b64(bytes: Uint8Array): string {
  let s = ''
  for (const b of bytes) s += String.fromCharCode(b)
  return btoa(s)
}

function unb64(text: string): Uint8Array {
  const s = atob(text)
  const out = new Uint8Array(s.length)
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i)
  return out
}

/** The frames for one SMP packet, ready to write to the port. */
export function encodePacket(packet: Uint8Array): Uint8Array {
  const body = new Uint8Array(packet.length + 4)
  const crc = crc16(packet)
  body[0] = (packet.length + 2) >> 8
  body[1] = (packet.length + 2) & 0xff
  body.set(packet, 2)
  body[body.length - 2] = crc >> 8
  body[body.length - 1] = crc & 0xff

  const parts: number[] = []
  for (let off = 0; off < body.length; off += CHUNK) {
    parts.push(...(off === 0 ? [PKT_1, PKT_2] : [FRAG_1, FRAG_2]))
    for (const c of b64(body.subarray(off, off + CHUNK))) parts.push(c.charCodeAt(0))
    parts.push(0x0a)
  }
  return new Uint8Array(parts)
}

/**
 * Splits the byte stream from the port into terminal text and SMP
 * packets. A frame marker can show up anywhere, also in the middle of a
 * shell line (the prompt has no newline), so this runs byte by byte. The
 * markers are control characters the shell never prints.
 */
export class StreamDemux {
  private held = -1 // first marker byte, waiting for the second
  private frame: number[] | null = null
  private first = false
  private pkt: Uint8Array[] = []
  private pktLen = -1
  private have = 0

  constructor(
    private readonly onText: (bytes: Uint8Array) => void,
    private readonly onPacket: (packet: Uint8Array) => void,
    private readonly onBad: (why: string) => void = () => {},
  ) {}

  push(data: Uint8Array) {
    const text: number[] = []
    const flush = () => {
      if (text.length) this.onText(new Uint8Array(text.splice(0)))
    }

    for (const b of data) {
      if (this.frame) {
        if (b === 0x0a) {
          const line = this.frame
          this.frame = null
          this.fragment(String.fromCharCode(...line), this.first)
        } else if (b !== 0x0d) {
          this.frame.push(b)
          if (this.frame.length > MAX_FRAME) {
            this.frame = null
            this.onBad('frame too long')
          }
        }
        continue
      }
      if (this.held >= 0) {
        const h = this.held
        this.held = -1
        if ((h === PKT_1 && b === PKT_2) || (h === FRAG_1 && b === FRAG_2)) {
          flush()
          this.frame = []
          this.first = h === PKT_1
          continue
        }
        text.push(h)
      }
      if (b === PKT_1 || b === FRAG_1) this.held = b
      else text.push(b)
    }
    flush()
  }

  private fragment(text: string, first: boolean) {
    let bytes: Uint8Array
    try {
      bytes = unb64(text)
    } catch {
      this.reset('bad base64')
      return
    }
    if (first) {
      this.pkt = []
      this.have = 0
      if (bytes.length < 2) {
        this.reset('short first frame')
        return
      }
      this.pktLen = (bytes[0] << 8) | bytes[1]
      bytes = bytes.subarray(2)
    } else if (this.pktLen < 0) {
      this.onBad('continuation without start')
      return
    }
    this.pkt.push(bytes)
    this.have += bytes.length
    if (this.have < this.pktLen) return
    if (this.have > this.pktLen) {
      this.reset('packet longer than its length')
      return
    }
    const body = new Uint8Array(this.have)
    let off = 0
    for (const p of this.pkt) {
      body.set(p, off)
      off += p.length
    }
    this.pkt = []
    this.pktLen = -1
    if (crc16(body) !== 0) {
      this.onBad('CRC mismatch')
      return
    }
    this.onPacket(body.subarray(0, body.length - 2))
  }

  private reset(why: string) {
    this.pkt = []
    this.pktLen = -1
    this.onBad(why)
  }
}
