<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!--
  The ignition log: the current through each channel during its last fire
  pulses, as the satellite recorded it (src/smp/ignlog.ts). On a satellite
  its own records; on the controller the ones it downloaded over LoRa, per
  satellite, with FETCH to download them now.

  The chart is drawn like the controller's memory LCD. Drag across it to
  zoom, double-click for the whole pulse; tick more records to lay them over
  each other (dashed). Requests go one at a time, so the terminal next to
  it keeps working.
-->
<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue'
import type { SmpClient } from '../smp/smp'
import {
  FLAG_FAULT,
  FLAG_NO_CURRENT,
  FLAG_OVERRUN,
  FLAG_TRUNCATED,
  IgnlogClient,
  openMs,
  toCsv,
  type Entry,
  type IgnitionRecord,
  type Listing,
} from '../smp/ignlog'

const props = defineProps<{
  smp?: SmpClient
  kind: 'controller' | 'satellite' | 'other'
  /** mcumgr is free: no firmware upload running */
  enabled: boolean
}>()

const SATS = 'ABCDEFGH'

const s = reactive({
  sat: 0, // controller: slot 1..8; satellite: 0
  listing: null as Listing | null,
  working: '',
  error: '',
  shown: [] as number[], // seqs on the chart, the first one in front
  window: null as [number, number] | null, // ms; null: the whole pulse
})
const records = reactive(new Map<string, IgnitionRecord>())

const isCtrl = computed(() => props.kind === 'controller')
const store = computed(() => s.listing?.store)
const entries = computed(() => [...(store.value?.records ?? [])].reverse()) // newest first
const key = (seq: number) => `${s.sat}:${store.value?.storeId ?? 0}:${seq}`
const shownRecs = computed(() =>
  s.shown.map((q) => records.get(key(q))).filter((r): r is IgnitionRecord => !!r),
)

let client: IgnlogClient | undefined
watch(
  () => props.smp,
  (smp) => {
    client = smp ? new IgnlogClient(smp) : undefined
    s.listing = null
    s.shown = []
    records.clear()
    if (smp && props.enabled) void refresh()
  },
  { immediate: true },
)
watch(
  () => props.enabled,
  (on) => {
    if (on && !s.listing) void refresh()
  },
)

async function run<T>(what: string, f: () => Promise<T>): Promise<T | undefined> {
  if (!client || s.working) return undefined
  s.working = what
  s.error = ''
  try {
    return await f()
  } catch (e) {
    s.error = (e as Error).message || String(e)
    return undefined
  } finally {
    s.working = ''
  }
}

async function refresh() {
  await run('Listing', async () => {
    let l = await client!.list(s.sat || undefined)
    if (isCtrl.value && !s.sat) {
      // Start on the first satellite the controller holds records of.
      s.sat = l.sats[0]?.sat ?? 1
      l = await client!.list(s.sat)
    }
    s.listing = l
  })
}

async function pickSat(sat: number) {
  if (sat === s.sat) return
  s.sat = sat
  s.shown = []
  s.window = null
  await refresh()
}

async function fetchFromSat() {
  await run('Downloading over LoRa', async () => {
    s.listing = await client!.fetch(s.sat, (l) => {
      s.listing = l
      const have = l.store?.records.filter((r) => r.have).length ?? 0
      s.working = `Downloading over LoRa · ${have} of ${l.store?.records.length ?? '?'} records`
    })
  })
}

async function load(e: Entry): Promise<boolean> {
  if (records.has(key(e.seq))) return true
  if (!e.have) {
    s.error = isCtrl.value
      ? `Record #${e.seq} is not on the controller yet: FETCH it from satellite ${SATS[s.sat - 1]}.`
      : `Record #${e.seq} is not complete.`
    return false
  }
  const r = await run(`Reading #${e.seq}`, () =>
    client!.read(e.seq, s.sat, (done, total) => (s.working = `Reading #${e.seq} · ${done} / ${total} bytes`)),
  )
  if (r) records.set(key(e.seq), r)
  return !!r
}

