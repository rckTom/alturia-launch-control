<script setup lang="ts">
import { computed, onBeforeUnmount, ref } from 'vue'
import { api } from '../api'
import { openTab, run, store } from '../store'
import type { Entity } from '../types'
import LinkEditor from './LinkEditor.vue'

const props = defineProps<{ node: Entity }>()
const n = computed(() => props.node)

// LED mask from the satellite (sim_io.c): bits 0-5 channel LEDs, 6 status,
// 8-13 channel armed (high-side enable).
const chanLeds = computed(() => [0, 1, 2, 3, 4, 5].map((i) => ((n.value.leds ?? 0) >> i) & 1))
const statusLed = computed(() => ((n.value.leds ?? 0) >> 6) & 1)
const armed = computed(() => [0, 1, 2, 3, 4, 5].map((i) => ((n.value.leds ?? 0) >> (8 + i)) & 1))

// The pairing button: its hold time is the gesture. gpio-keys debounces,
// so even a quick tap is held for MIN_PRESS_MS.
const MIN_PRESS_MS = 150
const held = ref(0)
let heldAt = 0
let heldTimer = 0
async function press(down: boolean) {
  if (down) {
    heldAt = performance.now()
    held.value = 0
    heldTimer = window.setInterval(() => (held.value = (performance.now() - heldAt) / 1000), 100)
    run(api.button(n.value.id, true))
    return
  }
  clearInterval(heldTimer)
  if (!heldAt) return
  const short = MIN_PRESS_MS - (performance.now() - heldAt)
  heldAt = 0
  if (short > 0) await new Promise((r) => setTimeout(r, short))
  run(api.button(n.value.id, false))
}
onBeforeUnmount(() => clearInterval(heldTimer))

const power = ref<number | null>(props.node.tx_power_override ?? null)
function setPower() {
  if (power.value === null || (power.value as unknown) === '')
    run(api.patchEntity(n.value.id, { clear_tx_power_override: true }))
  else run(api.patchEntity(n.value.id, { tx_power_override: Math.round(power.value) }))
}

function remove() {
  if (confirm(`Stop and remove ${n.value.name}? Its flash file stays in the work directory.`))
    run(api.removeNode(n.value.id))
}
</script>

<template>
  <div>
    <h3 class="panel-title">
      {{ n.role === 'ctrl' ? 'Controller' : n.role === 'sat' ? 'Satellite' : 'Node' }} {{ n.name }}
    </h3>
    <div class="row actions">
      <button v-if="n.role === 'ctrl'" class="primary" @click="openTab({ type: 'screen', id: n.id })">Screen</button>
      <button class="primary" @click="openTab({ type: 'shell', id: n.id })">Shell</button>
      <button v-if="!n.running" :disabled="!n.managed && !store.canSpawn[n.role!]" @click="run(api.startNode(n.id))">Start</button>
      <button v-if="n.running" @click="run(api.restartNode(n.id))">Restart</button>
      <button v-if="n.running" @click="run(api.killNode(n.id))">Stop</button>
      <button class="danger" @click="remove">Remove</button>
    </div>

    <div class="grid2">
      <span class="muted">state</span>
      <span>
        <span class="dot" :style="{ background: n.connected ? 'var(--ok)' : 'var(--danger)' }" />
        {{ n.connected ? 'connected' : n.running ? 'starting…' : 'not running' }}
        <span v-if="n.pid" class="muted">pid {{ n.pid }}</span>
        <span v-if="!n.managed" class="muted"> (started by hand)</span>
      </span>
      <span class="muted">radio</span>
      <span>SF{{ n.sf }} {{ n.sf === 8 ? '(pairing profile)' : '' }}</span>
      <template v-if="n.role === 'sat'">
        <span class="muted">uid</span><span class="mono">{{ n.uid }}</span>
      </template>
      <span class="muted">position</span>
      <span class="mono">{{ Math.round(n.x) }}, {{ Math.round(n.y) }} m</span>
      <span class="muted" title="Replaces the power the firmware asks for">TX power</span>
      <span class="row">
        <input v-model.number="power" type="number" placeholder="as fw" @change="setPower" /> dBm
      </span>
      <span class="muted" title="Antenna off: hears nothing, sends nothing">radio off</span>
      <input type="checkbox" :checked="n.isolated" @change="run(api.patchEntity(n.id, { isolated: ($event.target as HTMLInputElement).checked }))" />
    </div>

    <div v-if="n.role === 'sat'" class="sat">
      <div class="leds">
        <div v-for="(on, i) in chanLeds" :key="i" class="led-col">
          <span class="led" :class="{ on }" />
          <span class="muted">{{ i + 1 }}</span>
          <span class="arm" :class="{ on: armed[i] }" :title="armed[i] ? 'armed' : 'safe'">{{ armed[i] ? 'ARM' : '' }}</span>
        </div>
        <div class="led-col">
          <span class="led status" :class="{ on: statusLed }" />
          <span class="muted">st</span>
        </div>
      </div>
      <button class="pairbtn" :disabled="!n.connected" @pointerdown="press(true)" @pointerup="press(false)"
        @pointerleave="heldAt && press(false)">
        Button <span v-if="held > 0 && heldAt" class="mono">{{ held.toFixed(1) }} s</span>
      </button>
      <p class="muted hint">
        Hold 5 s (all SAFE) to pair, then compare the LEDs with the controller: short press if they match, hold 2 s if not.
      </p>
    </div>

    <h3 class="panel-title links">Links</h3>
    <LinkEditor :ent="n" />
  </div>
</template>

<style scoped>
.actions {
  margin-bottom: 10px;
}
.sat {
  margin-top: 12px;
  padding: 10px;
  background: var(--panel-2);
  border-radius: 6px;
}
.leds {
  display: flex;
  gap: 10px;
  margin-bottom: 8px;
}
.led-col {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 2px;
  font-size: 10px;
  width: 26px;
}
.led {
  width: 16px;
  height: 16px;
  border-radius: 50%;
  background: #2a1414;
  border: 1px solid #3f1d1d;
}
.led.on {
  background: #ff3b3b;
  box-shadow: 0 0 10px #ff3b3b;
}
.led.status {
  background: #102a18;
  border-color: #14532d;
}
.led.status.on {
  background: #22c55e;
  box-shadow: 0 0 10px #22c55e;
}
.arm {
  font-size: 9px;
  height: 11px;
  color: var(--danger);
  font-weight: 700;
}
.pairbtn {
  width: 100%;
  padding: 8px;
  user-select: none;
}
.pairbtn:active {
  background: #1e3a5f;
}
.hint {
  font-size: 11px;
  margin: 6px 0 0;
}
.links {
  margin-top: 14px;
}
</style>
