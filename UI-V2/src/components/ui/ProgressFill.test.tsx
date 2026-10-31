import { act, createElement } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it } from 'vitest'
import { ProgressFill } from './ProgressFill'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function render(node: React.ReactElement) {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(node))
  return { host, root, bar: () => host.querySelector('[role="progressbar"]') as HTMLElement }
}

describe('ProgressFill', () => {
  it('exposes a determinate percentage and drives the fill with --progress', () => {
    const { host, root, bar } = render(createElement(ProgressFill, { label: 'Import', value: 40 }, 'Importing 2 of 5'))
    expect(bar().classList.contains('uam-progress-fill')).toBe(true)
    expect(bar().getAttribute('aria-label')).toBe('Import')
    expect(bar().getAttribute('aria-valuemin')).toBe('0')
    expect(bar().getAttribute('aria-valuemax')).toBe('100')
    expect(bar().getAttribute('aria-valuenow')).toBe('40')
    expect(bar().hasAttribute('aria-busy')).toBe(false)
    expect(bar().style.getPropertyValue('--progress')).toBe('0.4')
    expect(bar().textContent).toBe('Importing 2 of 5')
    act(() => root.unmount()); host.remove()
  })

  it('sweeps with aria-busy and no value when no percentage is known', () => {
    const { host, root, bar } = render(createElement(ProgressFill, { label: 'Deleting', indeterminate: true }))
    expect(bar().hasAttribute('aria-valuenow')).toBe(false)
    expect(bar().getAttribute('aria-busy')).toBe('true')
    expect(bar().dataset.indeterminate).toBe('true')
    expect(bar().style.getPropertyValue('--progress')).toBe('')
    act(() => root.unmount()); host.remove()
  })
})
