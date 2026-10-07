import { act, lazy, Suspense } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { InterfaceErrorBoundary } from './InterfaceErrorBoundary'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

afterEach(() => vi.restoreAllMocks())

describe('interface failure containment', () => {
  it('preserves the chat when a lazy modal import rejects and permits dismissal', async () => {
    vi.spyOn(console, 'error').mockImplementation(() => {})
    let reject!: (reason: Error) => void
    const Modal = lazy(() => new Promise<never>((_resolve, fail) => { reject = fail }))
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    const dismiss = vi.fn()
    await act(async () => root.render(<><main>Existing chat</main><InterfaceErrorBoundary panel onDismiss={dismiss}><Suspense fallback={null}><Modal /></Suspense></InterfaceErrorBoundary></>))
    expect(host.textContent).toContain('Existing chat')
    await act(async () => reject(new TypeError('Failed to fetch dynamically imported module')))
    expect(host.querySelector('main')?.textContent).toBe('Existing chat')
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('This panel could not load.')
    const button = Array.from(host.querySelectorAll('button')).find((item) => item.textContent === 'Dismiss')!
    act(() => button.click())
    expect(dismiss).toHaveBeenCalledOnce()
    act(() => root.unmount())
    host.remove()
  })

  it('keeps root recovery controls visible after a render error', async () => {
    vi.spyOn(console, 'error').mockImplementation(() => {})
    function Broken(): never { throw new Error('Render failure') }
    const host = document.createElement('div')
    const root = createRoot(host)
    await act(async () => root.render(<InterfaceErrorBoundary><Broken /></InterfaceErrorBoundary>))
    expect(host.textContent).toContain('The interface could not load.')
    expect(host.querySelector('button')?.textContent).toBe('Reload interface')
    expect(host.textContent).toContain('Unsaved drafts may be lost')
    act(() => root.unmount())
  })
})
