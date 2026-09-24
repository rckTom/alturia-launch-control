<script setup lang="ts">
import { computed } from 'vue'
import { api } from '../api'
import { run, store } from '../store'
import { pdrColor } from '../style'
import type { Entity } from '../types'

const props = defineProps<{ ent: Entity }>()

const rows = computed(() =>
  [...store.entities.values()]
    .filter((e) => e.id !== props.ent.id && (e.kind === 'node' || props.ent.kind === 'node'))
    .map((e) => {
      const l = store.links.find(
        (l) => (l.a === e.id && l.b === props.ent.id) || (l.b === e.id && l.a === props.ent.id),
      )
      const st = (store.stats?.links ?? []).filter(
        (s) => (s.src === e.id && s.dst === props.ent.id) || (s.dst === e.id && s.src === props.ent.id),
      )
      const rx = st.find((s) => s.dst === props.ent.id)
      const tx = st.find((s) => s.src === props.ent.id)
      return { e, extra: l?.extra_db ?? 0, cut: l?.cut ?? false, loss: l?.loss_pct ?? 0, rx, tx }
    }),
)

function set(id: number, extra: number, cut: boolean, loss: number) {
  run(api.setLink(props.ent.id, id, extra, cut, loss))
}
</script>

<template>
  <table class="small">
    <tr><th>link to</th><th title="Extra attenuation, both ways">+dB</th><th title="Random loss, %">loss %</th><th>cut</th><th title="Delivery ratio, last 30 s: to it / from it">PDR</th></tr>
    <tr v-for="r in rows" :key="r.e.id">
      <td>{{ r.e.name }}</td>
      <td><input type="number" :value="r.extra" step="5" @change="set(r.e.id, +($event.target as HTMLInputElement).value, r.cut, r.loss)" /></td>
      <td><input type="number" :value="r.loss" step="10" min="0" max="100" @change="set(r.e.id, r.extra, r.cut, +($event.target as HTMLInputElement).value)" /></td>
      <td><input type="checkbox" :checked="r.cut" @change="set(r.e.id, r.extra, ($event.target as HTMLInputElement).checked, r.loss)" /></td>
      <td class="mono">
        <span v-if="r.tx" :style="{ color: pdrColor(r.tx.pdr) }" :title="`${r.tx.ok}/${r.tx.total} to ${r.e.name}`">→{{ Math.round(r.tx.pdr * 100) }}</span>
        <span v-if="r.rx" :style="{ color: pdrColor(r.rx.pdr) }" :title="`${r.rx.ok}/${r.rx.total} from ${r.e.name}`"> ←{{ Math.round(r.rx.pdr * 100) }}</span>
      </td>
    </tr>
  </table>
</template>

<style scoped>
input[type='number'] {
  width: 52px;
}
</style>
