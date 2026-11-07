// SPDX-License-Identifier: GPL-3.0-only
//
// The tool's state: one device on one port, what it runs, what is
// published for it, and the update sequence
//
//   upload to slot 1 -> mark for test -> reset -> MCUboot swaps ->
//   reconnect -> the app confirms itself after a few seconds
//
// If the new image does not come up, MCUboot swaps the old one back at the
// next reset and the tool reports that.

import { reactive } from 'vue'
import { Link, USB_FILTERS, serialSupported } from './link'
import { SmpError, type ImageSlot, uploadChunk } from './smp/smp'
import { compareVersions, hex, ImageError, parseImage, type McubootImage } from './smp/mcuboot'

export type Phase = 'unsupported' | 'idle' | 'connecting' | 'shell' | 'ready' | 'busy' | 'rebooting'
export type Kind = 'controller' | 'satellite' | 'other'

export interface Published {
  board: string
  version: string
  hash: string
  size: number
  file: string
}

interface Manifest {
  generated?: string
  commit?: string
  images: Published[]
}

export interface Candidate {
  name: string
  image: McubootImage
  data: Uint8Array
  published: boolean
}

export interface Progress {
  stage: string
  done: number
  total: number
}

export const BOARD_NAMES: Record<string, string> = {
  alturia_controller: 'Controller',
  alturia_satellite: 'Satellite',
}

export const state = reactive({
  phase: (serialSupported ? 'idle' : 'unsupported') as Phase,
  /** Board target as the device reports it, e.g. alturia_satellite/stm32g474xx */
  target: '',
  bootloader: '',
  bufSize: 0,
  slots: [] as ImageSlot[],
  manifest: null as Manifest | null,
  manifestNote: '',
  local: null as Candidate | null,
  progress: null as Progress | null,
  message: null as { text: string; error: boolean } | null,
})

/** Board name without qualifiers: alturia_satellite */
export function boardName(): string {
  return state.target.split('/')[0].split('@')[0]
}

export function kind(): Kind {
  const b = boardName()
  return b === 'alturia_controller' ? 'controller' : b === 'alturia_satellite' ? 'satellite' : 'other'
}

export function activeSlot(): ImageSlot | undefined {
  return state.slots.find((s) => s.active) ?? state.slots.find((s) => s.slot === 0)
}

export function published(): Published | undefined {
  return state.manifest?.images.find((i) => i.board === boardName())
}

/** The other slot, if it holds an image that could be booted again. */
export function fallbackSlot(): ImageSlot | undefined {
  const a = activeSlot()
  return state.slots.find((s) => s !== a && s.bootable && s.hash.length > 0)
}

let link: Link | undefined
let usbInfo: SerialPortInfo | undefined
let terminalSink: (bytes: Uint8Array) => void = () => {}
let cancel: AbortController | undefined

export function onTerminalText(sink: (bytes: Uint8Array) => void) {
  terminalSink = sink
}

function note(text: string, error = false) {
  state.message = { text, error }
}

function termNote(text: string) {
  terminalSink(new TextEncoder().encode(`\r\n\x1b[7m ${text} \x1b[0m\r\n`))
}

export function typeText(s: string) {
  if (link) void link.writeText(s).catch(() => {})
}

/** The open port's SMP client, for other mcumgr groups (the ignition log). */
export function smpClient() {
  return link?.smp
}

// ---- published firmware ----

export async function loadManifest() {
  try {
    const r = await fetch('firmware/manifest.json', { cache: 'no-cache' })
    if (!r.ok) throw new Error(String(r.status))
    state.manifest = (await r.json()) as Manifest
    state.manifestNote = ''
  } catch {
    state.manifest = { images: [] }
    state.manifestNote = 'No published firmware here. Load an image file instead.'
  }
}

async function fetchPublished(p: Published): Promise<Candidate> {
  const r = await fetch(`firmware/${p.file}`)
  if (!r.ok) throw new Error(`firmware/${p.file}: ${r.status}`)
  const data = new Uint8Array(await r.arrayBuffer())
  const image = parseImage(data)
  if (hex(image.hash) !== p.hash) throw new Error(`firmware/${p.file} does not match the manifest`)
  return { name: p.file, image, data, published: true }
}

export async function loadFile(file: File) {
  try {
    const data = new Uint8Array(await file.arrayBuffer())
    const image = parseImage(data)
    state.local = { name: file.name, image, data, published: false }
    note(`${file.name}: ${image.board ? (BOARD_NAMES[image.board] ?? image.board) : 'untagged'} firmware ${image.version}`)
  } catch (e) {
    state.local = null
    note(e instanceof ImageError ? `${file.name}: ${e.message}` : String(e), true)
  }
}

