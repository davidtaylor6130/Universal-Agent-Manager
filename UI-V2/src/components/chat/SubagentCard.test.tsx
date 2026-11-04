import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import { SubagentCard } from './SubagentCard'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

it('announces unknown provider/summary/time and opens without submitting a containing form or nesting actions', () => {
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  const open = vi.fn(); const submit = vi.fn()
  try {
    act(() => root.render(<form onSubmit={event => { event.preventDefault(); submit() }}><SubagentCard title="Worker" status="Running" working aria-label="Open Worker" onClick={open} /></form>))
    const button = host.querySelector<HTMLButtonElement>('button')!
    const descriptions = button.getAttribute('aria-describedby')!.split(' ').map(id => document.getElementById(id)!.textContent).join(' ')
    expect(descriptions).toContain('Provider unavailable')
    expect(descriptions).toContain('No summary reported')
    expect(host.querySelector('[aria-label="Timing unavailable"]')).not.toBeNull()
    expect(button.querySelector('img')).toBeNull()
    expect(button.querySelector('.lucide-bot')).not.toBeNull()
    expect(button.querySelector('.uam-working-ring')).not.toBeNull()
    expect(button.querySelector('button')).toBeNull()
    act(() => button.click())
    expect(open).toHaveBeenCalledExactlyOnceWith(expect.anything())
    expect(submit).not.toHaveBeenCalled()
    act(() => root.render(<SubagentCard title="Worker" status="Completed" summary="Recorded output" elapsed="9s" disabled />))
    expect(host.querySelector('.uam-working-ring')).toBeNull()
    expect(host.querySelector('[aria-label="Elapsed 9s"]')?.textContent).toBe('9s')
    expect(host.querySelector<HTMLButtonElement>('button')!.disabled).toBe(true)
  } finally { act(() => root.unmount()); host.remove() }
})
