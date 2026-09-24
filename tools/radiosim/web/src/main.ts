import { createApp } from 'vue'
import '@xterm/xterm/css/xterm.css'
import './app.css'
import App from './App.vue'
import { frames, serverNow, store } from './store'

// For poking at from the browser console.
Object.assign(window, { radiosim: { store, frames, serverNow } })

createApp(App).mount('#app')
