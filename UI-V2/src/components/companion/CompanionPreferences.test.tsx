import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import { CompanionPreferences } from './CompanionPreferences'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

it('labels the theme menu with its visible Theme text and opens it from that label', () => {
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  const alerts = { enabled: false, setEnabled: vi.fn(), browserEnabled: false, enableBrowser: vi.fn(), disableBrowser: vi.fn(), notificationError: '' } as unknown as Parameters<typeof CompanionPreferences>[0]['alerts']
  act(() => root.render(<CompanionPreferences alerts={alerts} />))
  const label = Array.from(host.querySelectorAll('label')).find((item) => item.textContent?.startsWith('Theme'))!
  const trigger = host.querySelector<HTMLButtonElement>('[role="combobox"]')!
  expect(label.control).toBe(trigger)
  expect(trigger.getAttribute('aria-label')).toBe('Theme')
  // No label in the preferences is left without a control.
  expect(Array.from(host.querySelectorAll('label')).every((item) => item.control !== null)).toBe(true)
  act(() => label.querySelector('span')!.click())
  expect(trigger.getAttribute('aria-expanded')).toBe('true')
  act(() => root.unmount())
  host.remove()
})
