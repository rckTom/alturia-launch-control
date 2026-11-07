// SPDX-License-Identifier: GPL-3.0-only
import { createApp } from 'vue'
import '@xterm/xterm/css/xterm.css'
import './style.css'
import App from './App.vue'
import { init } from './device'

init()
createApp(App).mount('#app')
