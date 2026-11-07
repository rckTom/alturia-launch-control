// SPDX-License-Identifier: GPL-3.0-only
//
// Reads what the tool needs from a signed MCUboot image (zephyr.signed.bin):
// the version from the header and the image hash from the TLV area, which
// is what img_mgmt reports for the slots. With both, a file can be matched
// against what the device runs before anything is uploaded.

const IMAGE_MAGIC = 0x96f3b83d
const TLV_INFO_MAGIC = 0x6907
const TLV_PROT_INFO_MAGIC = 0x6908
const TLV_SHA256 = 0x10
const TLV_SHA384 = 0x11
const TLV_SHA512 = 0x12
// Custom TLV with the board name (firmware/*/sysbuild.cmake), protected
const TLV_BOARD = 0xa0

export interface McubootImage {
  version: string
  hash: Uint8Array
  size: number
  /** Board the image was built for, e.g. "alturia_satellite"; null if not tagged. */
  board: string | null
}

export class ImageError extends Error {}

export function parseImage(data: Uint8Array): McubootImage {
  if (data.length < 32) throw new ImageError('too short for an MCUboot image')
  const v = new DataView(data.buffer, data.byteOffset, data.byteLength)
  if (v.getUint32(0, true) !== IMAGE_MAGIC) throw new ImageError('not a signed MCUboot image (bad magic)')
  const hdrSize = v.getUint16(8, true)
  const protSize = v.getUint16(10, true)
  const imgSize = v.getUint32(12, true)
  const major = v.getUint8(20)
  const minor = v.getUint8(21)
  const revision = v.getUint16(22, true)
  const build = v.getUint32(24, true)
  // Same format as img_mgmt_ver_str()
  const version = `${major}.${minor}.${revision}` + (build ? `.${build}` : '')

  let off = hdrSize + imgSize
  let board: string | null = null
  if (protSize > 0) {
    if (off + 4 > data.length || v.getUint16(off, true) !== TLV_PROT_INFO_MAGIC) {
      throw new ImageError('protected TLV area missing')
    }
    const pend = Math.min(off + protSize, data.length)
    for (let p = off + 4; p + 4 <= pend; ) {
      const type = v.getUint16(p, true)
      const len = v.getUint16(p + 2, true)
      if (type === TLV_BOARD) board = new TextDecoder().decode(data.subarray(p + 4, p + 4 + len))
      p += 4 + len
    }
    off += protSize
  }
  if (off + 4 > data.length || v.getUint16(off, true) !== TLV_INFO_MAGIC) {
    throw new ImageError('TLV area missing: is the image signed?')
  }
  const end = off + v.getUint16(off + 2, true)
  off += 4
  while (off + 4 <= end && end <= data.length) {
    const type = v.getUint16(off, true)
    const len = v.getUint16(off + 2, true)
    if (type === TLV_SHA256 || type === TLV_SHA384 || type === TLV_SHA512) {
      return { version, hash: data.slice(off + 4, off + 4 + len), size: data.length, board }
    }
    off += 4 + len
  }
  throw new ImageError('no image hash in the TLV area')
}

export function hex(b: Uint8Array, n = b.length): string {
  return Array.from(b.subarray(0, n), (x) => x.toString(16).padStart(2, '0')).join('')
}

export function sameBytes(a: Uint8Array, b: Uint8Array): boolean {
  return a.length === b.length && a.every((x, i) => x === b[i])
}

/** Compare dotted versions numerically: <0, 0, >0. */
export function compareVersions(a: string, b: string): number {
  const pa = a.split('.').map(Number)
  const pb = b.split('.').map(Number)
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] ?? 0) - (pb[i] ?? 0)
    if (d) return d
  }
  return 0
}
