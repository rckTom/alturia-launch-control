// Colours shared by the map, the timeline and the panels.
import type { Outcome } from './types'

export const kindColor: Record<string, string> = {
  POLL: '#3b82f6',
  DATA: '#14b8a6',
  HELLO: '#a855f7',
  JOIN_REQ: '#a855f7',
  JOIN_RSP: '#a855f7',
  JOIN_OK: '#8b5cf6',
  JOIN_ACK: '#8b5cf6',
  GROUP_UPDATE: '#6366f1',
  GROUP_ACK: '#6366f1',
  PAIR_ADV: '#f59e0b',
  PAIR_REQ: '#f97316',
  PAIR_RSP: '#f97316',
  PAIR_NC: '#f97316',
  PAIR_NS: '#f97316',
  PAIR_CHK_C: '#ea580c',
  PAIR_CHK_S: '#ea580c',
  PAIR_ABORT: '#dc2626',
  FOREIGN: '#78716c',
  EMPTY: '#78716c',
  ENERGY: '#ef4444',
}

export function frameColor(kind: string | undefined, energy: boolean): string {
  if (energy) return kindColor.ENERGY
  return kindColor[kind ?? 'FOREIGN'] ?? '#78716c'
}

export const outcomeColor: Record<Outcome, string> = {
  ok: '#22c55e',
  collision: '#ef4444',
  jammed: '#d946ef',
  below_sens: '#94a3b8',
  half_duplex: '#f59e0b',
  random_loss: '#eab308',
  cut: '#64748b',
  wrong_sf: '#475569',
  offline: '#334155',
}

export const outcomeLabel: Record<Outcome, string> = {
  ok: 'received',
  collision: 'collision',
  jammed: 'jammed',
  below_sens: 'too weak',
  half_duplex: 'was sending',
  random_loss: 'random loss',
  cut: 'link cut',
  wrong_sf: 'other SF',
  offline: 'offline',
}

export function pdrColor(pdr: number): string {
  if (pdr >= 0.95) return '#22c55e'
  if (pdr >= 0.7) return '#eab308'
  return '#ef4444'
}