// ---- connection ----

export async function connect() {
  let port: SerialPort
  try {
    port = await navigator.serial.requestPort({ filters: USB_FILTERS })
  } catch {
    return // chooser closed
  }
  await openPort(port)
}

async function openPort(port: SerialPort): Promise<boolean> {
  state.phase = 'connecting'
  try {
    link = await Link.open(port, {
      text: (t) => terminalSink(t),
      closed: (why) => onClosed(why),
    })
  } catch (e) {
    state.phase = 'idle'
    note(`Could not open the port: ${(e as Error).message}. Is another program using it?`, true)
    return false
  }
  usbInfo = port.getInfo()
  await probe()
  return true
}

/** Ask for mcumgr; without it the port is still a terminal. */
async function probe() {
  const l = link
  if (!l) return
  let ok = false
  for (let i = 0; i < 3 && !ok; i++) {
    try {
      await l.smp.echo('alturia')
      ok = true
    } catch {
      /* the shell may still be starting */
    }
  }
  if (link !== l) return
  if (!ok) {
    state.phase = 'shell'
    state.target = ''
    state.slots = []
    termNote('terminal only: no mcumgr answer')
    note('The terminal works, but this firmware does not answer mcumgr: it predates MCUboot. Flash it once with west.')
    return
  }
  state.target = await l.smp.board().catch(() => '')
  state.bootloader = await l.smp.bootloader().catch(() => '')
  state.bufSize = (await l.smp.params().catch(() => ({ bufSize: 0 }))).bufSize
  state.slots = await l.smp.images().catch(() => [])
  state.phase = 'ready'
  const a = activeSlot()
  termNote(`${BOARD_NAMES[boardName()] ?? state.target} ${a?.version ?? ''}`.trim())
  if (state.message?.error !== true) state.message = null
}

export async function refresh() {
  if (!link || state.phase !== 'ready') return
  try {
    state.slots = await link.smp.images()
  } catch (e) {
    note(String((e as Error).message), true)
  }
}

export async function disconnect() {
  cancel?.abort()
  const l = link
  link = undefined
  await l?.close()
  state.phase = serialSupported ? 'idle' : 'unsupported'
  state.target = ''
  state.slots = []
  state.progress = null
}

let rebootWaiter: ((closed: boolean) => void) | undefined

function onClosed(why: string) {
  link = undefined
  if (state.phase === 'rebooting') {
    rebootWaiter?.(true)
    return
  }
  state.phase = serialSupported ? 'idle' : 'unsupported'
  state.slots = []
  state.progress = null
  termNote('disconnected')
  if (why !== 'closed') note('The device went away.')
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))

/** After a reset: wait for the port to drop and come back, then open it. */
async function reconnectAfterReset(timeoutMs: number): Promise<boolean> {
  if (link) {
    // The port should drop within a moment of the reset.
    const dropped = await Promise.race([
      new Promise<boolean>((resolve) => (rebootWaiter = resolve)),
      sleep(8000).then(() => false),
    ])
    rebootWaiter = undefined
    // The controller's USB Serial/JTAG is part of the chip's hardware and
    // may stay up through the reset: then the open port just carries on.
    if (!dropped && link) return true
  }
  const until = Date.now() + timeoutMs
  while (Date.now() < until) {
    await sleep(1000)
    const ports = await navigator.serial.getPorts()
    const port = ports.find((p) => {
      const i = p.getInfo()
      return i.usbVendorId === usbInfo?.usbVendorId && i.usbProductId === usbInfo?.usbProductId
    })
    if (!port) continue
    try {
      state.phase = 'rebooting'
      link = await Link.open(port, { text: (t) => terminalSink(t), closed: (why) => onClosed(why) })
      return true
    } catch {
      link = undefined // not ready yet: try again
    }
  }
  return false
}

// ---- update ----

function explain(e: unknown): string {
  if (e instanceof SmpError && e.rc === 10 && e.group === undefined) {
    return 'Refused: something is armed. Disarm, then try again.'
  }
  return (e as Error).message || String(e)
}

export async function updatePublished() {
  const p = published()
  if (!p) return
  state.phase = 'busy'
  state.progress = { stage: 'Fetching', done: 0, total: 1 }
  let c: Candidate
  try {
    c = await fetchPublished(p)
  } catch (e) {
    state.phase = 'ready'
    state.progress = null
    note(explain(e), true)
    return
  }
  await install(c)
}

export async function updateLocal() {
  if (state.local) await install(state.local)
}

