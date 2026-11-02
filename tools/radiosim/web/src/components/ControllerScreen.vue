<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { api } from '../api'
import { store, wsUrl } from '../store'

const props = defineProps<{ id: number }>()
const canvas = ref<HTMLCanvasElement>()
const box = ref<HTMLDivElement>()
const dim = reactive({ w: 400, h: 240, scale: 2 })
const state = ref<'connecting' | 'open' | 'closed'>('connecting')
const node = computed(() => store.entities.get(props.id))

// A Sharp Memory LCD: black pixels on a silvery white.
const LIGHT = [0xd7, 0xdb, 0xd2]
const DARK = [0x16, 0x18, 0x15]

// The controls' gpio0 pins, as boards/native_sim.overlay gives them to gpio-keys.
const PIN = {
  fire: 2, back: 3, arm: 4, fill: 5, vent: 6, up: 7, down: 8, select: 9,
  left: 12, right: 13, power: 14,
}
type Control = keyof typeof PIN
const KEYS: Record<string, Control> = {
  ' ': 'fire', Escape: 'back', a: 'arm', f: 'fill', v: 'vent', p: 'power',
  ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right', Enter: 'select',
}
const down = reactive<Record<string, boolean>>({})

let ws: WebSocket | undefined
let closed = false
let retry = 0
let ro: ResizeObserver | undefined

function draw(buf: ArrayBuffer) {
  const v = new DataView(buf)
  const w = v.getUint16(0, true)
  const h = v.getUint16(2, true)
  const fmt = v.getUint8(4)
  const stride = v.getUint16(6, true)
  const px = new Uint8Array(buf, 8)
  const c = canvas.value
  if (!c) return
  if (c.width !== w || c.height !== h) {
    c.width = w
    c.height = h
    dim.w = w
    dim.h = h
    rescale()
  }
  const g = c.getContext('2d')!
  const img = g.createImageData(w, h)
  const oneIsWhite = fmt === 1 // MONO01
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const bit = (px[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1
      const col = (bit === 1) === oneIsWhite ? LIGHT : DARK
      const o = (y * w + x) * 4
      img.data[o] = col[0]
      img.data[o + 1] = col[1]
      img.data[o + 2] = col[2]
      img.data[o + 3] = 255
    }
  }
  g.putImageData(img, 0, 0)
}

function connect() {
  ws = new WebSocket(wsUrl(`/ws/screen/${props.id}`))
  ws.binaryType = 'arraybuffer'
  ws.onopen = () => (state.value = 'open')
  ws.onmessage = (m) => draw(m.data as ArrayBuffer)
  ws.onclose = () => {
    state.value = 'closed'
    if (!closed) retry = window.setTimeout(connect, 1500)
  }
}

function rescale() {
  const b = box.value
  if (!b) return
  const s = Math.min((b.clientWidth - 8) / dim.w, (b.clientHeight - 8) / dim.h)
  // Whole multiples keep the pixels crisp when there is room.
  dim.scale = s >= 1 ? Math.max(1, Math.floor(s * 2) / 2) : Math.max(0.25, s)
}

onMounted(() => {
  ro = new ResizeObserver(rescale)
  ro.observe(box.value!)
  connect()
})
onBeforeUnmount(() => {
  closed = true
  clearTimeout(retry)
  ws?.close()
  ro?.disconnect()
})

function send(type: 'pin' | 'wheel' | 'touch', code: number, value: number) {
  api.input(props.id, type, code, value).catch(() => {})
}

// gpio-keys debounces: hold even a quick tap for this long.
const MIN_PRESS_MS = 80
const pressedAt: Record<string, number> = {}

function control(c: Control, on: boolean) {
  if (!!down[c] === on) return
  down[c] = on
  if (on) {
    pressedAt[c] = performance.now()
    send('pin', PIN[c], 1)
  } else {
    const wait = Math.max(0, MIN_PRESS_MS - (performance.now() - (pressedAt[c] ?? 0)))
    setTimeout(() => send('pin', PIN[c], 0), wait)
  }
}

function onKey(ev: KeyboardEvent, on: boolean) {
  const c = KEYS[ev.key] ?? KEYS[ev.key.toLowerCase()]
  if (!c) return
  ev.preventDefault()
  if (ev.repeat) return
  control(c, on)
}

// Mouse wheel is the encoder, one detent per notch; up turns up.
let wheelAcc = 0
function onWheel(ev: WheelEvent) {
  wheelAcc += ev.deltaMode === 1 ? ev.deltaY * 40 : ev.deltaY
  const steps = Math.trunc(wheelAcc / 60)
  if (steps) {
    wheelAcc -= steps * 60
    send('wheel', 0, -steps)
  }
}

