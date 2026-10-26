import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, expect, it, vi } from 'vitest'
import { AppearanceDisplaySettings } from './AppearanceDisplaySettings'

afterEach(() => vi.unstubAllGlobals())

it('turns model token limits off and persists the choice', () => {
  const values = new Map<string, string>()
  vi.stubGlobal('localStorage', { getItem: (key: string) => values.get(key) ?? null, setItem: (key: string, value: string) => values.set(key, value), removeItem: (key: string) => values.delete(key) })
  const host = document.createElement('div'); document.body.append(host)
  const root = createRoot(host)
  act(() => root.render(<AppearanceDisplaySettings />))
  const toggle = host.querySelector<HTMLInputElement>('input[aria-label="Show model token limits"]')!
  expect(toggle.checked).toBe(true)
  act(() => toggle.click())
  expect(JSON.parse(values.get('uam-appearance')!).modelTokenLimits).toBe(false)
  act(() => root.unmount()); host.remove()
})
