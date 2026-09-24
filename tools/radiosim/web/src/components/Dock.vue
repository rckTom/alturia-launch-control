<script setup lang="ts">
import { nodeName, store } from '../store'
import type { DockTab } from '../types'
import ControllerScreen from './ControllerScreen.vue'
import Terminal from './Terminal.vue'

const key = (t: DockTab) => `${t.type}-${t.id}`

function close(i: number) {
  store.dock.splice(i, 1)
  if (store.dockSplit === i) store.dockSplit = -1
  else if (store.dockSplit > i) store.dockSplit--
  store.dockActive = Math.max(0, Math.min(store.dockActive - (store.dockActive >= i ? 1 : 0), store.dock.length - 1))
}

function toggleSplit(i: number) {
  store.dockSplit = store.dockSplit === i ? -1 : i
  if (store.dockActive === i && store.dockSplit === i) {
    store.dockActive = store.dock.findIndex((_, j) => j !== i)
  }
}

function popout(t: DockTab) {
  window.open(`/${t.type}/${t.id}`, key(t), 'width=900,height=560')
}

const shown = (i: number) => i === store.dockActive || i === store.dockSplit
</script>

<template>
  <section class="dock">
    <nav class="tabs">
      <span v-for="(t, i) in store.dock" :key="key(t)" class="tab"
        :class="{ active: i === store.dockActive, split: i === store.dockSplit }" @click="store.dockActive = i">
        <span class="ic">{{ t.type === 'shell' ? '›_' : '▣' }}</span>
        {{ nodeName(t.id) }} {{ t.type }}
        <button class="mini" title="Show side by side" @click.stop="toggleSplit(i)">◫</button>
        <button class="mini" title="Open in its own window" @click.stop="popout(t)">↗</button>
        <button class="mini" title="Close" @click.stop="close(i)">×</button>
      </span>
      <span v-if="!store.dock.length" class="muted hint">
        Shells and controller screens open here: a node's Shell or Screen button, or double-click it on the map.
      </span>
    </nav>
    <div class="panes">
      <div v-for="(t, i) in store.dock" v-show="shown(i)" :key="key(t)" class="pane"
        :style="{ order: i === store.dockSplit ? 2 : 1 }">
        <Terminal v-if="t.type === 'shell'" :id="t.id" />
        <ControllerScreen v-else :id="t.id" />
      </div>
    </div>
  </section>
</template>

<style scoped>
.dock {
  display: flex;
  flex-direction: column;
  background: var(--panel);
  min-height: 0;
}
.tabs {
  display: flex;
  gap: 2px;
  padding: 4px 6px 0;
  border-bottom: 1px solid var(--line);
  overflow-x: auto;
  flex-shrink: 0;
}
.tab {
  display: flex;
  align-items: center;
  gap: 4px;
  padding: 3px 4px 3px 10px;
  border: 1px solid transparent;
  border-bottom: none;
  border-radius: 5px 5px 0 0;
  cursor: pointer;
  white-space: nowrap;
  color: var(--muted);
}
.tab.active,
.tab.split {
  background: var(--panel-2);
  border-color: var(--line);
  color: var(--text);
}
.tab.split {
  box-shadow: inset 0 2px 0 var(--accent);
}
.ic {
  font-family: var(--mono);
  font-size: 11px;
  color: var(--accent);
}
.mini {
  padding: 0 5px;
  border: none;
  background: transparent;
  color: var(--muted);
}
.mini:hover {
  color: var(--text);
}
.hint {
  padding: 4px 6px 6px;
  font-size: 12px;
}
.panes {
  flex: 1;
  display: flex;
  min-height: 0;
  gap: 1px;
  background: var(--bg);
}
.pane {
  flex: 1;
  min-width: 0;
  background: var(--panel);
}
</style>
