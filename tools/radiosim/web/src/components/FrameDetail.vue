<script setup lang="ts">
import { computed, ref } from 'vue'
import { api } from '../api'
import { nodeName, run, slotName, store } from '../store'
import { frameColor, outcomeColor, outcomeLabel } from '../style'
import type { Frame } from '../types'

const props = defineProps<{ frame: Frame }>()
const f = computed(() => props.frame)

const radios = computed(() => [...store.entities.values()].filter((e) => e.device_type === 'radio'))
const from = ref<number | null>(radios.value[0]?.id ?? null)

const hexRows = computed(() => {
  const h = f.value.hex ?? ''
  const rows: { off: string; bytes: string }[] = []
  for (let i = 0; i < h.length; i += 32) {
    rows.push({
      off: (i / 2).toString(16).padStart(3, '0'),
      bytes: h.slice(i, i + 32).replace(/(..)/g, '$1 ').trim(),
    })
  }
  return rows
})

const airtime = computed(() =>
  f.value.t_end !== null ? ((f.value.t_end - f.value.t_start) / 1000).toFixed(2) + ' ms' : 'on',
)

async function addRadio() {
  const src = store.entities.get(f.value.src)
  const e = (await run(api.addDevice('radio', { x: (src?.x ?? 0) + 40, y: (src?.y ?? 0) + 40, power_dbm: 7 }))) as
    | { id: number }
    | undefined
  if (e) from.value = e.id
}

function replay() {
  if (from.value !== null) run(api.replay(f.value.id, from.value))
}
</script>

<template>
  <div>
    <h3 class="panel-title">
      Frame #{{ f.id }}
      <span class="kind" :style="{ background: frameColor(f.info.kind, f.energy) }">{{ f.energy ? 'energy' : f.info.kind }}</span>
    </h3>
    <div class="grid2">
      <span class="muted">from</span><span>{{ nodeName(f.src) }}</span>
      <span class="muted">start</span><span class="mono">{{ (f.t_start / 1e6).toFixed(6) }} s</span>
      <span class="muted">on air</span><span>{{ airtime }}</span>
      <span class="muted">radio</span><span>SF{{ f.sf }}, {{ f.power }} dBm{{ f.wideband ? ', wideband' : '' }}</span>
      <template v-if="f.len !== null"><span class="muted">length</span><span>{{ f.len }} bytes</span></template>
      <template v-if="f.info.net_tag"><span class="muted">network</span><span class="mono">{{ f.info.net_tag }}</span></template>
      <template v-if="f.info.src !== undefined">
        <span class="muted">link addressing</span><span>{{ slotName(f.info.src) }} → {{ slotName(f.info.dst) }}</span>
      </template>
      <template v-if="f.info.kid !== undefined">
        <span class="muted">key, counter</span><span class="mono">kid {{ f.info.kid }}, ctr {{ f.info.ctr }}</span>
      </template>
      <template v-if="f.info.uid"><span class="muted">satellite</span><span class="mono">{{ f.info.uid }}</span></template>
      <template v-if="f.origin"><span class="muted">origin</span><span>{{ f.origin }}</span></template>
    </div>

    <table v-if="f.results" class="small rx">
      <tr><th>receiver</th><th>outcome</th><th>RSSI</th><th>SNR</th><th>SINR</th></tr>
      <tr v-for="r in f.results" :key="r.node">
        <td>{{ nodeName(r.node) }}</td>
        <td><span class="dot" :style="{ background: outcomeColor[r.outcome] }" /> {{ outcomeLabel[r.outcome] }}</td>
        <td class="mono">{{ r.rssi ?? '' }}</td>
        <td class="mono">{{ r.snr ?? '' }}</td>
        <td class="mono">{{ r.sinr ?? '' }}</td>
      </tr>
    </table>
    <p v-else-if="!f.energy" class="muted">on air…</p>

    <pre v-if="f.hex" class="hex mono"><template v-for="r in hexRows" :key="r.off"><span class="muted">{{ r.off }}</span>  {{ r.bytes }}
</template></pre>

    <div v-if="f.hex" class="replay">
      <span class="muted">replay from</span>
      <select v-if="radios.length" v-model="from">
        <option v-for="r in radios" :key="r.id" :value="r.id">{{ r.name }}</option>
      </select>
      <button v-if="radios.length" class="primary" @click="replay">Replay</button>
      <button v-else @click="addRadio">Add a radio to replay from</button>
    </div>
  </div>
</template>

<style scoped>
.kind {
  color: #0b0f14;
  padding: 0 6px;
  border-radius: 3px;
  margin-left: 6px;
  letter-spacing: 0;
}
.rx {
  margin-top: 10px;
}
.hex {
  background: var(--bg);
  border: 1px solid var(--line);
  border-radius: 4px;
  padding: 6px 8px;
  font-size: 11.5px;
  overflow-x: auto;
  margin: 10px 0;
}
.replay {
  display: flex;
  gap: 6px;
  align-items: center;
}
</style>
