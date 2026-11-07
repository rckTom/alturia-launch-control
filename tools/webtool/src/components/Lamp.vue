<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- An LED with its label above, like STATUS and CHG on the panel. -->
<script setup lang="ts">
defineProps<{ label: string; on?: boolean; blink?: boolean }>()
</script>

<template>
  <div class="lamp" :title="`${label}: ${on ? 'on' : 'off'}`">
    <span class="label">{{ label }}</span>
    <span class="led" :class="{ on, blink: on && blink }" />
  </div>
</template>

<style scoped>
.lamp {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 9px;
}

.lamp .label {
  font-size: 11.5px;
}

.led {
  width: 11px;
  height: 11px;
  border-radius: 50%;
  background: #121315;
  box-shadow: inset 0 1px 2px rgb(0 0 0 / 0.8), 0 0 0 1px var(--ink-3);
}

.led.on {
  background: var(--ink);
  box-shadow: 0 0 8px rgb(243 242 238 / 0.55), 0 0 0 1px var(--ink);
}

.led.blink {
  animation: blink 0.9s steps(2, jump-none) infinite;
}

@keyframes blink {
  50% {
    background: #121315;
    box-shadow: inset 0 1px 2px rgb(0 0 0 / 0.8), 0 0 0 1px var(--ink-3);
  }
}

@media (prefers-reduced-motion: reduce) {
  .led.blink { animation: none; }
}
</style>
