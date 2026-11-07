// SPDX-License-Identifier: GPL-3.0-only
//
// Collects signed images for the tool to offer: copies them to
// <out>/firmware/<board>-<version>.bin and writes <out>/firmware/manifest.json.
//
//   npm run firmware                       local builds (firmware/*/build-dfu)
//                                          into public/, for `npm run dev`
//   npm run firmware -- --out dist a.bin b.bin
//
// The board comes from the image's own TLV 0xa0, so the files can be
// given in any order.

import { copyFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs'
import { join, resolve } from 'node:path'
import { execSync } from 'node:child_process'
import { hex, parseImage } from '../src/smp/mcuboot'

const args = process.argv.slice(2)
let out = 'public'
const files: string[] = []
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--out') out = args[++i]
  else files.push(args[i])
}
if (!files.length) {
  for (const app of ['controller', 'satellite']) {
    const f = resolve(`../../firmware/${app}/build-dfu/${app}/zephyr/zephyr.signed.bin`)
    if (existsSync(f)) files.push(f)
  }
}

let commit = ''
try {
  commit = execSync('git rev-parse --short HEAD', { encoding: 'utf8' }).trim()
} catch {
  /* not a checkout */
}

const dir = join(out, 'firmware')
mkdirSync(dir, { recursive: true })
const images = files.map((f) => {
  const img = parseImage(new Uint8Array(readFileSync(f)))
  if (!img.board) throw new Error(`${f}: no board TLV (0xa0); build it with sysbuild`)
  const file = `${img.board}-${img.version}.bin`
  copyFileSync(f, join(dir, file))
  console.log(`${img.board} ${img.version} ${hex(img.hash, 8)}  ${f}`)
  return { board: img.board, version: img.version, hash: hex(img.hash), size: img.size, file }
})
writeFileSync(
  join(dir, 'manifest.json'),
  JSON.stringify({ generated: new Date().toISOString(), commit, images }, null, 2) + '\n',
)
console.log(`wrote ${join(dir, 'manifest.json')}`)
