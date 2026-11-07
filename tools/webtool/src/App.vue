<!-- SPDX-License-Identifier: GPL-3.0-only -->
<script setup lang="ts">
import { computed, ref } from 'vue'
import Lcd from './components/Lcd.vue'
import Cap from './components/Cap.vue'
import Lamp from './components/Lamp.vue'
import Segments from './components/Segments.vue'
import IgnitionLog from './components/IgnitionLog.vue'
import {
  BOARD_NAMES,
  activeSlot,
  boardName,
  cancelUpload,
  connect,
  disconnect,
  fallbackSlot,
  kind,
  loadFile,
  published,
  publishedStatus,
  rollBack,
  smpClient,
  state,
  updateLocal,
  updatePublished,
} from './device'
import { hex } from './smp/mcuboot'

const connected = computed(() => ['shell', 'ready', 'busy', 'rebooting'].includes(state.phase))
const ready = computed(() => state.phase === 'ready')
const working = computed(() => state.phase === 'busy' || state.phase === 'rebooting' || state.phase === 'connecting')
const deviceName = computed(() => BOARD_NAMES[boardName()] ?? (state.target || '—'))
const active = computed(() => activeSlot())
const pub = computed(() => published())
const status = computed(() => publishedStatus())
const fallback = computed(() => fallbackSlot())

const lcdRight = computed(() => {
  if (state.phase === 'rebooting') return 'RESTARTING'
  if (state.phase === 'shell') return 'NO MCUMGR'
  if (state.phase === 'ready' || state.phase === 'busy') {
    return `${deviceName.value.toUpperCase()}  ${active.value?.version ?? ''}`.trim()
  }
  return 'NOT CONNECTED'
})

const statusWord = computed(() => {
  switch (status.value) {
    case 'current':
      return 'installed'
    case 'newer':
      return 'newer'
    case 'older':
      return 'older'
    case 'different':
      return 'differs'
    default:
      return ''
  }
})

// Only forward: an older image would undo fixes, as the satellite found out.
const canUpdate = computed(() => ready.value && status.value === 'newer')
const localFits = computed(
  () => !!state.local && (!state.local.image.board || state.local.image.board === boardName()),
)

const fileInput = ref<HTMLInputElement>()
function pickFile() {
  fileInput.value?.click()
}
function onFile(ev: Event) {
  const f = (ev.target as HTMLInputElement).files?.[0]
  if (f) void loadFile(f)
  ;(ev.target as HTMLInputElement).value = ''
}

function toggleConnection() {
  if (connected.value) void disconnect()
  else void connect()
}

function short(h: Uint8Array) {
  return hex(h, 4)
}
</script>

