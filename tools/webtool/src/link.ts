// SPDX-License-Identifier: GPL-3.0-only
//
// One USB serial port, shared by the terminal and SMP. Everything the
// device sends goes through StreamDemux: shell output to the terminal,
// SMP answers to the client. Keys typed into the terminal and SMP requests
// go out through the same writer, one write per frame, so they never
// interleave inside a frame.

import { StreamDemux } from './smp/frame'
import { SmpClient } from './smp/smp'

export interface LinkEvents {
  text(bytes: Uint8Array): void
  closed(reason: string): void
  bad?(why: string): void
}

export class Link {
  readonly smp: SmpClient
  private reader?: ReadableStreamDefaultReader<Uint8Array>
  private writer?: WritableStreamDefaultWriter<Uint8Array>
  private demux: StreamDemux
  private closing = false
  private writes: Promise<void> = Promise.resolve()

  private constructor(
    readonly port: SerialPort,
    private readonly ev: LinkEvents,
  ) {
    this.smp = new SmpClient((b) => this.write(b))
    this.demux = new StreamDemux(
      (t) => ev.text(t),
      (p) => this.smp.receive(p),
      (why) => ev.bad?.(why),
    )
  }

  static async open(port: SerialPort, ev: LinkEvents): Promise<Link> {
    // The baud rate means nothing to either USB port, but 1200 would be
    // the satellite's request for the ROM DFU bootloader.
    await port.open({ baudRate: 115200, bufferSize: 4096 })
    // DTR on: the satellite's shell only talks to an open terminal.
    // RTS off: on the controller's USB Serial/JTAG, RTS without DTR
    // resets the chip.
    try {
      await port.setSignals({ dataTerminalReady: true, requestToSend: false })
    } catch {
      // Some platforms refuse control lines on CDC ACM; the shell then
      // starts on its own once it sees traffic.
    }
    const link = new Link(port, ev)
    link.writer = port.writable!.getWriter()
    void link.readLoop()
    return link
  }

  private async readLoop() {
    let reason = 'disconnected'
    try {
      while (this.port.readable && !this.closing) {
        this.reader = this.port.readable.getReader()
        try {
          for (;;) {
            const { value, done } = await this.reader.read()
            if (done) break
            if (value) this.demux.push(value)
          }
        } finally {
          this.reader.releaseLock()
        }
      }
    } catch (e) {
      reason = (e as Error).message || reason
    }
    this.smp.abort('the device went away')
    if (!this.closing) {
      await this.release()
      this.ev.closed(reason)
    }
  }

  /** Writes are queued so a frame always goes out in one piece. */
  write(bytes: Uint8Array): Promise<void> {
    const w = this.writer
    if (!w) return Promise.reject(new Error('not connected'))
    const next = this.writes.then(() => w.write(bytes))
    this.writes = next.catch(() => {})
    return next
  }

  writeText(s: string): Promise<void> {
    return this.write(new TextEncoder().encode(s))
  }

  private async release() {
    try {
      this.writer?.releaseLock()
    } catch {
      /* already released */
    }
    this.writer = undefined
    try {
      await this.port.close()
    } catch {
      /* the device is gone already */
    }
  }

  async close() {
    this.closing = true
    this.smp.abort('disconnected')
    try {
      await this.reader?.cancel()
    } catch {
      /* nothing to cancel */
    }
    await this.release()
    this.ev.closed('closed')
  }
}

export const serialSupported = typeof navigator !== 'undefined' && 'serial' in navigator

/** USB IDs of the two boards' ports, for the port chooser's filter. */
export const USB_FILTERS: SerialPortFilter[] = [
  // ESP32-S3 USB Serial/JTAG (controller)
  { usbVendorId: 0x303a, usbProductId: 0x1001 },
  // Zephyr's default CDC ACM IDs (satellite)
  { usbVendorId: 0x2fe3 },
]
