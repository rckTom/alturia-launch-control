<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { api } from '../api'
import { frames, openTab, run, serverNow, store } from '../store'
import { pdrColor } from '../style'
import type { Entity } from '../types'

const DEMOD_SNR: Record<number, number> = { 7: -7.5, 8: -10, 9: -12.5, 10: -15, 11: -17.5, 12: -20 }

const el = ref<HTMLDivElement>()
const size = reactive({ w: 600, h: 400 })
// World metres to screen: centre (cx, cy) at the middle, s px per metre.
const view = reactive({ cx: -60, cy: 0, s: 1.4 })
const drag = reactive({ id: 0, dx: 0, dy: 0, moved: false, panning: false, px: 0, py: 0 })
const tick = ref(0)

let ro: ResizeObserver | undefined
let timer = 0
onMounted(() => {
  ro = new ResizeObserver(([e]) => {
    size.w = e.contentRect.width
    size.h = e.contentRect.height
  })
  ro.observe(el.value!)
  // Transmit pulses need a clock.
  timer = window.setInterval(() => tick.value++, 50)
})
onBeforeUnmount(() => {
  ro?.disconnect()
  clearInterval(timer)
})

const sx = (x: number) => size.w / 2 + (x - view.cx) * view.s
const sy = (y: number) => size.h / 2 + (y - view.cy) * view.s
const wx = (px: number) => (px - size.w / 2) / view.s + view.cx
const wy = (py: number) => (py - size.h / 2) / view.s + view.cy

const entities = computed(() => [...store.entities.values()])

const grid = computed(() => {
  // A step that lands some 70 px apart.
  const steps = [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000]
  const step = steps.find((s) => s * view.s >= 70) ?? 5000
  const xs: number[] = []
  const ys: number[] = []
  for (let x = Math.ceil(wx(0) / step) * step; x <= wx(size.w); x += step) xs.push(x)
  for (let y = Math.ceil(wy(0) / step) * step; y <= wy(size.h); y += step) ys.push(y)
  return { step, xs, ys }
})

// Who is on air right now.
const transmitting = computed(() => {
  void tick.value
  const now = serverNow()
  const on = new Map<number, string>()
  const list = frames.list
  for (let i = list.length - 1; i >= 0 && i > list.length - 400; i--) {
    const f = list[i]
    if (f.t_start <= now + 20_000 && (f.t_end === null || f.t_end >= now - 30_000))
      on.set(f.src, f.energy ? 'energy' : 'frame')
  }
  return on
})

const links = computed(() => {
  // Worse direction of each pair, from the last 30 s.
  const pairs = new Map<string, { a: Entity; b: Entity; pdr: number; total: number }>()
  for (const l of store.stats?.links ?? []) {
    const a = store.entities.get(l.src)
    const b = store.entities.get(l.dst)
    if (!a || !b || a.kind !== 'node' || b.kind !== 'node') continue
    const key = a.id < b.id ? `${a.id}-${b.id}` : `${b.id}-${a.id}`
    const cur = pairs.get(key)
    if (!cur) pairs.set(key, { a, b, pdr: l.pdr, total: l.total })
    else {
      cur.pdr = Math.min(cur.pdr, l.pdr)
      cur.total += l.total
    }
  }
  return [...pairs.values()]
})

const cutLinks = computed(() =>
  store.links
    .map((l) => ({ ...l, ea: store.entities.get(l.a), eb: store.entities.get(l.b) }))
    .filter((l) => l.ea && l.eb),
)

function power(e: Entity): number {
  if (e.tx_power_override !== null && e.tx_power_override !== undefined) return e.tx_power_override
  if (e.kind === 'device') return e.power_dbm ?? 7
  return e.sf === 8 ? 0 : 7 // pairing profile at 0 dBm
}

// Where a frame from @p e can still be demodulated, without interference.
function rangeM(e: Entity): number | null {
  const rf = store.rf
  if (!rf) return null
  const sf = e.sf ?? 7
  const limit = rf.noise_floor_dbm + (DEMOD_SNR[sf] ?? -7.5)
  return Math.pow(10, (power(e) - rf.pl0_db - limit) / (10 * rf.exponent))
}

const selectedEntity = computed(() =>
  store.selected?.kind === 'entity' ? store.entities.get(store.selected.id) : undefined,
)

