import { reactive, shallowReactive } from 'vue'
import type { DockTab, Entity, Frame, Link, Rf, Stats } from './types'

const MAX_FRAMES = 20000

export const store = reactive({
  connected: false,
  entities: new Map<number, Entity>(),
  links: [] as Link[],
  rf: null as Rf | null,
  stats: null as Stats | null,
  canSpawn: { ctrl: false, sat: false } as Record<string, boolean>,
  workdir: '',
  selected: null as { kind: 'entity' | 'frame'; id: number } | null,
  dock: [] as DockTab[],
  dockActive: 0,
  dockSplit: -1, // index of the tab shown on the right, -1 for none
  toast: '' as string,
  // Bumped on every frame change, for the canvas to redraw.
  frameVersion: 0,
})

// Frames are many and change often: kept out of deep reactivity.
export const frames = shallowReactive({ list: [] as Frame[], byId: new Map<number, Frame>() })

// Server time (microseconds since it started) at performance.now(), to
// place "now" on the timeline between messages.
let clockBase = { server: 0, perf: 0 }

export function serverNow(): number {
  return clockBase.server + (performance.now() - clockBase.perf) * 1000
}

function syncClock(now: number | undefined) {
  if (now === undefined) return
  const est = serverNow()
  // Messages only arrive late: move forward at once, back slowly.
  if (clockBase.perf === 0 || now > est || est - now > 200_000) {
    clockBase = { server: now, perf: performance.now() }
  }
}

function addFrame(f: Frame) {
  frames.list.push(f)
  frames.byId.set(f.id, f)
  if (frames.list.length > MAX_FRAMES) {
    const drop = frames.list.splice(0, frames.list.length - MAX_FRAMES)
    for (const d of drop) frames.byId.delete(d.id)
  }
}

let toastTimer = 0
export function toast(msg: string) {
  store.toast = msg
  clearTimeout(toastTimer)
  toastTimer = window.setTimeout(() => (store.toast = ''), 4000)
}

export async function run<T>(p: Promise<T>): Promise<T | undefined> {
  try {
    return await p
  } catch (e) {
    toast(String((e as Error).message ?? e))
    return undefined
  }
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function handle(ev: any) {
  switch (ev.type) {
    case 'snapshot':
      store.entities = new Map(ev.entities.map((e: Entity) => [e.id, e]))
      store.links = ev.links
      store.rf = ev.rf
      store.canSpawn = ev.can_spawn
      store.workdir = ev.workdir
      frames.list = []
      frames.byId = new Map()
      for (const f of ev.frames) addFrame(f)
      syncClock(ev.now)
      store.frameVersion++
      break
    case 'entity':
      store.entities.set(ev.entity.id, { ...store.entities.get(ev.entity.id), ...ev.entity })
      break
    case 'entity_removed':
      store.entities.delete(ev.id)
      store.dock = store.dock.filter((t) => t.id !== ev.id)
      if (store.selected?.kind === 'entity' && store.selected.id === ev.id) store.selected = null
      break
    case 'leds': {
      const e = store.entities.get(ev.id)
      if (e) e.leds = ev.leds
      break
    }
    case 'links':
      store.links = ev.links
      break
    case 'rf':
      store.rf = ev.rf
      break
    case 'stats':
      store.stats = ev
      syncClock(ev.now)
      break
    case 'frame':
      addFrame(ev.frame)
      syncClock(ev.frame.t_start)
      store.frameVersion++
      break
    case 'frame_result': {
      const f = frames.byId.get(ev.id)
      if (f) f.results = ev.results
      store.frameVersion++
      break
    }
    case 'frame_end': {
      const f = frames.byId.get(ev.id)
      if (f) f.t_end = ev.t_end
      store.frameVersion++
      break
    }
  }
}

export function wsUrl(path: string): string {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
  return `${proto}//${location.host}${path}`
}

export function connect() {
  let delay = 500
  const open = () => {
    const ws = new WebSocket(wsUrl('/ws'))
    ws.onopen = () => {
      store.connected = true
      delay = 500
    }
    ws.onmessage = (m) => handle(JSON.parse(m.data))
    ws.onclose = () => {
      store.connected = false
      setTimeout(open, delay)
      delay = Math.min(delay * 2, 5000)
    }
  }
  open()
}

export function openTab(tab: DockTab) {
  let i = store.dock.findIndex((t) => t.type === tab.type && t.id === tab.id)
  if (i < 0) {
    store.dock.push(tab)
    i = store.dock.length - 1
  }
  store.dockActive = i
}

// A link frame's src/dst: 0 is the controller, 255 the group, else a satellite slot.
export function slotName(n: number | undefined): string {
  if (n === undefined) return ''
  if (n === 0) return 'controller'
  if (n === 255) return 'group'
  return `slot ${n} (${String.fromCharCode(64 + n)})`
}

export function nodeName(id: number): string {
  return store.entities.get(id)?.name ?? `#${id}`
}
