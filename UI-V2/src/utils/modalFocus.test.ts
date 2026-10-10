import { afterEach, describe, expect, it } from 'vitest'
import { getTopmostModal, trapModalTab } from './modalFocus'

afterEach(() => { document.body.innerHTML = '' })

describe('trapModalTab', () => {
  it('keeps keyboard focus inside the topmost modal', () => {
    document.body.innerHTML = `
      <button id="background">Background</button>
      <div aria-modal="true"><button id="first">First</button><button id="last">Last</button></div>
    `
    const background = document.querySelector<HTMLButtonElement>('#background')!
    const first = document.querySelector<HTMLButtonElement>('#first')!
    const last = document.querySelector<HTMLButtonElement>('#last')!

    background.focus()
    trapModalTab(new KeyboardEvent('keydown', { key: 'Tab', cancelable: true }))
    expect(document.activeElement).toBe(first)

    last.focus()
    trapModalTab(new KeyboardEvent('keydown', { key: 'Tab', cancelable: true }))
    expect(document.activeElement).toBe(first)

    first.focus()
    trapModalTab(new KeyboardEvent('keydown', { key: 'Tab', shiftKey: true, cancelable: true }))
    expect(document.activeElement).toBe(last)
  })

  it('keeps the visually highest layer active even if a lower dialog mounts later', () => {
    document.body.innerHTML = `
      <div style="z-index:1000"><section id="diff" aria-modal="true"><button id="diff-action">Close diff</button></section></div>
      <div style="z-index:70"><section id="confirmation" aria-modal="true"><button>Cancel</button></section></div>
    `
    expect(getTopmostModal()?.id).toBe('diff')
    trapModalTab(new KeyboardEvent('keydown', { key: 'Tab', cancelable: true }))
    expect(document.activeElement?.id).toBe('diff-action')
  })

  it('ignores an exiting overlay and leaves keyboard ownership with the open dialog', () => {
    document.body.innerHTML = `
      <section id="open" aria-modal="true"><button>Open</button></section>
      <div data-state="closed" style="z-index:1000"><section aria-modal="true"><button>Exiting</button></section></div>
    `
    expect(getTopmostModal()?.id).toBe('open')
  })

})
