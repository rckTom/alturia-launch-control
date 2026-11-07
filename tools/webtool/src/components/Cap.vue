<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!--
  A round grey cap with its engraved label, like FILL, VENT, ARM and FIRE;
  `ring` draws the circle the D-pad sits in around it.
-->
<script setup lang="ts">
defineProps<{ label: string; disabled?: boolean; ring?: boolean; strong?: boolean; below?: boolean }>()
defineEmits<{ press: [] }>()
</script>

<template>
  <div class="cap-wrap" :class="{ below }">
    <span class="label cap-label" :class="{ strong, dim: disabled }">{{ label }}</span>
    <div class="seat" :class="{ ring }">
      <button class="cap" :disabled="disabled" :aria-label="label" @click="$emit('press')" />
    </div>
  </div>
</template>

<style scoped>
.cap-wrap {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 12px;
}

.cap-wrap.below {
  flex-direction: column-reverse;
}

.cap-label {
  font-size: 15px;
  letter-spacing: 0.1em;
}

.cap-label.strong {
  font-weight: 900;
  font-size: 19px;
}

/* Every seat is ring-sized, so caps side by side line up. */
.seat {
  display: grid;
  place-items: center;
  width: 128px;
  height: 128px;
  border-radius: 50%;
}

.seat.ring {
  border: 2px solid var(--ink);
}

.cap {
  width: 76px;
  height: 76px;
  border-radius: 50%;
  border: 0;
  cursor: pointer;
  background: radial-gradient(circle at 50% 38%, var(--cap-hi), var(--cap) 58%, var(--cap-lo));
  box-shadow:
    0 0 0 3px #17181a,
    0 4px 0 #111214,
    0 6px 14px rgb(0 0 0 / 0.45);
  transition: transform 60ms, box-shadow 60ms;
}

.cap:hover:not(:disabled) {
  background: radial-gradient(circle at 50% 38%, #b2b3b7, var(--cap-hi) 58%, var(--cap));
}

.cap:active:not(:disabled) {
  transform: translateY(3px);
  box-shadow:
    0 0 0 3px #17181a,
    0 1px 0 #111214,
    0 2px 6px rgb(0 0 0 / 0.45);
}

.cap:disabled {
  cursor: default;
  background: radial-gradient(circle at 50% 38%, #4a4b4f, #3c3d41 60%, #333438);
  box-shadow:
    0 0 0 3px #17181a,
    0 2px 0 #111214;
}

.cap:focus-visible {
  outline: 2px solid var(--ink);
  outline-offset: 6px;
}
</style>
