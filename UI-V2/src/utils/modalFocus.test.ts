import { afterEach, describe, expect, it } from 'vitest'
import { trapModalTab } from './modalFocus'

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
})
