<script setup lang="ts">
import { reactive, watch } from 'vue'
import { api } from '../api'
import { run, store } from '../store'

const form = reactive<Record<string, number>>({})
watch(
  () => store.rf,
  (rf) => rf && Object.assign(form, rf),
  { immediate: true },
)

const fields: [string, string, string][] = [
  ['exponent', 'path loss exponent', '2 free space, 2.7–3.5 near the ground'],
  ['pl0_db', 'loss at 1 m, dB', '31.2 dB is free space at 869.85 MHz'],
  ['noise_figure_db', 'receiver noise figure, dB', ''],
  ['capture_db', 'capture margin, dB', 'a frame survives a same-SF interferer this much weaker'],
  ['cross_sf_rejection_db', 'other-SF rejection, dB', 'how much stronger another SF must be to destroy a frame'],
  ['loss_pct', 'random loss on every link, %', ''],
  ['resolve_delay_us', 'decision delay, µs', 'wait after a frame ends for overlapping TX messages'],
]

function apply() {
  const p: Record<string, number> = {}
  for (const [k] of fields) p[k] = Number(form[k])
  run(api.setRf(p))
}
</script>

<template>
  <div>
    <h3 class="panel-title">Radio channel</h3>
    <div class="grid2">
      <template v-for="[k, label, help] in fields" :key="k">
        <label :title="help">{{ label }}</label>
        <input v-model.number="form[k]" type="number" step="any" @change="apply" />
      </template>
      <span class="muted">noise floor</span>
      <span class="mono">{{ store.rf?.noise_floor_dbm }} dBm at {{ store.rf?.bw_khz }} kHz</span>
    </div>
    <p class="muted help">
      Select a node, a device or a frame to inspect it. Nodes run the real firmware under native_sim; every
      frame they send is decided here per receiver: distance and path loss, other frames on air (capture,
      collision), jammers, half duplex, spreading factor, and the link overrides.
    </p>
  </div>
</template>

<style scoped>
.grid2 input {
  width: 90px;
}
.help {
  font-size: 12px;
  margin-top: 14px;
}
</style>
