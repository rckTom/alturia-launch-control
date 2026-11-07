// SPDX-License-Identifier: GPL-3.0-only
//
// The slice of CBOR (RFC 8949) that SMP uses: unsigned and negative
// integers, byte and text strings, arrays, maps with text keys, booleans,
// null. Definite lengths only on encode; indefinite ones are accepted on
// decode because zcbor may produce them.

export type CborValue =
  | number
  | bigint
  | string
  | boolean
  | null
  | undefined
  | Uint8Array
  | CborValue[]
  | { [key: string]: CborValue }

class Writer {
  private buf = new Uint8Array(64)
  private len = 0

  private grow(n: number) {
    if (this.len + n <= this.buf.length) return
    let size = this.buf.length * 2
    while (size < this.len + n) size *= 2
    const next = new Uint8Array(size)
    next.set(this.buf.subarray(0, this.len))
    this.buf = next
  }

  byte(b: number) {
    this.grow(1)
    this.buf[this.len++] = b
  }

  bytes(b: Uint8Array) {
    this.grow(b.length)
    this.buf.set(b, this.len)
    this.len += b.length
  }

  head(major: number, n: number) {
    const m = major << 5
    if (n < 24) this.byte(m | n)
    else if (n < 0x100) {
      this.byte(m | 24)
      this.byte(n)
    } else if (n < 0x10000) {
      this.byte(m | 25)
      this.byte(n >> 8)
      this.byte(n & 0xff)
    } else if (n < 0x100000000) {
      this.byte(m | 26)
      this.byte((n >>> 24) & 0xff)
      this.byte((n >>> 16) & 0xff)
      this.byte((n >>> 8) & 0xff)
      this.byte(n & 0xff)
    } else {
      throw new RangeError(`CBOR: ${n} does not fit 32 bits`)
    }
  }

  result(): Uint8Array {
    return this.buf.slice(0, this.len)
  }
}

const utf8 = new TextEncoder()
const utf8d = new TextDecoder()

function put(w: Writer, v: CborValue) {
  if (v === null || v === undefined) w.byte(0xf6)
  else if (v === false) w.byte(0xf4)
  else if (v === true) w.byte(0xf5)
  else if (typeof v === 'number') {
    if (!Number.isInteger(v)) throw new TypeError('CBOR: only integers are supported')
    if (v >= 0) w.head(0, v)
    else w.head(1, -1 - v)
  } else if (typeof v === 'bigint') {
    put(w, Number(v))
  } else if (typeof v === 'string') {
    const b = utf8.encode(v)
    w.head(3, b.length)
    w.bytes(b)
  } else if (v instanceof Uint8Array) {
    w.head(2, v.length)
    w.bytes(v)
  } else if (Array.isArray(v)) {
    w.head(4, v.length)
    for (const item of v) put(w, item)
  } else {
    const entries = Object.entries(v).filter(([, x]) => x !== undefined)
    w.head(5, entries.length)
    for (const [k, x] of entries) {
      put(w, k)
      put(w, x)
    }
  }
}

export function encode(v: CborValue): Uint8Array {
  const w = new Writer()
  put(w, v)
  return w.result()
}

class Reader {
  pos = 0
  constructor(private readonly b: Uint8Array) {}

  byte(): number {
    if (this.pos >= this.b.length) throw new RangeError('CBOR: truncated')
    return this.b[this.pos++]
  }

  take(n: number): Uint8Array {
    if (this.pos + n > this.b.length) throw new RangeError('CBOR: truncated')
    const out = this.b.subarray(this.pos, this.pos + n)
    this.pos += n
    return out
  }

  // The argument of a head byte; -1 for indefinite length.
  arg(info: number): number {
    if (info < 24) return info
    if (info === 24) return this.byte()
    if (info === 25) return (this.byte() << 8) | this.byte()
    if (info === 26) return ((this.byte() << 24) >>> 0) + ((this.byte() << 16) | (this.byte() << 8) | this.byte())
    if (info === 27) {
      const hi = ((this.byte() << 24) >>> 0) + ((this.byte() << 16) | (this.byte() << 8) | this.byte())
      const lo = ((this.byte() << 24) >>> 0) + ((this.byte() << 16) | (this.byte() << 8) | this.byte())
      return hi * 0x100000000 + lo
    }
    if (info === 31) return -1
    throw new RangeError(`CBOR: bad additional info ${info}`)
  }
}

const BREAK = Symbol('break')

function get(r: Reader): CborValue | typeof BREAK {
  const ib = r.byte()
  const major = ib >> 5
  const info = ib & 0x1f
  if (ib === 0xff) return BREAK
  switch (major) {
    case 0:
      return r.arg(info)
    case 1:
      return -1 - r.arg(info)
    case 2:
    case 3: {
      const n = r.arg(info)
      let bytes: Uint8Array
      if (n < 0) {
        const parts: Uint8Array[] = []
        for (;;) {
          const part = get(r)
          if (part === BREAK) break
          parts.push(typeof part === 'string' ? utf8.encode(part) : (part as Uint8Array))
        }
        bytes = concat(parts)
      } else {
        bytes = r.take(n).slice()
      }
      return major === 2 ? bytes : utf8d.decode(bytes)
    }
    case 4: {
      const n = r.arg(info)
      const out: CborValue[] = []
      for (let i = 0; n < 0 || i < n; i++) {
        const v = get(r)
        if (v === BREAK) break
        out.push(v)
      }
      return out
    }
    case 5: {
      const n = r.arg(info)
      const out: { [key: string]: CborValue } = {}
      for (let i = 0; n < 0 || i < n; i++) {
        const k = get(r)
        if (k === BREAK) break
        const v = get(r)
        if (v === BREAK) throw new RangeError('CBOR: map without value')
        out[String(k)] = v
      }
      return out
    }
    case 6:
      r.arg(info)
      return get(r) // tags are not used by SMP: unwrap
    case 7:
      if (info === 20) return false
      if (info === 21) return true
      if (info === 22) return null
      if (info === 23) return undefined
      if (info === 25 || info === 26 || info === 27) {
        // Floats: SMP sends none, skip them as numbers.
        const n = info === 25 ? 2 : info === 26 ? 4 : 8
        const view = new DataView(r.take(n).slice().buffer)
        return n === 4 ? view.getFloat32(0) : n === 8 ? view.getFloat64(0) : 0
      }
      throw new RangeError(`CBOR: unsupported simple value ${info}`)
  }
  throw new RangeError('CBOR: unreachable')
}

export function decode(b: Uint8Array): CborValue {
  const r = new Reader(b)
  const v = get(r)
  if (v === BREAK) throw new RangeError('CBOR: stray break')
  return v
}

export function concat(parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0))
  let off = 0
  for (const p of parts) {
    out.set(p, off)
    off += p.length
  }
  return out
}
