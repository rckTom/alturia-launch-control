<script setup lang="ts">
import { computed } from 'vue'
import { frames, store } from '../store'
import ChannelPanel from './ChannelPanel.vue'
import DevicePanel from './DevicePanel.vue'
import FrameDetail from './FrameDetail.vue'
import NodePanel from './NodePanel.vue'

const entity = computed(() =>
  store.selected?.kind === 'entity' ? store.entities.get(store.selected.id) : undefined,
)
const frame = computed(() => {
  void store.frameVersion
  return store.selected?.kind === 'frame' ? frames.byId.get(store.selected.id) : undefined
})
</script>

<template>
  <section class="insp">
    <NodePanel v-if="entity && entity.kind === 'node'" :key="'n' + entity.id" :node="entity" />
    <DevicePanel v-else-if="entity" :key="'d' + entity.id" :dev="entity" />
    <FrameDetail v-else-if="frame" :key="'f' + frame.id + '-' + store.frameVersion" :frame="frame" />
    <ChannelPanel v-else />
  </section>
</template>

<style scoped>
.insp {
  padding: 10px 12px;
}
</style>
