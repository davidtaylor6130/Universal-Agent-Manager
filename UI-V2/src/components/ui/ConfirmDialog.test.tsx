import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { ConfirmDialog } from './ConfirmDialog'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function mount(busy = false) {
  const opener = document.createElement('button')
  const host = document.createElement('div')
  document.body.append(opener, host)
  opener.focus()
  const root = createRoot(host)
  const cancel = vi.fn()
  const render = (nextBusy: boolean) => act(() => root.render(<ConfirmDialog open title="Delete chat?" confirmLabel="Delete" onConfirm={vi.fn()} onCancel={cancel} busy={nextBusy}>Saved chat</ConfirmDialog>))
  render(busy)
  return { host, opener, cancel, render, close: () => { act(() => root.unmount()); const restored = document.activeElement === opener; host.remove(); opener.remove(); return restored } }
}

function key(key: string, shiftKey = false) {
  const event = new KeyboardEvent('keydown', { key, shiftKey, bubbles: true, cancelable: true })
  act(() => document.activeElement?.dispatchEvent(event))
  return event
}

describe('ConfirmDialog keyboard ownership', () => {
  it('wraps forward and backward focus inside the dialog', () => {
    const view = mount()
    const buttons = document.querySelector('[role="alertdialog"]')!.querySelectorAll('button')
    expect(document.activeElement).toBe(buttons[0])
    key('Tab', true)
    expect(document.activeElement).toBe(buttons[1])
    key('Tab')
    expect(document.activeElement).toBe(buttons[0])
    view.close()
  })

  it('keeps focus and consumes Escape while an operation is busy', () => {
    const view = mount()
    view.render(true)
    const dialog = document.querySelector('[role="alertdialog"]')
    expect(document.activeElement).toBe(dialog)
    expect(key('Escape').defaultPrevented).toBe(true)
    expect(view.cancel).not.toHaveBeenCalled()
    key('Tab')
    expect(document.activeElement).toBe(dialog)
    view.close()
  })

  it('cancels idle dialogs and restores the opener on close', () => {
    const view = mount()
    key('Escape')
    expect(view.cancel).toHaveBeenCalledOnce()
    expect(view.close()).toBe(true)
  })
})
