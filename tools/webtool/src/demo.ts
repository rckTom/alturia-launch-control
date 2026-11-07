// SPDX-License-Identifier: GPL-3.0-only
//
// index.html#demo: the connected view with made-up data, for looking at
// the layout without a device.

import { state } from './device'

export function demo(write: (s: string) => void) {
  const h = (s: string) => new Uint8Array(s.match(/../g)!.map((x) => parseInt(x, 16)))
  state.phase = 'ready'
  state.target = 'alturia_satellite/stm32g474xx'
  state.bootloader = 'MCUboot'
  state.bufSize = 640
  state.slots = [
    { image: 0, slot: 0, version: '0.1.0', hash: h('4878553792c37a47'), bootable: true, pending: false, confirmed: true, active: true, permanent: false },
    { image: 0, slot: 1, version: '0.0.9', hash: h('9c01a7e2554f0b13'), bootable: true, pending: false, confirmed: false, active: false, permanent: false },
  ]
  state.manifest = {
    commit: 'd59d007',
    images: [{ board: 'alturia_satellite', version: '0.2.0', hash: 'ab', size: 204539, file: 'alturia_satellite-0.2.0.bin' }],
  }
  state.progress = { stage: 'Uploading', done: 131072, total: 204539 }
  write(
    '\x1b[1;32muart:~$ \x1b[msat status\r\n' +
      'state     SAFE, key off, relay off\r\n' +
      'link      joined, slot 1, rssi -61 dBm\r\n' +
      'battery   12.41 V\r\n' +
      'ch  armed  continuity\r\n' +
      ' 1  -      1.9 ohm\r\n' +
      ' 2  -      open\r\n' +
      ' 3  -      2.1 ohm\r\n' +
      '[00:01:12.410,000] \x1b[1;33m<wrn> satellite: the console stopped polling\x1b[0m\r\n' +
      '\x1b[1;32muart:~$ \x1b[m',
  )
}
