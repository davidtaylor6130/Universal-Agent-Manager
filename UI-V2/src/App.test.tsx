import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import App from './App'
import { useAppStore } from './store/useAppStore'

vi.mock('./components/layout/AppShell', () => ({ AppShell: () => null }))
;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

it('follows OS theme changes only in System mode and removes its listener on unmount', () => {
  const listeners = new Set<() => void>()
  const media = { matches: false, addEventListener: (_: string, listener: () => void) => listeners.add(listener), removeEventListener: (_: string, listener: () => void) => listeners.delete(listener) }
  vi.stubGlobal('matchMedia', () => media)
  const root = createRoot(document.createElement('div'))
  useAppStore.setState({ theme: 'system', customThemes: [] })
  try {
    act(() => root.render(<App />))
    expect(document.documentElement.getAttribute('data-theme')).toBe('light')
    act(() => { media.matches = true; listeners.forEach((listener) => listener()) })
    expect(document.documentElement.getAttribute('data-theme')).toBe('dark')
    expect(useAppStore.getState().theme).toBe('system')
    act(() => useAppStore.setState({ theme: 'paper' }))
    act(() => { media.matches = false; listeners.forEach((listener) => listener()) })
    expect(document.documentElement.getAttribute('data-theme')).toBe('paper')
  } finally {
    act(() => root.unmount())
    vi.unstubAllGlobals()
  }
  expect(listeners.size).toBe(0)
})