async function show(e: Entry) {
  if (await load(e)) {
    s.shown = [e.seq, ...s.shown.filter((q) => q !== e.seq)].slice(0, 1)
    s.window = null
  }
}

async function toggleOverlay(e: Entry) {
  if (s.shown.includes(e.seq)) {
    s.shown = s.shown.filter((q) => q !== e.seq)
  } else if (await load(e)) {
    s.shown = [...s.shown, e.seq]
  }
}

async function clearAll() {
  const what = isCtrl.value
    ? `Drop the controller's copies of satellite ${SATS[s.sat - 1]}'s records?`
    : 'Erase all ignition records on this satellite?'
  if (!confirm(what)) return
  await run('Clearing', () => client!.clear(s.sat))
  s.shown = []
  records.clear()
  await refresh()
}

function saveCsv() {
  const r = shownRecs.value[0]
  if (!r) return
  const a = document.createElement('a')
  a.href = URL.createObjectURL(new Blob([toCsv(r)], { type: 'text/csv' }))
  a.download = `ignition-${isCtrl.value ? `sat${SATS[s.sat - 1]}-` : ''}ch${r.channel + 1}-${r.seq}.csv`
  a.click()
  URL.revokeObjectURL(a.href)
}

// ---- texts ----

function amps(ma: number) {
  return `${(ma / 1000).toFixed(2)} A`
}

function peakMa(e: { peakCode: number }) {
  return (e.peakCode * (store.value?.scaleNa ?? 0)) / 1e6
}

function openText(e: Entry | IgnitionRecord) {
  if (e.flags & FLAG_NO_CURRENT) return 'no current'
  const o = openMs(e)
  return o === undefined ? 'never opened' : `opened ${o} ms`
}

function age(e: Entry) {
  const st = store.value
  if (!st || e.boot !== st.boots) return 'before a reboot'
  const sec = Math.max(0, Math.round((st.nowMs - e.tFireMs) / 1000))
  if (sec < 120) return `${sec} s ago`
  if (sec < 7200) return `${Math.round(sec / 60)} min ago`
  return `${Math.round(sec / 3600)} h ago`
}

function flags(f: number) {
  return [
    f & FLAG_FAULT ? 'fault' : '',
    f & FLAG_TRUNCATED ? 'cut short' : '',
    f & FLAG_OVERRUN ? 'overrun' : '',
  ].filter(Boolean)
}

// ---- chart ----

const W = 760
const H = 300
const M = { l: 58, r: 14, t: 14, b: 34 }
const PW = W - M.l - M.r
const PH = H - M.t - M.b

const span = computed<[number, number]>(() => {
  if (s.window) return s.window
  const end = Math.max(10, ...shownRecs.value.map((r) => (r.codes.length * r.periodUs) / 1000))
  return [0, end]
})

const fullScale = computed(() => {
  const peak = Math.max(0, ...shownRecs.value.map((r) => Math.max(...r.ma)))
  return niceCeil(Math.max(peak * 1.1, 500))
})

function niceCeil(v: number) {
  const p = 10 ** Math.floor(Math.log10(v))
  for (const m of [1, 2, 2.5, 5, 10]) if (m * p >= v) return m * p
  return 10 * p
}

function ticks(lo: number, hi: number, n: number) {
  const step = niceCeil((hi - lo) / n)
  const out: number[] = []
  for (let v = Math.ceil(lo / step) * step; v <= hi + 1e-9; v += step) out.push(v)
  return out
}

const x = (ms: number) => M.l + ((ms - span.value[0]) / (span.value[1] - span.value[0])) * PW
const y = (ma: number) => M.t + PH - (ma / fullScale.value) * PH

function path(r: IgnitionRecord) {
  const [t0, t1] = span.value
  const dt = r.periodUs / 1000
  const from = Math.max(0, Math.floor(t0 / dt) - 1)
  const to = Math.min(r.ma.length, Math.ceil(t1 / dt) + 2)
  let d = ''
  for (let i = from; i < to; i++) d += `${i === from ? 'M' : 'L'}${x(i * dt).toFixed(1)},${y(r.ma[i]).toFixed(1)}`
  return d
}

