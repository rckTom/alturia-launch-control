<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- Progress as a row of segments, the way a 1-bit display would draw it. -->
<script setup lang="ts">
import { computed } from 'vue'

const props = defineProps<{ done: number; total: number; count?: number }>()
const n = computed(() => props.count ?? 24)
const lit = computed(() => (props.total > 0 ? Math.round((props.done / props.total) * n.value) : 0))
</script>

<template>
  <div class="segments" role="progressbar" :aria-valuenow="done" :aria-valuemax="total">
    <span v-for="i in n" :key="i" :class="{ lit: i <= lit }" />
  </div>
</template>

<style scoped>
.segments {
  display: grid;
  grid-auto-flow: column;
  grid-auto-columns: 1fr;
  gap: 3px;
  height: 20px;
  padding: 3px;
  border: 2px solid var(--ink);
}

.segments span {
  background: transparent;
}

.segments span.lit {
  background: var(--ink);
}
</style>