<template>
  <main class="plate">
    <header class="head">
      <h1 class="title">ALTURIA</h1>
      <div class="ruled sub"><span class="label">Launch Control</span></div>
      <p class="label dim purpose">Service · firmware and shell over USB</p>
    </header>

    <div class="body">
      <Lcd class="lcd" :title="state.phase === 'shell' || ready ? 'SHELL' : 'SERVICE'" :right="lcdRight" :live="connected">
        <template v-if="state.phase === 'unsupported'">
          <div class="splash-title">NO SERIAL PORTS</div>
          <div>This browser cannot open USB serial ports.<br />Open this page in Chrome or Edge on a computer.</div>
        </template>
        <template v-else-if="state.phase === 'connecting'">
          <div class="splash-title">OPENING THE PORT</div>
        </template>
        <template v-else>
          <div class="splash-title">ALTURIA</div>
          <div>Plug the controller or a satellite into USB-C,<br />then press CONNECT.</div>
        </template>
      </Lcd>

      <aside class="controls">
        <div class="lamps">
          <Lamp label="Link" :on="connected" />
          <Lamp label="Mcumgr" :on="ready || state.phase === 'busy'" />
          <Lamp label="Busy" :on="working" blink />
        </div>

        <div class="caps">
          <Cap
            ring
            :label="connected ? 'Disconnect' : 'Connect'"
            :disabled="state.phase === 'unsupported' || working"
            @press="toggleConnection"
          />
          <Cap strong label="Update" :disabled="!canUpdate" @press="updatePublished" />
        </div>

        <dl class="readout">
          <div><dt class="label dim">Device</dt><dd class="value">{{ connected ? deviceName : '—' }}</dd></div>
          <div><dt class="label dim">Firmware</dt><dd class="value">{{ active?.version ?? '—' }}</dd></div>
          <div>
            <dt class="label dim">Published</dt>
            <dd class="value">
              {{ pub?.version ?? '—' }}
              <span v-if="statusWord" class="word">{{ statusWord }}</span>
            </dd>
          </div>
          <div><dt class="label dim">Boot</dt><dd class="value">{{ state.bootloader || '—' }}</dd></div>
        </dl>

        <section v-if="state.progress" class="progress">
          <div class="label">{{ state.progress.stage }}</div>
          <Segments :done="state.progress.done" :total="state.progress.total" />
          <div class="progress-foot">
            <span class="value">{{ Math.round((state.progress.done / Math.max(1, state.progress.total)) * 100) }} %</span>
            <button v-if="state.progress.stage === 'Uploading'" class="textbtn" @click="cancelUpload">Cancel</button>
          </div>
        </section>

        <section v-if="state.slots.length" class="slots">
          <div class="ruled small"><span class="label">Slots</span></div>
          <div v-for="s in state.slots" :key="s.slot" class="slot">
            <span class="slot-no">{{ s.slot }}</span>
            <span class="value">{{ s.version }}</span>
            <span class="value hash" :title="hex(s.hash)">{{ short(s.hash) }}</span>
            <span class="tags">
              <span v-if="s.active" class="tag">active</span>
              <span v-if="s.confirmed" class="tag">confirmed</span>
              <span v-if="s.pending" class="tag open">test</span>
              <span v-if="s.permanent" class="tag open">permanent</span>
            </span>
          </div>
          <button
            v-if="fallback && ready"
            class="textbtn"
            :title="`Boot ${fallback.version} again; it stays on test until it confirms itself.`"
            @click="rollBack"
          >
            Go back to {{ fallback.version }}
          </button>
        </section>

        <section class="file">
          <div class="ruled small"><span class="label">Image file</span></div>
          <p v-if="state.local" class="value file-line">
            {{ state.local.name }}<br />
            <span class="dim">
              {{ state.local.image.board ? (BOARD_NAMES[state.local.image.board] ?? state.local.image.board) : 'untagged' }}
              · {{ state.local.image.version }} · {{ short(state.local.image.hash) }}
            </span>
          </p>
          <div class="file-actions">
            <button class="textbtn" :disabled="working" @click="pickFile">Choose file</button>
            <button v-if="state.local" class="textbtn" :disabled="!ready || !localFits" @click="updateLocal">
              Install
            </button>
          </div>
          <input ref="fileInput" type="file" accept=".bin" hidden @change="onFile" />
        </section>
      </aside>
    </div>

    <IgnitionLog
      v-if="(ready || state.phase === 'busy') && kind() !== 'other'"
      :smp="smpClient()"
      :kind="kind()"
      :enabled="ready"
    />

    <footer class="message" :class="{ error: state.message?.error }" aria-live="polite">
      {{ state.message?.text ?? state.manifestNote ?? '' }}
    </footer>
  </main>

  <p class="colophon">
    Chrome or Edge, over Web Serial · nothing leaves this page
    <template v-if="state.manifest?.commit"> · firmware from {{ state.manifest.commit }}</template>
    · <a href="https://github.com/rcktom/alturia-launch-control">source</a>
  </p>
</template>

<style scoped>
.plate {
  width: 100%;
  max-width: 1180px;
  background: var(--plate);
  border-radius: 34px;
  padding: 30px 38px 22px;
  box-shadow:
    inset 0 1px 0 rgb(255 255 255 / 0.04),
    0 1px 0 #0b0c0d,
    0 30px 60px rgb(0 0 0 / 0.45);
}

/* ---- the engraved header ---- */

