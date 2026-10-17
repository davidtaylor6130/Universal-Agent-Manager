import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { WindowChrome, useWindowChrome } from './WindowChrome'
import { APP_FAILURE_EVENT } from '../../utils/appFailure'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function Shell() {
  const chrome = useWindowChrome()
  return <div data-platform={chrome.state?.platform}>{chrome.state && <WindowChrome state={chrome.state} command={chrome.command} />}</div>
}

describe('native window controls', () => {
  let host: HTMLDivElement
  let root: Root
  let maximized: boolean
  let fullscreen: boolean
  let failCommand: boolean
  let requests: Array<{ action: string; payload?: { command: string } }>

  beforeEach(() => {
    host = document.createElement('div')
    document.body.append(host)
    root = createRoot(host)
    maximized = false
    fullscreen = false
    failCommand = false
    requests = []
    vi.spyOn(navigator, 'platform', 'get').mockReturnValue('Win32')
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (parsed.action === 'windowCommand') {
        if (failCommand) { onFailure(503, 'Window unavailable.'); return }
        if (parsed.payload.command === 'toggleMaximize') maximized = !maximized
      }
      onSuccess(JSON.stringify({ platform: navigator.platform === 'MacIntel' ? 'macos' : 'windows', maximized, fullscreen }))
    }
  })

  afterEach(() => {
    act(() => root.unmount())
    host.remove()
    delete window.cefQuery
    vi.restoreAllMocks()
  })

  const mount = async () => { await act(async () => root.render(<Shell />)) }
  const click = async (label: string) => {
    const button = host.querySelector<HTMLButtonElement>(`button[aria-label="${label}"]`)
    expect(button).toBeTruthy()
    await act(async () => button!.click())
  }

  it('dispatches minimize, maximize, restore and close to the native window', async () => {
    await mount()
    await click('Minimize window')
    await click('Maximize window')
    expect(host.querySelector('[aria-label="Restore window"]')).toBeTruthy()
    await click('Restore window')
    expect(host.querySelector('[aria-label="Maximize window"]')).toBeTruthy()
    await click('Close window')
    expect(requests.filter(r => r.action === 'windowCommand').map(r => r.payload?.command))
      .toEqual(['minimize', 'toggleMaximize', 'toggleMaximize', 'close'])
  })

  it('updates restore and fullscreen state after an operating-system resize', async () => {
    await mount()
    maximized = true
    await act(async () => window.dispatchEvent(new Event('resize')))
    expect(host.querySelector('[aria-label="Restore window"]')).toBeTruthy()
    fullscreen = true
    await act(async () => window.dispatchEvent(new Event('resize')))
    expect(host.querySelector('[aria-label="Window controls"]')).toBeNull()
    fullscreen = false
    await act(async () => window.dispatchEvent(new Event('focus')))
    expect(host.querySelector('[aria-label="Restore window"]')).toBeTruthy()
  })

  it('leaves macOS traffic lights to the operating system', async () => {
    vi.spyOn(navigator, 'platform', 'get').mockReturnValue('MacIntel')
    await mount()
    expect(host.querySelector('[data-platform="macos"]')).toBeTruthy()
    expect(host.querySelector('[data-testid="window-chrome"]')).toBeTruthy()
    expect(host.querySelector('button')).toBeNull()
  })

  it('rechecks native state after the last resize of a window transition', async () => {
    vi.useFakeTimers()
    try {
      await mount()
      await act(async () => window.dispatchEvent(new Event('resize')))
      maximized = true
      await act(async () => vi.advanceTimersByTimeAsync(350))
      expect(host.querySelector('[aria-label="Restore window"]')).toBeTruthy()
      fullscreen = true
      await act(async () => window.dispatchEvent(new Event('uam-window-changed')))
      expect(host.querySelector('[aria-label="Window controls"]')).toBeNull()
    } finally { vi.useRealTimers() }
  })

  it('does not add desktop controls or bridge requests in a standalone browser', async () => {
    delete window.cefQuery
    await mount()
    expect(host.querySelector('[data-testid="window-chrome"]')).toBeNull()
    expect(requests).toEqual([])
  })

  it('never exposes desktop controls through the phone transport', async () => {
    const previous = window.location.href
    window.history.replaceState({}, '', '/companion')
    try {
      await mount()
      expect(host.querySelector('[data-testid="window-chrome"]')).toBeNull()
      expect(requests).toEqual([])
    } finally { window.history.replaceState({}, '', previous) }
  })

  it('keeps Linux system window decorations', async () => {
    vi.spyOn(navigator, 'platform', 'get').mockReturnValue('Linux x86_64')
    await mount()
    expect(host.querySelector('[data-testid="window-chrome"]')).toBeNull()
    expect(requests).toEqual([])
  })

  it('reports native command failure through the existing notification channel', async () => {
    const failure = vi.fn()
    window.addEventListener(APP_FAILURE_EVENT, failure)
    try {
      await mount()
      failCommand = true
      await click('Maximize window')
      expect(failure).toHaveBeenCalledOnce()
      expect(host.querySelector('[aria-label="Maximize window"]')).toBeTruthy()
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Maximize window"]')?.disabled).toBe(false)
    } finally { window.removeEventListener(APP_FAILURE_EVENT, failure) }
  })
})
