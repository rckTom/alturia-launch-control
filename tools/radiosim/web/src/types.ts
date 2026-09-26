export interface Entity {
  id: number
  kind: 'node' | 'device'
  name: string
  x: number
  y: number
  tx_power_override: number | null
  // node
  role?: 'ctrl' | 'sat' | 'other'
  uid?: string
  pid?: number
  sf?: number
  leds?: number
  connected?: boolean
  isolated?: boolean
  managed?: boolean
  running?: boolean
  device_id?: number | null
  has_display?: boolean
  // device
  device_type?: 'jammer' | 'traffic' | 'radio'
  power_dbm?: number
  enabled?: boolean
  mode?: 'continuous' | 'pulsed'
  period_ms?: number
  duty?: number
  wideband?: boolean
  rate_hz?: number
  len_min?: number
  len_max?: number
}

export type Outcome =
  | 'ok'
  | 'offline'
  | 'cut'
  | 'half_duplex'
  | 'wrong_sf'
  | 'below_sens'
  | 'jammed'
  | 'collision'
  | 'random_loss'

export interface RxResult {
  node: number
  outcome: Outcome
  rssi: number | null
  snr: number | null
  sinr: number | null
}

export interface FrameInfo {
  kind?: string
  uid?: string
  net_tag?: string
  src?: number
  dst?: number
  kid?: number
  ctr?: number
}

export interface Frame {
  id: number
  src: number
  t_start: number // microseconds since the simulator started
  t_end: number | null // null while a continuous jammer is on
  sf: number
  power: number
  len: number | null
  hex: string | null
  info: FrameInfo
  wideband: boolean
  energy: boolean
  origin: string | null
  results: RxResult[] | null
}

export interface Link {
  a: number
  b: number
  extra_db: number
  cut: boolean
  loss_pct: number
}

export interface Rf {
  pl0_db: number
  exponent: number
  noise_figure_db: number
  bw_khz: number
  capture_db: number
  cross_sf_rejection_db: number
  loss_pct: number
  resolve_delay_us: number
  energy_cutoff_db: number
  noise_floor_dbm: number
}

export interface PollStats {
  net_tag: string
  ctrl: number
  polls: number
  period_ms: number | null
  replies: Record<string, number>
}

export interface JoinStats {
  net_tag: string
  ctrl: number | null
  slot: number
  attempts: number
  joins: number
  last_join_ms: number | null
  joining: boolean
}

export interface Stats {
  now: number
  util: Record<string, number>
  jam: Record<string, number>
  sources: Record<string, { frames_10s: number; airtime_pct: number }>
  outcomes_10s: Record<string, number>
  links: { src: number; dst: number; ok: number; total: number; pdr: number }[]
  polls: PollStats[]
  joins: JoinStats[]
}

export type DockTab = { type: 'shell' | 'screen'; id: number }
