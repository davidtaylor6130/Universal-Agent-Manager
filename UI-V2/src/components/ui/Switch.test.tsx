import { readFileSync } from 'node:fs'
import { act, createElement } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it } from 'vitest'
import { Switch } from './Switch'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

// Only the switch rules: jsdom's CSS parser does not need the rest of the sheet.
const switchCss = readFileSync('src/index.css', 'utf8').split('\n').filter((line) => line.startsWith('.uam-switch')).join('\n')

// Fresh render per state: jsdom does not recompute styles when only the checked property changes.
function renderSwitch(checked: boolean) {
  const style = document.createElement('style')
  style.textContent = switchCss
  document.head.appendChild(style)
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(createElement(Switch, { label: 'Enable Git', defaultChecked: checked })))
  const icon = (name: string) => getComputedStyle(host.querySelector(`.uam-switch__thumb .uam-switch__icon--${name}`)!).opacity
  return { host, shown: [icon('on'), icon('off')], cleanup: () => { act(() => root.unmount()); host.remove(); style.remove() } }
}

describe('Switch', () => {
  it('keeps checkbox semantics with the label and a hidden track', () => {
    const { host, cleanup } = renderSwitch(false)
    const input = host.querySelector('input')!
    expect(input.type).toBe('checkbox')
    expect(input.getAttribute('aria-label')).toBe('Enable Git')
    expect(host.querySelector('.uam-switch__track')!.getAttribute('aria-hidden')).toBe('true')
    cleanup()
  })

  it('shows a red X in the thumb when off and a green tick when on', () => {
    const off = renderSwitch(false)
    expect(off.shown).toEqual(['0', '1'])
    off.cleanup()
    const on = renderSwitch(true)
    expect(on.shown).toEqual(['1', '0'])
    on.cleanup()
  })
})
