import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { SelectionGrid, selectionOptionDescription } from './SelectionGrid'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const render = (onChange = vi.fn()) => {
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  act(() => root.render(<SelectionGrid label="Runs on" value="local" onChange={onChange} options={[
    { id: 'local', label: 'This computer', icon: null },
    { id: 'lab', label: 'Lab', icon: null, detail: 'lab-alias', tone: 'warning', status: 'Offline', disabled: true, issues: ['Remote runner is offline. Open Settings to configure this computer.'] },
    { id: 'busy', label: 'Busy', icon: null, disabled: true },
  ]} />))
  const option = (name: string) => host.querySelector<HTMLButtonElement>(`button[aria-label="${name}"]`)!
  const description = (name: string) => document.getElementById(option(name).getAttribute('aria-describedby') ?? '')?.textContent ?? null
  return { option, description, cleanup: () => { act(() => root.unmount()); host.remove() } }
}

describe('SelectionGrid', () => {
  it('reads the detail, the status words and why an option is unavailable as its description', () => {
    const { option, description, cleanup } = render()
    expect(description('Lab')).toBe('lab-alias. Offline. Remote runner is offline. Open Settings to configure this computer.')
    expect(option('This computer').hasAttribute('aria-describedby')).toBe(false)
    expect(description('This computer')).toBeNull()
    cleanup()
  })

  it('keeps an explained unavailable option focusable but inert, and natively disables one without a reason', () => {
    const onChange = vi.fn()
    const { option, cleanup } = render(onChange)
    const lab = option('Lab')
    expect(lab.disabled).toBe(false)
    expect(lab.getAttribute('aria-disabled')).toBe('true')
    lab.focus()
    expect(document.activeElement).toBe(lab)
    act(() => lab.click())
    expect(onChange).not.toHaveBeenCalled()
    expect(option('Busy').disabled).toBe(true)
    act(() => option('This computer').click())
    expect(onChange).toHaveBeenCalledWith('local')
    cleanup()
  })

  it('ends every part as a sentence without doubling punctuation or repeating an issue', () => {
    expect(selectionOptionDescription({ detail: ' ', status: 'Ready', issues: ['Sign in.', 'Sign in.', ''] })).toBe('Ready. Sign in.')
    expect(selectionOptionDescription({})).toBe('')
  })
})
