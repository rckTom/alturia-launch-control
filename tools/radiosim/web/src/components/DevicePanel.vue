<script setup lang="ts">
import { computed, reactive, ref } from 'vue'
import { api } from '../api'
import { run } from '../store'
import type { Entity } from '../types'
import LinkEditor from './LinkEditor.vue'

const props = defineProps<{ dev: Entity }>()
const d = computed(() => props.dev)
const form = reactive<Record<string, unknown>>({ ...props.dev })
const hex = ref('')

const title = computed(
  () => ({ jammer: 'Jammer', traffic: 'Foreign traffic', radio: 'Radio' })[d.value.device_type!] ?? 'Device',
)

function apply(extra: Record<string, unknown> = {}) {
  const p: Record<string, unknown> = { ...extra }
  for (const k of ['name', 'power_dbm', 'sf', 'mode', 'period_ms', 'duty', 'wideband', 'rate_hz', 'len_min', 'len_max'])
    if (form[k] !== undefined) p[k] = form[k]
  run(api.updateDevice(d.value.id, p))
}

function toggle() {
  form.enabled = !d.value.enabled
  apply({ enabled: form.enabled })
}

function send() {
  run(api.inject(d.value.id, hex.value, Number(form.sf)))
}
</script>

<template>
  <div>
    <h3 class="panel-title">{{ title }} {{ d.name }}</h3>
    <div class="row actions">
      <button v-if="d.device_type !== 'radio'" :class="{ on: d.enabled }" @click="toggle">
        {{ d.enabled ? 'On' : 'Off' }}
      </button>
      <button class="danger" @click="run(api.removeDevice(d.id))">Remove</button>
    </div>
    <div class="grid2">
      <label>name</label><input v-model="form.name" @change="apply()" />
      <label>power at antenna, dBm</label><input v-model.number="form.power_dbm" type="number" @change="apply()" />
      <template v-if="d.device_type === 'jammer'">
        <label>signal</label>
        <select v-model="form.wideband" @change="apply()">
          <option :value="true">wideband noise</option>
          <option :value="false">LoRa-like on its SF</option>
        </select>
        <label v-if="!form.wideband">SF</label>
        <input v-if="!form.wideband" v-model.number="form.sf" type="number" min="5" max="12" @change="apply()" />
        <label>mode</label>
        <select v-model="form.mode" @change="apply()">
          <option value="continuous">continuous</option>
          <option value="pulsed">pulsed</option>
        </select>
        <template v-if="form.mode === 'pulsed'">
          <label>period, ms</label><input v-model.number="form.period_ms" type="number" @change="apply()" />
          <label>duty, 0–1</label><input v-model.number="form.duty" type="number" step="0.05" @change="apply()" />
        </template>
      </template>
      <template v-if="d.device_type === 'traffic'">
        <label>SF</label><input v-model.number="form.sf" type="number" min="5" max="12" @change="apply()" />
        <label>frames per second</label><input v-model.number="form.rate_hz" type="number" step="0.5" @change="apply()" />
        <label>length, bytes</label>
        <span class="row"><input v-model.number="form.len_min" type="number" @change="apply()" /> –
          <input v-model.number="form.len_max" type="number" @change="apply()" /></span>
      </template>
      <template v-if="d.device_type === 'radio'">
        <label>SF</label><input v-model.number="form.sf" type="number" min="5" max="12" @change="apply()" />
      </template>
    </div>

    <div v-if="d.device_type === 'radio'" class="inject">
      <label class="muted">send a frame (hex)</label>
      <div class="row">
        <input v-model="hex" class="mono hex" placeholder="65 34 12 00 ff …" @keydown.enter="send" />
        <button class="primary" :disabled="!hex" @click="send">Send</button>
      </div>
      <p class="muted">Or select a frame on the timeline and replay it from here.</p>
    </div>

    <h3 class="panel-title links">Links</h3>
    <LinkEditor :ent="d" />
  </div>
</template>

<style scoped>
.actions {
  margin-bottom: 10px;
}
.grid2 input,
.grid2 select {
  width: 150px;
}
.grid2 .row input {
  width: 60px;
}
.inject {
  margin-top: 12px;
}
.hex {
  flex: 1;
}
.inject p {
  font-size: 11px;
}
.links {
  margin-top: 14px;
}
</style>