const xTicks = computed(() => ticks(span.value[0], span.value[1], 8))
const yTicks = computed(() => ticks(0, fullScale.value, 5))

const svg = ref<SVGSVGElement>()
const drag = reactive({ from: null as number | null, to: 0 })
const hover = ref<number | null>(null)

function msAt(ev: PointerEvent | MouseEvent) {
  const box = svg.value!.getBoundingClientRect()
  const px = ((ev.clientX - box.left) / box.width) * W
  const [t0, t1] = span.value
  return Math.min(t1, Math.max(t0, t0 + ((px - M.l) / PW) * (t1 - t0)))
}

function down(ev: PointerEvent) {
  if (!shownRecs.value.length) return
  drag.from = drag.to = msAt(ev)
  ;(ev.target as Element).setPointerCapture?.(ev.pointerId)
}

function move(ev: PointerEvent) {
  if (!shownRecs.value.length) return
  hover.value = msAt(ev)
  if (drag.from !== null) drag.to = hover.value
}

function up() {
  if (drag.from !== null && Math.abs(drag.to - drag.from) > (span.value[1] - span.value[0]) / 100) {
    s.window = [Math.min(drag.from, drag.to), Math.max(drag.from, drag.to)]
  }
  drag.from = null
}

const readout = computed(() => {
  const t = hover.value
  const r = shownRecs.value[0]
  if (t === null || !r) return ''
  const i = Math.round(t / (r.periodUs / 1000))
  if (i < 0 || i >= r.ma.length) return ''
  return `${((i * r.periodUs) / 1000).toFixed(1)} ms   ${amps(r.ma[i])}`
})

const DASH = ['', '6 4', '2 3', '10 3 2 3']
</script>

