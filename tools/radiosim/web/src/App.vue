<script setup lang="ts">
import { computed, onMounted } from 'vue'
import { connect, store } from './store'
import Toolbar from './components/Toolbar.vue'
import MapView from './components/MapView.vue'
import Timeline from './components/Timeline.vue'
import Inspector from './components/Inspector.vue'
import StatsPanel from './components/StatsPanel.vue'
import Dock from './components/Dock.vue'
import Terminal from './components/Terminal.vue'
import ControllerScreen from './components/ControllerScreen.vue'

// /shell/<id> and /screen/<id> are pop-out windows with just that view.
const popout = computed(() => {
  const m = location.pathname.match(/^\/(shell|screen)\/(\d+)/)
  return m ? { type: m[1] as 'shell' | 'screen', id: Number(m[2]) } : null
})

onMounted(() => {
  connect()
  if (popout.value) {
    const e = () => store.entities.get(popout.value!.id)?.name
    document.title = `${popout.value.type} · node ${popout.value.id}`
    const t = setInterval(() => {
      if (e()) {
        document.title = `${e()} ${popout.value!.type}`
        clearInterval(t)
      }
    }, 300)
  }
})
</script>

<template>
  <div v-if="popout" class="popout">
    <Terminal v-if="popout.type === 'shell'" :id="popout.id" />
    <ControllerScreen v-else :id="popout.id" />
  </div>
  <div v-else class="layout" :class="{ nodock: !store.dock.length }">
    <Toolbar class="top" />
    <MapView class="map" />
    <div class="side">
      <StatsPanel />
      <Inspector />
    </div>
    <Timeline class="timeline" />
    <Dock class="dock" />
    <div v-if="store.toast" class="toast" @click="store.toast = ''">{{ store.toast }}</div>
    <div v-if="!store.connected" class="offline">connecting to radiosim…</div>
  </div>
</template>

<style scoped>
.layout {
  display: grid;
  height: 100%;
  grid-template-columns: minmax(0, 1fr) 380px;
  grid-template-rows: auto minmax(220px, 38fr) minmax(170px, 27fr) minmax(200px, 35fr);
  grid-template-areas:
    'top top'
    'map side'
    'timeline side'
    'dock dock';
  gap: 1px;
  background: var(--line);
}
.layout.nodock {
  grid-template-rows: auto minmax(220px, 55fr) minmax(170px, 45fr) auto;
}
.top {
  grid-area: top;
}
.map {
  grid-area: map;
}
.side {
  grid-area: side;
  background: var(--panel);
  overflow: auto;
  min-height: 0;
}
.timeline {
  grid-area: timeline;
}
.dock {
  grid-area: dock;
}
.popout {
  height: 100%;
  padding: 6px;
}
.toast {
  position: fixed;
  bottom: 18px;
  left: 50%;
  transform: translateX(-50%);
  background: #7f1d1d;
  color: white;
  padding: 8px 14px;
  border-radius: 6px;
  cursor: pointer;
  z-index: 10;
  max-width: 70vw;
}
.offline {
  position: fixed;
  top: 8px;
  left: 50%;
  transform: translateX(-50%);
  background: var(--warn);
  color: black;
  padding: 3px 12px;
  border-radius: 12px;
  z-index: 10;
}
</style>