function releaseAll() {
  for (const c of Object.keys(down) as Control[]) if (down[c]) control(c, false)
}
</script>

<template>
  <div class="scr" tabindex="0" @keydown="onKey($event, true)" @keyup="onKey($event, false)" @blur="releaseAll">
    <div ref="box" class="lcd">
      <canvas ref="canvas" width="400" height="240" :style="{ width: dim.w * dim.scale + 'px', height: dim.h * dim.scale + 'px' }"
        title="Wheel: up/down · click: OK (hold for long press)"
        @wheel.prevent="onWheel" @pointerdown="send('touch', 0, 1)" @pointerup="send('touch', 0, 0)" />
      <span v-if="state !== 'open' || !node?.connected" class="state">
        {{ state !== 'open' ? state : node?.running ? 'waiting for the controller…' : 'not running' }}
      </span>
    </div>
    <div class="controls">
      <button class="arm" :class="{ down: down.arm }" title="A"
        @pointerdown="control('arm', true)" @pointerup="control('arm', false)" @pointerleave="control('arm', false)">ARM</button>
      <button :class="{ down: down.fill }" title="F"
        @pointerdown="control('fill', true)" @pointerup="control('fill', false)" @pointerleave="control('fill', false)">FILL</button>
      <button :class="{ down: down.vent }" title="V"
        @pointerdown="control('vent', true)" @pointerup="control('vent', false)" @pointerleave="control('vent', false)">VENT</button>
      <button class="fire" :class="{ down: down.fire }" title="Hold (Space)"
        @pointerdown="control('fire', true)" @pointerup="control('fire', false)" @pointerleave="control('fire', false)">FIRE</button>
      <button class="back" :class="{ down: down.back }" title="Esc; hold to leave the button test"
        @pointerdown="control('back', true)" @pointerup="control('back', false)" @pointerleave="control('back', false)">BACK</button>
      <div class="pad">
        <button class="up" :class="{ down: down.up }" @pointerdown="control('up', true)" @pointerup="control('up', false)" @pointerleave="control('up', false)">▲</button>
        <button :class="{ down: down.left }" @pointerdown="control('left', true)" @pointerup="control('left', false)" @pointerleave="control('left', false)">◀</button>
        <button :class="{ down: down.select }" title="OK" @pointerdown="control('select', true)" @pointerup="control('select', false)" @pointerleave="control('select', false)">●</button>
        <button :class="{ down: down.right }" @pointerdown="control('right', true)" @pointerup="control('right', false)" @pointerleave="control('right', false)">▶</button>
        <button class="dn" :class="{ down: down.down }" @pointerdown="control('down', true)" @pointerup="control('down', false)" @pointerleave="control('down', false)">▼</button>
      </div>
      <button class="power" :class="{ down: down.power }" title="Power button (P)"
        @pointerdown="control('power', true)" @pointerup="control('power', false)" @pointerleave="control('power', false)">POWER</button>
      <p class="muted help">Click the screen for the keyboard: A F V P, Space fire, Esc back, arrows, Enter.</p>
    </div>
  </div>
</template>

<style scoped>
.scr {
  display: flex;
  gap: 10px;
  height: 100%;
  outline: none;
  padding: 4px;
}
.scr:focus-within .lcd {
  outline: 1px solid var(--accent);
}
.lcd {
  flex: 1;
  min-width: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  position: relative;
  background: #0b0f14;
  border-radius: 6px;
}
canvas {
  image-rendering: pixelated;
  border-radius: 3px;
  box-shadow: 0 0 0 6px #262a24;
  cursor: ns-resize;
}
.state {
  position: absolute;
  top: 8px;
  right: 10px;
  font-size: 11px;
  color: var(--warn);
}
.controls {
  width: 150px;
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 6px;
  align-content: start;
}
.controls button {
  padding: 8px 4px;
  font-weight: 600;
  user-select: none;
}
.controls button.down {
  background: #1e3a5f;
  border-color: var(--accent);
}
.arm {
  color: #fbbf24;
}
.fire {
  background: #7f1d1d;
  border-color: #b91c1c;
}
.fire.down {
  background: #dc2626 !important;
}
.back {
  color: #4ade80;
}
.power {
  grid-column: span 2;
}
.pad {
  grid-column: span 2;
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 6px;
}
.pad .up,
.pad .dn {
  grid-column: 2;
}
.help {
  grid-column: span 2;
  font-size: 10.5px;
  margin: 0;
}
</style>