<template>
  <section class="ignlog">
    <div class="ruled"><span class="label">Ignition log</span></div>

    <div class="bar">
      <div v-if="isCtrl" class="sats" role="tablist" aria-label="Satellite">
        <button
          v-for="(n, i) in SATS"
          :key="n"
          class="sat"
          :class="{ on: s.sat === i + 1, has: s.listing?.sats.some((x) => x.sat === i + 1) }"
          role="tab"
          :aria-selected="s.sat === i + 1"
          :disabled="!!s.working || !enabled"
          @click="pickSat(i + 1)"
        >
          {{ n }}
        </button>
      </div>
      <span v-else class="label dim">This satellite's last pulses</span>
      <span class="actions">
        <button v-if="isCtrl" class="textbtn" :disabled="!!s.working || !enabled" @click="fetchFromSat">Fetch</button>
        <button class="textbtn" :disabled="!!s.working || !enabled" @click="refresh">Refresh</button>
        <button class="textbtn" :disabled="!shownRecs.length" @click="saveCsv">CSV</button>
        <button class="textbtn" :disabled="!!s.working || !enabled || !entries.length" @click="clearAll">Clear</button>
      </span>
    </div>

    <div class="grid">
      <div class="list">
        <p v-if="!entries.length" class="value dim empty">
          {{
            s.working
              ? ''
              : isCtrl && !store
                ? `Nothing from satellite ${SATS[s.sat - 1]} yet. FETCH downloads its records over LoRa.`
                : 'No ignitions recorded.'
          }}
        </p>
        <div
          v-for="e in entries"
          :key="e.seq"
          class="row"
          :class="{ front: s.shown[0] === e.seq, missing: !e.have }"
          @click="show(e)"
        >
          <input
            type="checkbox"
            :checked="s.shown.includes(e.seq)"
            :title="'Lay over the chart'"
            @click.stop="toggleOverlay(e)"
          />
          <span class="seq value">#{{ e.seq }}</span>
          <span class="label">CH{{ e.channel + 1 }}</span>
          <span class="value">{{ amps(peakMa(e)) }}</span>
          <span class="value dim">{{ openText(e) }}</span>
          <span class="value dim when">{{ age(e) }}</span>
          <span v-for="f in flags(e.flags)" :key="f" class="tag">{{ f }}</span>
        </div>
      </div>

      <figure class="chart">
        <svg
          ref="svg"
          :viewBox="`0 0 ${W} ${H}`"
          role="img"
          aria-label="Igniter current over time"
          @pointerdown="down"
          @pointermove="move"
          @pointerup="up"
          @pointerleave="hover = null"
          @dblclick="s.window = null"
        >
          <g class="grid-lines">
            <line v-for="t in xTicks" :key="'x' + t" :x1="x(t)" :x2="x(t)" :y1="M.t" :y2="M.t + PH" />
            <line v-for="t in yTicks" :key="'y' + t" :x1="M.l" :x2="M.l + PW" :y1="y(t)" :y2="y(t)" />
          </g>
          <rect class="frame" :x="M.l" :y="M.t" :width="PW" :height="PH" />
          <g class="ticks">
            <text
              v-for="(t, i) in xTicks"
              :key="'xt' + t"
              :x="i === xTicks.length - 1 && x(t) > M.l + PW - 30 ? M.l + PW : x(t)"
              :y="H - 12"
              :text-anchor="i === xTicks.length - 1 && x(t) > M.l + PW - 30 ? 'end' : 'middle'"
            >
              {{ +t.toFixed(1) }}{{ i === xTicks.length - 1 ? ' ms' : '' }}
            </text>
            <text v-for="t in yTicks" :key="'yt' + t" :x="M.l - 6" :y="y(t) + 4" text-anchor="end">
              {{ +(t / 1000).toFixed(2) }} A
            </text>
          </g>
          <clipPath id="ignlog-plot"><rect :x="M.l" :y="M.t" :width="PW" :height="PH" /></clipPath>
          <g clip-path="url(#ignlog-plot)">
            <path
              v-for="(r, i) in shownRecs"
              :key="r.seq"
              class="trace"
              :class="{ back: i > 0 }"
              :d="path(r)"
              :stroke-dasharray="DASH[i % DASH.length]"
            />
            <rect
              v-if="drag.from !== null"
              class="sel"
              :x="x(Math.min(drag.from, drag.to))"
              :y="M.t"
              :width="Math.abs(x(drag.to) - x(drag.from))"
              :height="PH"
            />
            <line v-if="hover !== null && drag.from === null" class="cursor" :x1="x(hover)" :x2="x(hover)" :y1="M.t" :y2="M.t + PH" />
          </g>
          <text v-if="!shownRecs.length" class="hint" :x="M.l + PW / 2" :y="M.t + PH / 2" text-anchor="middle">
            {{ entries.length ? 'Pick a record' : '' }}
          </text>
          <text v-if="readout" class="readout" :x="M.l + PW - 8" :y="M.t + 18" text-anchor="end">{{ readout }}</text>
        </svg>
        <figcaption v-if="shownRecs[0]" class="caption">
          <span class="value">
            #{{ shownRecs[0].seq }} · CH{{ shownRecs[0].channel + 1 }} · peak {{ amps(Math.max(...shownRecs[0].ma)) }} ·
            {{ openText(shownRecs[0]) }} · V_PYRO min {{ (shownRecs[0].vpyroMinMv / 1000).toFixed(1) }} V ·
            {{ shownRecs[0].periodUs / 1000 }} ms per sample
          </span>
          <span v-if="shownRecs.length > 1" class="legend">
            <span v-for="(r, i) in shownRecs" :key="r.seq" class="value">
              <svg width="26" height="8" aria-hidden="true">
                <line x1="1" x2="25" y1="4" y2="4" :stroke-dasharray="DASH[i % DASH.length]" />
              </svg>
              #{{ r.seq }} CH{{ r.channel + 1 }}
            </span>
          </span>
          <span class="zoom">
            <button class="textbtn" @click="s.window = [0, 50]">First 50 ms</button>
            <button class="textbtn" @click="s.window = null">Whole pulse</button>
          </span>
        </figcaption>
      </figure>
    </div>

    <p class="status value" :class="{ error: !!s.error }" aria-live="polite">{{ s.error || s.working }}</p>
  </section>
</template>

<style scoped>
.ignlog {
  margin-top: 28px;
  display: grid;
  gap: 14px;
}