async function install(c: Candidate) {
  const l = link
  if (!l) return
  const board = boardName()
  if (c.image.board && c.image.board !== board) {
    note(`${c.name} is for the ${BOARD_NAMES[c.image.board] ?? c.image.board}; this is the ${BOARD_NAMES[board] ?? board}.`, true)
    state.phase = 'ready'
    state.progress = null
    return
  }
  if (!c.image.board && !confirm(`${c.name} does not say which board it is for. Upload it anyway?`)) {
    state.phase = 'ready'
    state.progress = null
    return
  }
  const a = activeSlot()
  if (a && hex(a.hash) === hex(c.image.hash)) {
    note(`The device already runs ${c.image.version}.`)
    state.phase = 'ready'
    state.progress = null
    return
  }

  state.phase = 'busy'
  cancel = new AbortController()
  const total = c.data.length
  try {
    state.progress = { stage: 'Erasing slot 1', done: 0, total }
    const sha = new Uint8Array(await crypto.subtle.digest('SHA-256', c.data))
    await l.smp.upload(
      c.data,
      sha,
      uploadChunk(state.bufSize),
      (done) => (state.progress = { stage: 'Uploading', done, total }),
      cancel.signal,
    )
    state.progress = { stage: 'Marking for test', done: total, total }
    state.slots = await l.smp.setImageState(c.image.hash, false)
  } catch (e) {
    state.phase = link ? 'ready' : 'idle'
    state.progress = null
    note(explain(e), true)
    await refresh()
    return
  }
  await restartInto(c.image.hash, c.image.version, total)
}

/** Boot the image in the other slot again: the one that ran before. */
export async function rollBack() {
  const f = fallbackSlot()
  if (!link || !f) return
  state.phase = 'busy'
  try {
    state.slots = await link.smp.setImageState(f.hash, false)
  } catch (e) {
    state.phase = 'ready'
    note(explain(e), true)
    return
  }
  await restartInto(f.hash, f.version, 1)
}

async function restartInto(hash: Uint8Array, version: string, total: number) {
  const l = link
  if (!l) return
  state.progress = { stage: 'Restarting', done: total, total }
  // Before the request: the port may drop before the answer arrives.
  state.phase = 'rebooting'
  try {
    await l.smp.reset()
  } catch (e) {
    if (e instanceof SmpError) {
      state.phase = 'ready'
      state.progress = null
      note(explain(e), true)
      await refresh()
      return
    }
    // No answer: the reset may have beaten the reply.
  }
  state.progress = { stage: 'MCUboot swapping', done: total, total }
  termNote('restarting into the new image')
  if (!(await reconnectAfterReset(120000))) {
    state.phase = 'idle'
    state.progress = null
    note('The device did not come back on USB. Reconnect it by hand.', true)
    return
  }
  // The controller's USB Serial/JTAG port can stay up through the reset,
  // so the port may be open while MCUboot still swaps (some 15 s): keep
  // asking until the app answers.
  state.progress = { stage: 'Starting', done: total, total }
  const answerBy = Date.now() + 90000
  while (link && Date.now() < answerBy) {
    try {
      await link.smp.echo('alturia')
      break
    } catch {
      await sleep(1000)
    }
  }
  await probe()
  if ((state.phase as Phase) !== 'ready') {
    state.progress = null
    note('The device is back but does not answer mcumgr.', true)
    return
  }

  const want = hex(hash)
  const until = Date.now() + 60000
  for (;;) {
    const a = activeSlot()
    if (!a || hex(a.hash) !== want) {
      state.progress = null
      note(`The device came back with ${a?.version ?? 'unknown'}: ${version} did not start, MCUboot went back.`, true)
      return
    }
    if (a.confirmed) break
    if (Date.now() > until) {
      state.progress = null
      note(`${version} runs on test but has not confirmed itself; the next reset goes back to the old image.`, true)
      return
    }
    state.progress = { stage: 'Waiting for the app to confirm', done: total, total }
    await sleep(2000)
    await refresh()
  }
  state.progress = null
  note(`Running ${version}, confirmed.`)
}

export function cancelUpload() {
  cancel?.abort()
}

/**
 * The published image against the active one. Only a newer one is offered
 * as the update; an older or same-version one has to be chosen as a file.
 */
export function publishedStatus(): 'none' | 'current' | 'newer' | 'older' | 'different' {
  const p = published()
  const a = activeSlot()
  if (!p || !a) return 'none'
  if (p.hash === hex(a.hash)) return 'current'
  const d = compareVersions(p.version, a.version)
  return d > 0 ? 'newer' : d < 0 ? 'older' : 'different'
}

export function init() {
  void loadManifest()
  if (!serialSupported) return
  // A device that was granted before comes back on its own after a replug.
  navigator.serial.addEventListener('connect', (ev) => {
    if (state.phase === 'idle' && ev.target) void openPort(ev.target as SerialPort)
  })
}
