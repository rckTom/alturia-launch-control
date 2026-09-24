<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import { frames, nodeName, serverNow, slotName, store } from '../store'
import { frameColor, outcomeColor, outcomeLabel } from '../style'
import type { Entity, Frame } from '../types'

const LABEL_W = 96
const LANE_H = 24
const AXIS_H = 18

const wrap = ref<HTMLDivElement>()
const canvas = ref<HTMLCanvasElement>()
const size = reactive({ w: 800, h: 200 })
// The window shown: `span` microseconds ending at now (live) or at `end`.
const view = reactive({ span: 2_000_000, end: 0, live: true, scroll: 0 })
const hover = reactive({ x: 0, y: 0, frame: null as Frame | null, rx: null as string | null })
const drag = reactive({ on: false, x: 0, moved: false })

const lanes = computed<Entity[]>(() => {
  const order = (e: Entity) =>
    e.kind === 'node' ? (e.role === 'ctrl' ? 0 : e.role === 'sat' ? 1 : 2) : 3
  return [...store.entities.values()].sort((a, b) => order(a) - order(b) || a.name.localeCompare(b.name))
})
const laneIndex = computed(() => new Map(lanes.value.map((e, i) => [e.id, i])))

function t1(): number {
  return view.live ? serverNow() : view.end
}

const tx = (t: number, t0: number) => LABEL_W + ((t - t0) / view.span) * (size.w - LABEL_W)
const laneY = (i: number) => AXIS_H + i * LANE_H - view.scroll

function fmtSpan(us: number): string {
  return us >= 1e6 ? `${+(us / 1e6).toFixed(2)} s` : `${+(us / 1e3).toFixed(1)} ms`
}

function tickStep(): number {
  const target = (view.span * 90) / Math.max(1, size.w - LABEL_W)
  const steps = [100, 200, 500, 1e3, 2e3, 5e3, 1e4, 2e4, 5e4, 1e5, 2e5, 5e5, 1e6, 2e6, 5e6, 1e7, 3e7, 6e7]
  return steps.find((s) => s >= target) ?? 6e7
}

// The frames that may be visible, newest last. The list is in start order.
function visible(t0: number, tEnd: number): Frame[] {
  const list = frames.list
  const out: Frame[] = []
  for (let i = list.length - 1; i >= 0; i--) {
    const f = list[i]
    if (f.t_start > tEnd) continue
    if (f.t_start < t0 - 120e6 && !f.energy) break
    const end = f.t_end ?? tEnd
    if (end >= t0) out.push(f)
  }
  return out.reverse()
}

function draw() {
  const c = canvas.value
  if (!c) return
  const dpr = window.devicePixelRatio || 1
  if (c.width !== Math.round(size.w * dpr) || c.height !== Math.round(size.h * dpr)) {
    c.width = Math.round(size.w * dpr)
    c.height = Math.round(size.h * dpr)
  }
  const g = c.getContext('2d')!
  g.setTransform(dpr, 0, 0, dpr, 0, 0)
  g.fillStyle = '#0b0f14'
  g.fillRect(0, 0, size.w, size.h)

  const tEnd = t1()
  const t0 = tEnd - view.span

  // Lanes
  g.font = '11px system-ui, sans-serif'
  g.textBaseline = 'middle'
  lanes.value.forEach((_, i) => {
    const y = laneY(i)
    if (y + LANE_H < AXIS_H || y > size.h) return
    g.fillStyle = i % 2 ? '#0f141b' : '#0b0f14'
    g.fillRect(0, y, size.w, LANE_H)
  })

  // Time grid
  const step = tickStep()
  g.strokeStyle = '#18212c'
  g.fillStyle = '#6b7785'
  g.textAlign = 'left'
  for (let t = Math.ceil(t0 / step) * step; t <= tEnd; t += step) {
    const x = tx(t, t0)
    g.beginPath()
    g.moveTo(x + 0.5, AXIS_H)
    g.lineTo(x + 0.5, size.h)
    g.stroke()
    const label = step >= 1e6 ? `${(t / 1e6).toFixed(0)} s` : step >= 1e3 ? `${(t / 1e6).toFixed(3)}` : `${(t / 1e6).toFixed(4)}`
    g.fillText(label, x + 3, AXIS_H / 2)
  }

  const sel = store.selected?.kind === 'frame' ? store.selected.id : -1
  const xMin = LABEL_W

  for (const f of visible(t0, tEnd)) {
    const li = laneIndex.value.get(f.src)
    const xs = Math.max(xMin, tx(f.t_start, t0))
    const xe = Math.max(xs + 1.5, tx(f.t_end ?? tEnd, t0))
    if (xe < xMin) continue

    // Reception traces in the receivers' lanes
    if (f.results) {
      for (const r of f.results) {
        if (r.outcome === 'offline' || r.outcome === 'wrong_sf') continue
        const ri = laneIndex.value.get(r.node)
        if (ri === undefined) continue
        const y = laneY(ri)
        g.fillStyle = outcomeColor[r.outcome]
        g.globalAlpha = r.outcome === 'ok' ? 0.9 : 0.8
        g.fillRect(xs, y + LANE_H - 7, xe - xs, 4)
        g.globalAlpha = 1
      }
    }

    if (li === undefined) continue
    const y = laneY(li)
    if (y + LANE_H < AXIS_H || y > size.h) continue
    const lost = f.results?.some((r) => r.outcome === 'collision' || r.outcome === 'jammed')
    g.fillStyle = frameColor(f.info.kind, f.energy)
    g.globalAlpha = f.energy ? 0.35 : f.results ? 0.95 : 0.6
    g.fillRect(xs, y + 3, xe - xs, LANE_H - 12)
    g.globalAlpha = 1
    if (lost) {
      g.strokeStyle = '#ef4444'
      g.lineWidth = 1.5
      g.strokeRect(xs + 0.75, y + 3.75, xe - xs - 1.5, LANE_H - 13.5)
      g.lineWidth = 1
    }
    if (f.id === sel) {
      g.strokeStyle = 'white'
      g.lineWidth = 2
      g.strokeRect(xs - 1, y + 2, xe - xs + 2, LANE_H - 10)
      g.lineWidth = 1
    }
    if (xe - xs > 34 && !f.energy) {
      g.save()
      g.beginPath()
      g.rect(xs, y, xe - xs, LANE_H)
      g.clip()
      g.fillStyle = '#0b0f14'
      g.font = '10px system-ui, sans-serif'
      g.fillText(f.info.kind ?? '', xs + 3, y + LANE_H / 2 - 3)
      g.restore()
    }
  }

  // Labels on top
  g.fillStyle = '#10161e'
  g.fillRect(0, AXIS_H, LABEL_W, size.h)
  g.font = '11px system-ui, sans-serif'
  lanes.value.forEach((e, i) => {
    const y = laneY(i)
    if (y + LANE_H < AXIS_H || y > size.h) return
    const up = e.kind === 'device' ? e.enabled : e.connected
    g.fillStyle = up ? '#d6dde6' : '#5b6673'
    g.textAlign = 'left'
    g.fillText(e.name, 8, y + LANE_H / 2 - 2)
  })

  if (view.live) {
    g.strokeStyle = 'rgba(255,255,255,0.35)'
    g.beginPath()
    g.moveTo(size.w - 0.5, AXIS_H)
    g.lineTo(size.w - 0.5, size.h)
    g.stroke()
  }
}

