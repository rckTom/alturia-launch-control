<script setup lang="ts">
import { ref } from 'vue'
import { api } from '../api'
import { run, store, toast } from '../store'

const file = ref<HTMLInputElement>()

async function addNode(role: 'ctrl' | 'sat') {
  const e = (await run(api.addNode(role))) as { id: number } | undefined
  if (e) store.selected = { kind: 'entity', id: e.id }
}

async function addDevice(type: 'jammer' | 'traffic' | 'radio') {
  // Near the middle of the nodes, where it matters.
  const nodes = [...store.entities.values()].filter((e) => e.kind === 'node')
  const cx = nodes.length ? nodes.reduce((s, e) => s + e.x, 0) / nodes.length : 0
  const cy = nodes.length ? nodes.reduce((s, e) => s + e.y, 0) / nodes.length : 0
  const params: Record<string, unknown> = { x: cx + 60, y: cy + 60 }
  if (type === 'jammer') Object.assign(params, { power_dbm: 0, mode: 'pulsed', period_ms: 100, duty: 0.3 })
  if (type === 'traffic') Object.assign(params, { power_dbm: 7, rate_hz: 3 })
  const e = (await run(api.addDevice(type, params))) as { id: number } | undefined
  if (e) store.selected = { kind: 'entity', id: e.id }
}

async function save() {
  const r = await run(api.saveScenario())
  if (r) toast(`saved to ${r.path}`)
}

async function load() {
  if (confirm('Replace everything with the saved scenario? Running nodes are stopped.'))
    await run(api.loadScenario(undefined, true))
}

function importFile(ev: Event) {
  const f = (ev.target as HTMLInputElement).files?.[0]
  if (!f) return
  f.text().then(async (text) => {
    try {
      await run(api.loadScenario(JSON.parse(text), true))
    } catch {
      toast('not a scenario file')
    }
  })
  ;(ev.target as HTMLInputElement).value = ''
}
</script>

<template>
  <header class="bar">
    <span class="brand"><span class="logo" />radiosim</span>
    <div class="group">
      <button class="primary" :disabled="!store.canSpawn.ctrl" title="Start a controller instance"
        @click="addNode('ctrl')">+ Controller</button>
      <button class="primary" :disabled="!store.canSpawn.sat" title="Start a satellite instance"
        @click="addNode('sat')">+ Satellite</button>
    </div>
    <div class="group">
      <button title="Wideband or LoRa-like RF energy, continuous or pulsed" @click="addDevice('jammer')">+ Jammer</button>
      <button title="Another LoRa network: random frames at a Poisson rate" @click="addDevice('traffic')">+ Traffic</button>
      <button title="A radio that sends what you tell it: inject and replay" @click="addDevice('radio')">+ Radio</button>
    </div>
    <div class="group">
      <button title="Save to the work directory's scenario.json" @click="save">Save</button>
      <button title="Load the work directory's scenario.json and start its nodes" @click="load">Load</button>
      <a href="/api/scenario" download="scenario.json"><button>Export</button></a>
      <button @click="file?.click()">Import</button>
      <input ref="file" type="file" accept=".json" hidden @change="importFile" />
    </div>
    <span class="spacer" />
    <span class="muted mono" :title="`work directory: ${store.workdir}`">{{ store.workdir }}</span>
    <span class="dot" :style="{ background: store.connected ? 'var(--ok)' : 'var(--danger)' }" />
  </header>
</template>

<style scoped>
.bar {
  display: flex;
  align-items: center;
  gap: 14px;
  padding: 6px 10px;
  background: var(--panel);
}
.brand {
  font-weight: 700;
  letter-spacing: 0.02em;
  display: flex;
  align-items: center;
  gap: 7px;
}
.logo {
  width: 14px;
  height: 14px;
  border-radius: 50%;
  border: 2px solid var(--accent);
  box-shadow: inset 0 0 0 3px var(--panel), inset 0 0 0 7px var(--accent);
}
.group {
  display: flex;
  gap: 5px;
}
.spacer {
  flex: 1;
}
.muted {
  max-width: 30vw;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  font-size: 11px;
}
</style>
