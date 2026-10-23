import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { PhonePairingPanel } from './PhonePairingPanel'

const send = vi.hoisted(() => vi.fn())
vi.mock('../../ipc/cefBridge', () => ({ isCompanionContext: () => false, sendToCEF: send }))
vi.mock('qrcode.react', () => ({ QRCodeSVG: ({ value }: { value: string }) => <svg aria-label="Pairing fixture" data-pairing={value} /> }))

let root: Root | undefined
let host: HTMLDivElement | undefined
afterEach(() => {
  if (root) act(() => root!.unmount())
  host?.remove(); root = undefined; host = undefined
  vi.useRealTimers(); vi.restoreAllMocks(); vi.clearAllMocks()
})

function render(enabled = true, url = 'https://uam.local:58950/companion') {
  host = document.createElement('div'); document.body.append(host)
  root = createRoot(host)
  act(() => root!.render(<PhonePairingPanel url={url} enabled={enabled} />))
  return host
}

describe('desktop native-phone pairing', () => {
  it('refuses a credential response for a different active publication route', async () => {
    send.mockResolvedValue({ ok: true, data: { token: 'a'.repeat(64), url: 'https://other.local/companion' } })
    const host = render()
    await act(async () => host.querySelector('button')!.click())
    expect(host.querySelector('svg[data-pairing]')).toBeNull()
    expect(host.textContent).toContain('changed')
  })

  it('carries only the public certificate supplied with the active route', async () => {
    const certificate = 'YWJjZA=='
    send.mockResolvedValue({ ok: true, data: { token: 'a'.repeat(64), url: 'https://uam.local:58950/companion', certificate } })
    const host = render()
    await act(async () => host.querySelector('button')!.click())
    expect(JSON.parse(host.querySelector('svg[data-pairing]')!.getAttribute('data-pairing')!).certificate).toBe(certificate)
  })

  it('requests no credential until explicitly revealed and encodes the shared connection', async () => {
    send.mockResolvedValue({ ok: true, data: { token: 'a'.repeat(64), url: 'https://uam.local:58950/companion' } })
    const host = render()
    expect(send).not.toHaveBeenCalled()
    await act(async () => host.querySelector('button')!.click())
    expect(send).toHaveBeenCalledExactlyOnceWith({ action: 'getCompanionToken' })
    expect(JSON.parse(host.querySelector('svg[data-pairing]')!.getAttribute('data-pairing')!)).toEqual({
      type: 'uam-companion', version: 1, url: 'https://uam.local:58950/companion', token: 'a'.repeat(64), name: 'Main Universal Agent Manager computer',
    })
    expect(host.textContent).not.toContain('a'.repeat(64))
    act(() => host.querySelector('button')!.click())
    expect(host.querySelector('svg[data-pairing]')).toBeNull()
  })

  it('does not reveal a code while publication is disabled or the route is insecure', () => {
    const host = render(false)
    expect(host.querySelector('button')!.disabled).toBe(true)
    expect(host.textContent).toContain('Available when phone access is ready.')
    act(() => root!.render(<PhonePairingPanel url="http://192.168.1.5/companion" enabled />))
    expect(host.querySelector('button')!.disabled).toBe(true)
    expect(send).not.toHaveBeenCalled()
  })

  it('rejects a route that the native endpoint cannot use', () => {
    const host = render(true, 'https://uam.local/unrelated-path')
    expect(host.querySelector('button')!.disabled).toBe(true)
    expect(send).not.toHaveBeenCalled()
  })

  it('discards an in-flight credential when the desktop becomes hidden', async () => {
    let resolve!: (value: unknown) => void
    send.mockImplementationOnce(() => new Promise((done) => { resolve = done }))
    const host = render()
    act(() => host.querySelector('button')!.click())
    const visibility = vi.spyOn(document, 'visibilityState', 'get').mockReturnValue('hidden')
    act(() => document.dispatchEvent(new Event('visibilitychange')))
    visibility.mockReturnValue('visible')
    await act(async () => resolve({ ok: true, data: { token: 'a'.repeat(64), url: 'https://uam.local:58950/companion' } }))
    expect(host.querySelector('svg[data-pairing]')).toBeNull()
    expect(host.querySelector('button')!.disabled).toBe(false)
  })

  it('discards an in-flight credential when the published route changes', async () => {
    let resolve!: (value: unknown) => void
    send.mockImplementationOnce(() => new Promise((done) => { resolve = done }))
    const host = render()
    act(() => host.querySelector('button')!.click())
    act(() => root!.render(<PhonePairingPanel url="https://other.local/companion" enabled />))
    await act(async () => resolve({ ok: true, data: { token: 'a'.repeat(64), url: 'https://uam.local:58950/companion' } }))
    expect(host.querySelector('svg[data-pairing]')).toBeNull()
  })

  it('hides the code after two minutes without claiming credential revocation', async () => {
    vi.useFakeTimers()
    send.mockResolvedValue({ ok: true, data: { token: 'a'.repeat(64), url: 'https://uam.local:58950/companion' } })
    const host = render()
    await act(async () => host.querySelector('button')!.click())
    expect(host.querySelector('svg[data-pairing]')).not.toBeNull()
    expect(host.textContent).toContain('does not revoke')
    act(() => vi.advanceTimersByTime(120000))
    expect(host.querySelector('svg[data-pairing]')).toBeNull()
  })

  it('shows a recoverable error for invalid credentials without rendering their contents', async () => {
    send.mockResolvedValue({ ok: true, data: { token: 'invalid-secret' } })
    const host = render()
    await act(async () => host.querySelector('button')!.click())
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Pairing code unavailable')
    expect(host.textContent).not.toContain('invalid-secret')
    expect(host.querySelector('svg[data-pairing]')).toBeNull()
  })
})