function color(e: Entity): string {
  if (e.kind === 'device') {
    return { jammer: '#ef4444', traffic: '#a8a29e', radio: '#c084fc' }[e.device_type!] ?? '#aaa'
  }
  return e.role === 'ctrl' ? '#3b82f6' : e.role === 'sat' ? '#14b8a6' : '#eab308'
}

function alive(e: Entity): boolean {
  return e.kind === 'device' ? !!e.enabled : !!e.connected
}

function select(e: Entity) {
  store.selected = { kind: 'entity', id: e.id }
}

function onEntityDown(ev: PointerEvent, e: Entity) {
  ev.stopPropagation()
  select(e)
  const r = el.value!.getBoundingClientRect()
  drag.id = e.id
  drag.dx = wx(ev.clientX - r.left) - e.x
  drag.dy = wy(ev.clientY - r.top) - e.y
  drag.moved = false
  ;(ev.target as Element).setPointerCapture(ev.pointerId)
}

function onDown(ev: PointerEvent) {
  drag.panning = true
  drag.px = ev.clientX
  drag.py = ev.clientY
  ;(ev.currentTarget as Element).setPointerCapture(ev.pointerId)
}

function onMove(ev: PointerEvent) {
  if (drag.id) {
    const r = el.value!.getBoundingClientRect()
    const e = store.entities.get(drag.id)
    if (!e) return
    e.x = Math.round(wx(ev.clientX - r.left) - drag.dx)
    e.y = Math.round(wy(ev.clientY - r.top) - drag.dy)
    drag.moved = true
  } else if (drag.panning) {
    view.cx -= (ev.clientX - drag.px) / view.s
    view.cy -= (ev.clientY - drag.py) / view.s
    drag.px = ev.clientX
    drag.py = ev.clientY
  }
}

function onUp() {
  if (drag.id && drag.moved) {
    const e = store.entities.get(drag.id)
    if (e) run(api.patchEntity(e.id, { x: e.x, y: e.y }))
  }
  drag.id = 0
  drag.panning = false
}

function onWheel(ev: WheelEvent) {
  const r = el.value!.getBoundingClientRect()
  const px = ev.clientX - r.left
  const py = ev.clientY - r.top
  const x0 = wx(px)
  const y0 = wy(py)
  view.s = Math.min(50, Math.max(0.02, view.s * Math.exp(-ev.deltaY * 0.0015)))
  // Keep the point under the cursor in place.
  view.cx = x0 - (px - size.w / 2) / view.s
  view.cy = y0 - (py - size.h / 2) / view.s
}

function fit() {
  const es = entities.value
  if (!es.length) return
  const xs = es.map((e) => e.x)
  const ys = es.map((e) => e.y)
  const [x0, x1, y0, y1] = [Math.min(...xs), Math.max(...xs), Math.min(...ys), Math.max(...ys)]
  view.cx = (x0 + x1) / 2
  view.cy = (y0 + y1) / 2
  view.s = Math.min(20, Math.max(0.02, Math.min(size.w / (x1 - x0 + 120), size.h / (y1 - y0 + 120))))
}

function openDefault(e: Entity) {
  if (e.kind !== 'node') return
  openTab({ type: e.role === 'ctrl' && e.has_display ? 'screen' : 'shell', id: e.id })
}

const icon = (e: Entity) =>
  e.kind === 'device'
    ? e.device_type === 'jammer'
      ? 'M0,-11 L10,7 L-10,7 Z'
      : e.device_type === 'traffic'
        ? 'M0,-10 L10,0 L0,10 L-10,0 Z'
        : 'M-9,-5 L0,-10 L9,-5 L9,5 L0,10 L-9,5 Z'
    : e.role === 'ctrl'
      ? 'M-10,-8 h20 v16 h-20 Z'
      : ''
</script>

