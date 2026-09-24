<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from 'vue'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import { wsUrl } from '../store'

const props = defineProps<{ id: number; active?: boolean }>()
const el = ref<HTMLDivElement>()
const state = ref<'connecting' | 'open' | 'closed'>('connecting')

let term: Terminal | undefined
let fit: FitAddon | undefined
let ws: WebSocket | undefined
let ro: ResizeObserver | undefined
let closed = false
let retry = 0

function connect() {
  ws = new WebSocket(wsUrl(`/ws/console/${props.id}`))
  ws.binaryType = 'arraybuffer'
  ws.onopen = () => {
    state.value = 'open'
    term?.reset()
  }
  ws.onmessage = (m) => term?.write(new Uint8Array(m.data as ArrayBuffer))
  ws.onclose = () => {
    state.value = 'closed'
    if (!closed) retry = window.setTimeout(connect, 1500)
  }
}

onMounted(() => {
  term = new Terminal({
    fontFamily: "ui-monospace, 'JetBrains Mono', Menlo, Consolas, monospace",
    fontSize: 12.5,
    scrollback: 20000,
    convertEol: false,
    cursorBlink: true,
    theme: { background: '#0b0f14', foreground: '#d6dde6', selectionBackground: '#264f78' },
  })
  fit = new FitAddon()
  term.loadAddon(fit)
  term.open(el.value!)
  const enc = new TextEncoder()
  term.onData((d) => ws?.readyState === WebSocket.OPEN && ws.send(enc.encode(d)))
  ro = new ResizeObserver(() => {
    if (el.value && el.value.offsetWidth > 0) fit?.fit()
  })
  ro.observe(el.value!)
  connect()
})

onBeforeUnmount(() => {
  closed = true
  clearTimeout(retry)
  ws?.close()
  ro?.disconnect()
  term?.dispose()
})

defineExpose({ focus: () => term?.focus() })
</script>

<template>
  <div class="term">
    <div ref="el" class="xt" />
    <span v-if="state !== 'open'" class="state">{{ state }}</span>
  </div>
</template>

<style scoped>
.term {
  position: relative;
  height: 100%;
  background: #0b0f14;
  padding: 4px 0 0 6px;
}
.xt {
  height: 100%;
}
.state {
  position: absolute;
  top: 4px;
  right: 8px;
  font-size: 11px;
  color: var(--warn);
}
</style>
