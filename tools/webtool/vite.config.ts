// SPDX-License-Identifier: GPL-3.0-only
import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// Relative asset paths: the site is served from a subpath on GitHub Pages
// (https://rcktom.github.io/alturia-launch-control/).
export default defineConfig({
  base: './',
  plugins: [vue()],
})
