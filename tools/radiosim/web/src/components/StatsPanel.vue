<script setup lang="ts">
import { computed } from 'vue'
import { nodeName, store } from '../store'
import { outcomeColor, outcomeLabel } from '../style'
import type { Outcome } from '../types'

const s = computed(() => store.stats)
const pct = (v: number | undefined) => (v === undefined ? '–' : `${(v * 100).toFixed(1)} %`)

const sources = computed(() =>
  Object.entries(s.value?.sources ?? {})
    .map(([id, v]) => ({ id: Number(id), ...v }))
    .sort((a, b) => b.airtime_pct - a.airtime_pct),
)

const outcomes = computed(() => {
  const o = s.value?.outcomes_10s ?? {}
  const total = Object.entries(o)
    .filter(([k]) => k !== 'offline' && k !== 'wrong_sf')
    .reduce((a, [, v]) => a + v, 0)
  return {
    total,
    list: (Object.entries(o) as [Outcome, number][])
      .filter(([k]) => k !== 'offline' && k !== 'wrong_sf')
      .sort((a, b) => b[1] - a[1]),
  }
})
</script>

<template>
  <section class="stats">
    <h3 class="panel-title">Channel</h3>
    <div class="util">
      <div v-for="w in ['1', '10', '60']" :key="w" class="gauge">
        <div class="val">{{ pct(s?.util[w]) }}</div>
        <div class="bar"><span :style="{ width: Math.min(100, (s?.util[w] ?? 0) * 100) + '%' }" /></div>
        <div class="muted">busy, {{ w }} s</div>
      </div>
      <div class="gauge">
        <div class="val" :class="{ jam: (s?.jam['10'] ?? 0) > 0 }">{{ pct(s?.jam['10']) }}</div>
        <div class="bar jam"><span :style="{ width: Math.min(100, (s?.jam['10'] ?? 0) * 100) + '%' }" /></div>
        <div class="muted">jammed, 10 s</div>
      </div>
    </div>

    <div v-if="s?.polls.length" class="block">
      <table class="small">
        <tr><th>network</th><th>poll period</th><th>replies per poll, by slot</th></tr>
        <tr v-for="p in s.polls" :key="p.net_tag">
          <td class="mono">{{ nodeName(p.ctrl) }} <span class="muted">{{ p.net_tag }}</span></td>
          <td>{{ p.period_ms ?? '–' }} ms</td>
          <td>
            <span v-for="(r, slot) in p.replies" :key="slot" class="slot"
              :style="{ color: r >= 0.95 ? 'var(--ok)' : r >= 0.7 ? 'var(--warn)' : 'var(--danger)' }">
              {{ String.fromCharCode(64 + Number(slot)) }} {{ Math.round(r * 100) }}%
            </span>
            <span v-if="!Object.keys(p.replies).length" class="muted">none</span>
          </td>
        </tr>
      </table>
    </div>

    <div v-if="s?.joins?.length" class="block">
      <table class="small">
        <tr><th>joins, 60 s</th><th>slot</th><th>tries</th><th>joined</th><th>took</th></tr>
        <tr v-for="j in s.joins" :key="j.net_tag + j.slot">
          <td class="mono">{{ j.ctrl !== null ? nodeName(j.ctrl) : '?' }} <span class="muted">{{ j.net_tag }}</span></td>
          <td>{{ String.fromCharCode(64 + j.slot) }}</td>
          <td>{{ j.attempts }}</td>
          <td :style="{ color: j.joins ? 'var(--ok)' : 'var(--danger)' }">{{ j.joins }}</td>
          <td>
            <span v-if="j.last_join_ms !== null">{{ (j.last_join_ms / 1000).toFixed(1) }} s</span>
            <span v-if="j.joining" class="muted"> joining…</span>
          </td>
        </tr>
      </table>
    </div>

    <div v-if="outcomes.total" class="block">
      <div class="stack">
        <span v-for="[k, v] in outcomes.list" :key="k" :title="`${outcomeLabel[k]}: ${v}`"
          :style="{ flex: v, background: outcomeColor[k] }" />
      </div>
      <div class="row small">
        <span v-for="[k, v] in outcomes.list" :key="k"><span class="dot" :style="{ background: outcomeColor[k] }" />
          {{ outcomeLabel[k] }} {{ v }}</span>
      </div>
    </div>

    <div v-if="sources.length" class="block">
      <table class="small">
        <tr><th>airtime, 10 s</th><th /><th>frames</th></tr>
        <tr v-for="src in sources" :key="src.id">
          <td>{{ nodeName(src.id) }}</td>
          <td class="barcell"><span :style="{ width: Math.min(100, src.airtime_pct * 2) + '%' }" /> {{ src.airtime_pct }} %</td>
          <td>{{ src.frames_10s }}</td>
        </tr>
      </table>
    </div>
  </section>
</template>

<style scoped>
.stats {
  padding: 10px 12px;
  border-bottom: 1px solid var(--line);
}
.util {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 8px;
}
.gauge .val {
  font-size: 17px;
  font-weight: 600;
  font-variant-numeric: tabular-nums;
}
.gauge .val.jam {
  color: var(--danger);
}
.gauge .muted {
  font-size: 10px;
}
.bar {
  height: 4px;
  background: var(--panel-2);
  border-radius: 2px;
  overflow: hidden;
  margin: 2px 0;
}
.bar span {
  display: block;
  height: 100%;
  background: var(--accent);
}
.bar.jam span {
  background: var(--danger);
}
.block {
  margin-top: 10px;
}
.slot {
  margin-right: 8px;
  font-variant-numeric: tabular-nums;
}
.stack {
  display: flex;
  height: 6px;
  border-radius: 3px;
  overflow: hidden;
  gap: 1px;
}
.row.small {
  font-size: 11px;
  margin-top: 4px;
  gap: 10px;
}
.barcell {
  width: 55%;
}
.barcell span {
  display: inline-block;
  height: 6px;
  background: var(--accent);
  border-radius: 2px;
  vertical-align: middle;
  max-width: 70%;
}
</style>