let raf = 0
function loop() {
  draw()
  raf = requestAnimationFrame(loop)
}

let ro: ResizeObserver | undefined
onMounted(() => {
  ro = new ResizeObserver(([e]) => {
    size.w = e.contentRect.width
    size.h = e.contentRect.height
  })
  ro.observe(wrap.value!)
  loop()
})
onBeforeUnmount(() => {
  cancelAnimationFrame(raf)
  ro?.disconnect()
})
watch(
  () => lanes.value.length,
  () => (view.scroll = Math.max(0, Math.min(view.scroll, lanes.value.length * LANE_H - (size.h - AXIS_H)))),
)

function at(ev: MouseEvent) {
  const r = canvas.value!.getBoundingClientRect()
  return { x: ev.clientX - r.left, y: ev.clientY - r.top }
}

// The frame under (x, y): the bar in its sender's lane, or its trace in a receiver's.
function hit(x: number, y: number): { frame: Frame; rx: string | null } | null {
  if (x < LABEL_W || y < AXIS_H) return null
  const lane = lanes.value[Math.floor((y + view.scroll - AXIS_H) / LANE_H)]
  if (!lane) return null
  const tEnd = t1()
  const t0 = tEnd - view.span
  const t = t0 + ((x - LABEL_W) / (size.w - LABEL_W)) * view.span
  const slack = (3 * view.span) / (size.w - LABEL_W)
  const fs = visible(t0, tEnd)
  for (let i = fs.length - 1; i >= 0; i--) {
    const f = fs[i]
    if (t < f.t_start - slack || t > (f.t_end ?? tEnd) + slack) continue
    if (f.src === lane.id) return { frame: f, rx: null }
    const r = f.results?.find((r) => r.node === lane.id)
    if (r && r.outcome !== 'offline' && r.outcome !== 'wrong_sf')
      return { frame: f, rx: `${lane.name}: ${outcomeLabel[r.outcome]}${r.rssi !== null ? `, ${r.rssi} dBm, SNR ${r.snr} dB` : ''}` }
  }
  return null
}

function onDown(ev: PointerEvent) {
  drag.on = true
  drag.moved = false
  drag.x = ev.clientX
  canvas.value!.setPointerCapture(ev.pointerId)
}

function onMove(ev: PointerEvent) {
  const p = at(ev)
  if (drag.on) {
    const dx = ev.clientX - drag.x
    if (Math.abs(dx) > 2 || drag.moved) {
      if (view.live) {
        view.end = serverNow()
        view.live = false
      }
      view.end -= (dx / (size.w - LABEL_W)) * view.span
      drag.x = ev.clientX
      drag.moved = true
    }
    return
  }
  const h = hit(p.x, p.y)
  hover.x = p.x
  hover.y = p.y
  hover.frame = h?.frame ?? null
  hover.rx = h?.rx ?? null
}

