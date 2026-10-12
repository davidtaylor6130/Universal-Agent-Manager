import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import { NumberField } from './NumberField'
;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

it('commits clamped values on blur only, and reverts empty input', () => {
  const onCommit = vi.fn()
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(<NumberField label="Delay" value={60} min={30} max={3600} onCommit={onCommit} />))
  const input = host.querySelector('input')!
  const type = (value: string) => act(() => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, value)
    input.dispatchEvent(new Event('input', { bubbles: true }))
  })
  type('5')
  expect(onCommit).not.toHaveBeenCalled()
  expect(input.getAttribute('aria-invalid')).toBe('true')
  act(() => { input.focus(); input.blur() })
  expect(onCommit).toHaveBeenLastCalledWith(30)
  type('')
  act(() => { input.focus(); input.blur() })
  expect(input.value).toBe('60')
  expect(onCommit).toHaveBeenCalledTimes(1)
  act(() => root.unmount())
  host.remove()
})