.head {
  text-align: center;
  margin-bottom: 26px;
}

.title {
  margin: 0;
  font-weight: 700;
  font-size: clamp(34px, 5vw, 50px);
  line-height: 1;
  letter-spacing: 0.42em;
  /* tracking also adds space after the last letter: pull it back */
  margin-right: -0.42em;
}

.sub {
  margin: 12px auto 0;
  max-width: 560px;
}

.sub .label {
  font-size: clamp(15px, 2vw, 19px);
  letter-spacing: 0.16em;
}

.purpose {
  margin: 12px 0 0;
  font-size: 11.5px;
}

/* ---- body ---- */

.body {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 330px;
  gap: 38px;
  align-items: stretch;
}

.lcd {
  height: clamp(440px, 66vh, 680px);
}

.splash-title {
  font-size: 30px;
  letter-spacing: 0.32em;
  margin-right: -0.32em;
}

.controls {
  display: flex;
  flex-direction: column;
  gap: 22px;
  min-width: 0;
}

.lamps {
  display: flex;
  justify-content: flex-end;
  gap: 26px;
}

.caps {
  display: flex;
  justify-content: space-between;
  align-items: flex-end;
  padding: 0 4px;
}

.readout {
  margin: 0;
  display: grid;
  gap: 7px;
}

.readout div {
  display: flex;
  justify-content: space-between;
  align-items: baseline;
  gap: 12px;
}

.readout dt { font-size: 11.5px; }
.readout dd { margin: 0; text-align: right; }

.word {
  font: 800 10.5px var(--sans);
  letter-spacing: var(--track);
  text-transform: uppercase;
  color: var(--ink-2);
  margin-left: 6px;
}

.ruled.small { gap: 12px; }
.ruled.small::before,
.ruled.small::after { height: 1.5px; background: var(--ink-3); }
.ruled.small .label { font-size: 11.5px; }

.progress {
  display: grid;
  gap: 8px;
}

.progress-foot {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.slots,
.file {
  display: grid;
  gap: 10px;
  justify-items: start;
}

.slots .ruled,
.file .ruled { width: 100%; }

.slot {
  display: grid;
  grid-template-columns: 22px auto auto 1fr;
  align-items: center;
  gap: 10px;
  width: 100%;
}

.slot-no {
  display: grid;
  place-items: center;
  width: 22px;
  height: 22px;
  border: 1.5px solid var(--ink);
  border-radius: 50%;
  font: 800 11px var(--sans);
}

.hash { color: var(--ink-2); }

.tags {
  display: flex;
  flex-wrap: wrap;
  gap: 4px;
  justify-content: flex-end;
}

.tag {
  font: 800 9.5px/1 var(--sans);
  letter-spacing: 0.12em;
  text-transform: uppercase;
  padding: 3px 5px;
  background: var(--ink);
  color: var(--plate);
}

.tag.open {
  background: none;
  color: var(--ink);
  box-shadow: inset 0 0 0 1.5px var(--ink);
}

.file-line {
  margin: 0;
  word-break: break-all;
}

.file-line .dim { color: var(--ink-2); }

.file-actions {
  display: flex;
  gap: 18px;
}

/* ---- the message line along the bottom edge ---- */

.message {
  margin-top: 24px;
  min-height: 30px;
  padding: 5px 12px;
  font-weight: 500;
  font-size: 14px;
  color: var(--ink-2);
  border-top: 1.5px solid var(--ink-3);
}

.message.error {
  background: var(--ink);
  color: var(--plate);
  border-top-color: var(--ink);
  font-weight: 700;
}

.colophon {
  margin: 16px 0 0;
  font-size: 12.5px;
  color: var(--ink-3);
  text-align: center;
}

.colophon a {
  color: var(--ink-2);
}

@media (max-width: 900px) {
  .plate {
    padding: 24px 18px 18px;
    border-radius: 24px;
  }
  .body {
    grid-template-columns: 1fr;
    gap: 28px;
  }
  .lcd {
    height: 52vh;
    min-height: 320px;
  }
}
</style>
