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

  it('only the top dialog owns Escape and Tab when confirmations are stacked', () => {
    const lower = mount()
    const upper = mount()
    try {
      const dialogs = document.querySelectorAll('[role="alertdialog"]')
      const upperButtons = dialogs[1].querySelectorAll('button')
      expect(document.activeElement).toBe(upperButtons[0])
      key('Tab', true)
      expect(document.activeElement).toBe(upperButtons[1])
      key('Escape')
      expect(upper.cancel).toHaveBeenCalledOnce()
      expect(lower.cancel).not.toHaveBeenCalled()
    } finally {
      upper.close()
      lower.close()
    }
  })

  it('does not steal focus from a newer dialog when an underlying dialog unmounts', () => {
    const lower = mount()
    const upper = mount()
    const focused = document.activeElement
    lower.close()
    try {
      expect(document.activeElement).toBe(focused)
    } finally {
      upper.close()
    }
  })

  it('does not steal upper dialog focus when an underlying operation becomes busy', () => {
    const lower = mount()
    const upper = mount()
    try {
      const focused = document.activeElement
      lower.render(true)
      expect(document.activeElement).toBe(focused)
    } finally {
      upper.close()
      lower.close()
    }
  })

  it('keeps focus in an existing higher layer when a lower confirmation mounts', () => {
    const upper = document.createElement('div')
    upper.setAttribute('role', 'dialog')
    upper.setAttribute('aria-modal', 'true')
    upper.style.zIndex = '1000'
    const button = document.createElement('button')
    upper.append(button)
    const host = document.createElement('div')
    document.body.append(upper, host)
    button.focus()
    const root = createRoot(host)
    try {
      act(() => root.render(<ConfirmDialog open title="Lower" confirmLabel="Confirm" onConfirm={vi.fn()} onCancel={vi.fn()}>Lower layer</ConfirmDialog>))
      expect(document.activeElement).toBe(button)
    } finally {
      act(() => root.unmount())
      host.remove()
      upper.remove()
    }
  })

})