.bar {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 16px;
  flex-wrap: wrap;
}

.sats {
  display: flex;
  gap: 6px;
}

.sat {
  width: 28px;
  height: 28px;
  border-radius: 50%;
  border: 1.5px solid var(--ink-3);
  background: none;
  color: var(--ink-2);
  font: 800 12px var(--sans);
  cursor: pointer;
}

.sat.has { border-color: var(--ink); color: var(--ink); }
.sat.on { background: var(--ink); color: var(--plate); border-color: var(--ink); }
.sat:disabled { cursor: default; }
.sat:focus-visible { outline: 2px solid var(--ink); outline-offset: 3px; }

.actions {
  display: flex;
  gap: 18px;
}

.grid {
  display: grid;
  grid-template-columns: minmax(0, 340px) minmax(0, 1fr);
  gap: 28px;
  align-items: start;
}

@media (max-width: 900px) {
  .grid { grid-template-columns: minmax(0, 1fr); }
}

.list {
  display: grid;
  gap: 2px;
}

.empty { margin: 6px 0; color: var(--ink-2); }

.row {
  display: grid;
  grid-template-columns: 18px 40px 38px 64px 1fr;
  grid-auto-flow: row;
  align-items: center;
  gap: 2px 8px;
  padding: 6px 8px;
  cursor: pointer;
  border-left: 2px solid transparent;
}

.row:hover { background: rgb(243 242 238 / 0.05); }
.row.front { border-left-color: var(--ink); background: rgb(243 242 238 / 0.08); }
.row.missing { opacity: 0.55; }
.row .when { grid-column: 2 / -1; font-size: 12px; }
.row .label { font-size: 11.5px; }
.row input { accent-color: var(--ink-2); margin: 0; }
.dim { color: var(--ink-2); }

.tag {
  grid-column: 2 / -1;
  justify-self: start;
  font: 800 9.5px/1 var(--sans);
  letter-spacing: 0.12em;
  text-transform: uppercase;
  padding: 3px 5px;
  background: var(--ink);
  color: var(--plate);
}

.chart {
  margin: 0;
  display: grid;
  gap: 8px;
}

svg {
  width: 100%;
  height: auto;
  background: var(--lcd);
  border-radius: 3px;
  box-shadow: 0 0 0 3px #111214, 0 0 0 4px var(--plate-edge);
  touch-action: none;
  user-select: none;
  cursor: crosshair;
}

.grid-lines line { stroke: var(--lcd-ink); stroke-opacity: 0.18; stroke-dasharray: 1 3; }
.frame { fill: none; stroke: var(--lcd-ink); stroke-width: 1.2; }
.ticks text { fill: var(--lcd-ink); font: 600 11px var(--mono); }
.trace { fill: none; stroke: var(--lcd-ink); stroke-width: 2; stroke-linejoin: round; }
.trace.back { stroke-width: 1.4; stroke-opacity: 0.75; }
.sel { fill: var(--lcd-ink); fill-opacity: 0.12; }
.cursor { stroke: var(--lcd-ink); stroke-opacity: 0.5; }
.hint { fill: var(--lcd-ink); fill-opacity: 0.55; font: 700 15px 'Noto Sans Condensed', var(--sans); }
.readout { fill: var(--lcd-ink); font: 700 13px var(--mono); }

.caption {
  display: flex;
  justify-content: space-between;
  gap: 16px;
  flex-wrap: wrap;
  align-items: baseline;
}

.caption .value { font-size: 12.5px; }

.zoom {
  display: flex;
  gap: 18px;
}

.legend {
  display: flex;
  gap: 16px;
  flex-wrap: wrap;
}

.legend svg {
  width: 26px;
  height: 8px;
  background: none;
  box-shadow: none;
  vertical-align: middle;
  cursor: default;
}

.legend line { stroke: var(--ink); stroke-width: 2; }

.status {
  margin: 0;
  min-height: 22px;
  color: var(--ink-2);
}

.status.error {
  color: var(--plate);
  background: var(--ink);
  padding: 2px 8px;
  justify-self: start;
}
</style>
