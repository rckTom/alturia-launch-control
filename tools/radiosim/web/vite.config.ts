import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// `npm run dev` serves the UI with hot reload and forwards the API and the
// WebSockets to a running `radiosim` (default port 8077).
// eslint-disable-next-line @typescript-eslint/no-explicit-any
const backend: string = (globalThis as any).process?.env?.RADIOSIM ?? 'http://127.0.0.1:8077'

export default defineConfig({
  plugins: [vue()],
  server: {
    proxy: {
      '/api': backend,
      '/ws': { target: backend.replace(/^http/, 'ws'), ws: true },
    },
  },
})