function onUp(ev: PointerEvent) {
  drag.on = false
  if (drag.moved) return
  const p = at(ev)
  const h = hit(p.x, p.y)
  if (h) store.selected = { kind: 'frame', id: h.frame.id }
}

function onWheel(ev: WheelEvent) {
  if (ev.shiftKey || (Math.abs(ev.deltaX) < Math.abs(ev.deltaY) && at(ev).x < LABEL_W)) {
    view.scroll = Math.max(0, Math.min(view.scroll + ev.deltaY * 0.5,
      Math.max(0, lanes.value.length * LANE_H - (size.h - AXIS_H))))
    return
  }
  const p = at(ev)
  const tEnd = t1()
  const frac = Math.max(0, (p.x - LABEL_W) / (size.w - LABEL_W))
  const tc = tEnd - view.span + frac * view.span
  const span = Math.min(120e6, Math.max(2_000, view.span * Math.exp(ev.deltaY * 0.002)))
  if (!view.live) view.end = tc + (1 - frac) * span
  view.span = span
}

function goLive() {
  view.live = true
}

const legend = ['POLL', 'DATA', 'HELLO', 'JOIN_OK', 'PAIR_ADV', 'PAIR_REQ', 'FOREIGN']
const legendRx: (keyof typeof outcomeColor)[] = ['ok', 'collision', 'jammed', 'below_sens', 'half_duplex', 'random_loss']
</script>

<template>
  <div class="tl">
    <div class="head">
      <span class="panel-title">Timeline</span>
      <button :class="{ on: view.live }" title="Follow the air as it happens" @click="goLive">Live</button>
      <button v-if="!view.live" @click="view.end = t1() - view.span / 2">◀</button>
      <button v-if="!view.live" @click="view.end = t1() + view.span / 2">▶</button>
      <span class="muted">{{ fmtSpan(view.span) }} shown · wheel zooms · drag pans · shift-wheel scrolls lanes</span>
      <span class="spacer" />
      <span v-for="k in legend" :key="k" class="lg"><span class="sw" :style="{ background: frameColor(k, false) }" />{{ k }}</span>
      <span class="lg"><span class="sw" :style="{ background: frameColor('', true), opacity: 0.5 }" />energy</span>
      <span class="sep" />
      <span v-for="o in legendRx" :key="o" class="lg"><span class="sw thin" :style="{ background: outcomeColor[o] }" />{{ outcomeLabel[o] }}</span>
    </div>
    <div ref="wrap" class="cv" @wheel.prevent="onWheel">
      <canvas ref="canvas" :style="{ width: size.w + 'px', height: size.h + 'px' }" @pointerdown="onDown"
        @pointermove="onMove" @pointerup="onUp" @pointerleave="hover.frame = null" />
      <div v-if="hover.frame && !drag.on" class="tip" :style="{ left: Math.min(hover.x + 12, size.w - 230) + 'px', top: hover.y + 14 + 'px' }">
        <b>{{ hover.frame.energy ? 'energy' : hover.frame.info.kind }}</b> #{{ hover.frame.id }} from {{ nodeName(hover.frame.src) }}<br />
        <template v-if="!hover.frame.energy">
          {{ hover.frame.len }} B, SF{{ hover.frame.sf }}, {{ hover.frame.power }} dBm,
          {{ hover.frame.t_end !== null ? ((hover.frame.t_end - hover.frame.t_start) / 1000).toFixed(1) + ' ms' : '' }}
          <template v-if="hover.frame.info.src !== undefined"><br />{{ slotName(hover.frame.info.src) }} → {{ slotName(hover.frame.info.dst) }}, net {{ hover.frame.info.net_tag }}</template>
        </template>
        <template v-if="hover.rx"><br />{{ hover.rx }}</template>
      </div>
    </div>
  </div>
</template>

<style scoped>
.tl {
  display: flex;
  flex-direction: column;
  background: var(--panel);
  min-height: 0;
}
.head {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 4px 8px;
  flex-wrap: wrap;
}
.head .panel-title {
  margin: 0;
}
.head .muted {
  font-size: 11px;
}
.spacer {
  flex: 1;
}
.lg {
  font-size: 10px;
  color: var(--muted);
  display: inline-flex;
  align-items: center;
  gap: 3px;
}
.sw {
  width: 10px;
  height: 8px;
  border-radius: 2px;
  display: inline-block;
}
.sw.thin {
  height: 4px;
}
.sep {
  width: 1px;
  height: 14px;
  background: var(--line);
}
.cv {
  position: relative;
  flex: 1;
  min-height: 0;
  overflow: hidden;
}
canvas {
  display: block;
  cursor: crosshair;
}
.tip {
  position: absolute;
  background: #1f2937;
  border: 1px solid var(--line);
  padding: 4px 8px;
  border-radius: 4px;
  font-size: 11px;
  pointer-events: none;
  white-space: nowrap;
  z-index: 2;
}
</style>
