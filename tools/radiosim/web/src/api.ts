async function call<T = unknown>(method: string, path: string, body?: unknown): Promise<T> {
  const res = await fetch(path, {
    method,
    headers: body !== undefined ? { 'Content-Type': 'application/json' } : undefined,
    body: body !== undefined ? JSON.stringify(body) : undefined,
  })
  if (!res.ok) {
    let msg = `${res.status} ${res.statusText}`
    try {
      const j = await res.json()
      if (j.detail) msg = typeof j.detail === 'string' ? j.detail : JSON.stringify(j.detail)
    } catch {
      /* not JSON */
    }
    throw new Error(msg)
  }
  return res.json() as Promise<T>
}

export const api = {
  addNode: (role: 'ctrl' | 'sat', start = true) => call('POST', '/api/nodes', { role, start }),
  startNode: (id: number) => call('POST', `/api/nodes/${id}/start`),
  killNode: (id: number) => call('POST', `/api/nodes/${id}/kill`),
  restartNode: (id: number) => call('POST', `/api/nodes/${id}/restart`),
  removeNode: (id: number) => call('DELETE', `/api/nodes/${id}`),
  button: (id: number, pressed: boolean, index = 0) =>
    call('POST', `/api/nodes/${id}/button`, { index, pressed }),
  input: (id: number, type: 'pin' | 'wheel' | 'touch', code: number, value: number) =>
    call('POST', `/api/nodes/${id}/input`, { type, code, value }),
  patchEntity: (id: number, patch: Record<string, unknown>) =>
    call('PATCH', `/api/entities/${id}`, patch),
  setLink: (a: number, b: number, extra_db: number, cut: boolean, loss_pct: number) =>
    call('PUT', '/api/links', { a, b, extra_db, cut, loss_pct }),
  addDevice: (device_type: string, params: Record<string, unknown>) =>
    call('POST', '/api/devices', { device_type, params }),
  updateDevice: (id: number, params: Record<string, unknown>) =>
    call('PATCH', `/api/devices/${id}`, params),
  removeDevice: (id: number) => call('DELETE', `/api/devices/${id}`),
  inject: (src: number, hex: string, sf?: number) => call('POST', '/api/inject', { src, hex, sf }),
  replay: (frame_id: number, src: number) => call('POST', '/api/replay', { frame_id, src }),
  setRf: (params: Record<string, unknown>) => call('PUT', '/api/rf', params),
  saveScenario: () => call<{ path: string }>('POST', '/api/scenario/save', {}),
  loadScenario: (scenario?: unknown, autostart = false) =>
    call('POST', '/api/scenario/load', scenario ? { scenario, autostart } : { autostart }),
}