<template>
  <div ref="el" class="map" @wheel.prevent="onWheel">
    <svg :width="size.w" :height="size.h" @pointerdown="onDown" @pointermove="onMove" @pointerup="onUp"
      @click.self="store.selected = null">
      <g class="grid">
        <line v-for="x in grid.xs" :key="'x' + x" :x1="sx(x)" :x2="sx(x)" y1="0" :y2="size.h" />
        <line v-for="y in grid.ys" :key="'y' + y" :y1="sy(y)" :y2="sy(y)" x1="0" :x2="size.w" />
      </g>

      <circle v-if="selectedEntity && rangeM(selectedEntity)" class="range" :cx="sx(selectedEntity.x)"
        :cy="sy(selectedEntity.y)" :r="rangeM(selectedEntity)! * view.s" />

      <line v-for="l in links" :key="`${l.a.id}-${l.b.id}`" :x1="sx(l.a.x)" :y1="sy(l.a.y)" :x2="sx(l.b.x)"
        :y2="sy(l.b.y)" :stroke="pdrColor(l.pdr)" stroke-width="2" stroke-opacity="0.55">
        <title>{{ l.a.name }} ↔ {{ l.b.name }}: {{ Math.round(l.pdr * 100) }} % of {{ l.total }} frames</title>
      </line>
      <line v-for="l in cutLinks" :key="`c${l.a}-${l.b}`" :x1="sx(l.ea!.x)" :y1="sy(l.ea!.y)" :x2="sx(l.eb!.x)"
        :y2="sy(l.eb!.y)" :stroke="l.cut ? '#ef4444' : '#eab308'" stroke-width="1.5" stroke-dasharray="4 4" />

      <g v-for="e in entities" :key="e.id" :transform="`translate(${sx(e.x)},${sy(e.y)})`" class="ent"
        :class="{ dead: !alive(e), sel: store.selected?.kind === 'entity' && store.selected.id === e.id }"
        @pointerdown="onEntityDown($event, e)" @dblclick="openDefault(e)">
        <circle v-if="transmitting.has(e.id)" r="18" class="pulse"
          :class="transmitting.get(e.id)" :stroke="color(e)" />
        <path v-if="icon(e)" :d="icon(e)" :fill="color(e)" />
        <circle v-else r="9" :fill="color(e)" />
        <text y="24" text-anchor="middle">{{ e.name }}</text>
        <text v-if="e.kind === 'node' && e.sf === 8" y="-14" text-anchor="middle" class="tag">pairing</text>
        <text v-if="e.isolated" y="-14" text-anchor="middle" class="tag off">radio off</text>
      </g>
    </svg>
    <div class="overlay">
      <span class="scale"><span :style="{ width: grid.step * view.s + 'px' }" />{{ grid.step }} m</span>
      <button title="Show everything" @click="fit">Fit</button>
    </div>
    <div v-if="!entities.length" class="empty muted">
      Add a controller and satellites from the toolbar. Drag to move, wheel to zoom, double-click for a
      node's shell or screen.
    </div>
  </div>
</template>

<style scoped>
.map {
  position: relative;
  background: #0b0f14;
  overflow: hidden;
  min-height: 0;
  user-select: none;
}
svg {
  display: block;
  cursor: grab;
}
.grid line {
  stroke: #18212c;
  stroke-width: 1;
}
.range {
  fill: rgba(59, 130, 246, 0.05);
  stroke: rgba(59, 130, 246, 0.5);
  stroke-dasharray: 6 5;
}
.ent {
  cursor: pointer;
}
.ent text {
  fill: var(--text);
  font-size: 11px;
  paint-order: stroke;
  stroke: #0b0f14;
  stroke-width: 3px;
}
.ent .tag {
  fill: #f59e0b;
  font-size: 9px;
}
.ent .tag.off {
  fill: #ef4444;
}
.ent.dead path,
.ent.dead > circle:not(.pulse) {
  opacity: 0.35;
}
.ent.sel path,
.ent.sel > circle:not(.pulse) {
  stroke: white;
  stroke-width: 2;
}
.pulse {
  fill: none;
  stroke-width: 3;
  opacity: 0.8;
}
.pulse.energy {
  stroke-dasharray: 3 3;
}
.overlay {
  position: absolute;
  right: 8px;
  bottom: 8px;
  display: flex;
  gap: 10px;
  align-items: center;
}
.scale {
  font-size: 11px;
  color: var(--muted);
  display: flex;
  align-items: center;
  gap: 6px;
}
.scale span {
  display: inline-block;
  height: 6px;
  border: 1px solid var(--muted);
  border-top: none;
}
.empty {
  position: absolute;
  top: 45%;
  left: 0;
  right: 0;
  text-align: center;
  pointer-events: none;
  padding: 0 30px;
}
</style>
