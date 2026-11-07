<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!--
  The shell, drawn as the controller's Sharp memory LCD: a reflective grey
  window, dark pixels, and the controller UI's inverted title bar in its
  condensed bold font. ANSI colours fold into shades of the LCD ink.
-->
<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import { onTerminalText, typeText } from '../device'
import { demo } from '../demo'

const props = defineProps<{ title: string; right: string; live: boolean }>()

const el = ref<HTMLDivElement>()
let term: Terminal | undefined
let fit: FitAddon | undefined
let ro: ResizeObserver | undefined

const ink = '#1c1d1f'
const soft = '#45464a'

onMounted(async () => {
  await document.fonts.load('13px "Noto Sans Mono"').catch(() => {})
  term = new Terminal({
    fontFamily: '"Noto Sans Mono", ui-monospace, monospace',
    fontSize: 13,
    lineHeight: 1.18,
    cursorBlink: true,
    cursorStyle: 'block',
    scrollback: 5000,
    allowTransparency: true,
    theme: {
      background: 'rgba(0,0,0,0)',
      foreground: ink,
      cursor: ink,
      cursorAccent: '#d8d9d3',
      selectionBackground: 'rgba(28,29,31,0.25)',
      black: ink,
      red: ink,
      green: ink,
      yellow: soft,
      blue: ink,
      magenta: ink,
      cyan: soft,
      white: soft,
      brightBlack: soft,
      brightRed: ink,
      brightGreen: ink,
      brightYellow: soft,
      brightBlue: ink,
      brightMagenta: ink,
      brightCyan: soft,
      brightWhite: ink,
    },
  })
  fit = new FitAddon()
  term.loadAddon(fit)
  term.open(el.value!)
  fit.fit()
  term.onData((d) => typeText(d))
  onTerminalText((b) => term?.write(b))
  ro = new ResizeObserver(() => fit?.fit())
  ro.observe(el.value!)
  if (location.hash === '#demo') demo((t) => term?.write(t))
})

watch(
  () => props.live,
  (live) => {
    if (live) term?.focus()
  },
)

onBeforeUnmount(() => {
  ro?.disconnect()
  term?.dispose()
})
</script>

<template>
  <section class="lcd" :class="{ live }">
    <header class="bar">
      <span>{{ title }}</span>
      <span>{{ right }}</span>
    </header>
    <div class="screen">
      <div ref="el" class="term" :class="{ hidden: !live }" />
      <div v-if="!live" class="splash">
        <slot />
      </div>
    </div>
  </section>
</template>

<style scoped>
.lcd {
  display: flex;
  flex-direction: column;
  min-height: 0;
  background: var(--lcd);
  color: var(--lcd-ink);
  border-radius: 3px;
  /* the bezel: the panel's cut-out edge and a little depth */
  box-shadow:
    0 0 0 3px #111214,
    0 0 0 4px var(--plate-edge),
    inset 0 3px 10px rgb(0 0 0 / 0.28);
  position: relative;
  overflow: hidden;
}

/* Faint pixel grid, as a memory LCD shows when you look closely. */
.lcd::after {
  content: '';
  position: absolute;
  inset: 0;
  pointer-events: none;
  background-image:
    repeating-linear-gradient(0deg, rgb(0 0 0 / 0.035) 0 1px, transparent 1px 3px),
    repeating-linear-gradient(90deg, rgb(0 0 0 / 0.035) 0 1px, transparent 1px 3px);
}

.bar {
  display: flex;
  justify-content: space-between;
  gap: 12px;
  padding: 3px 10px 4px;
  background: var(--lcd-ink);
  color: var(--lcd);
  font: 700 15px/1.25 'Noto Sans Condensed', var(--sans);
  letter-spacing: 0.02em;
  white-space: nowrap;
  overflow: hidden;
}

.screen {
  flex: 1;
  min-height: 0;
  position: relative;
  padding: 8px 4px 6px 10px;
}

.term {
  position: absolute;
  inset: 8px 4px 6px 10px;
}

.term.hidden {
  visibility: hidden;
}

.splash {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  text-align: center;
  padding: 24px;
  gap: 14px;
  font: 700 17px/1.35 'Noto Sans Condensed', var(--sans);
}
</style>
