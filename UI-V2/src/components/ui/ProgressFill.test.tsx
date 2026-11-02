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

  it('meter variant: a bar with role meter, value text, tone and a marker tick; unknown keeps an empty track with no role', () => {
    const { host, root } = render(createElement(ProgressFill, { variant: 'meter', label: 'Weekly', value: 70, marker: 40, tone: 'warning', valueText: '70% used, ahead of pace' }))
    const meter = host.querySelector('[role="meter"]') as HTMLElement
    expect(meter.getAttribute('aria-valuenow')).toBe('70')
    expect(meter.getAttribute('aria-valuetext')).toBe('70% used, ahead of pace')
    expect(meter.dataset.tone).toBe('warning')
    expect(meter.style.getPropertyValue('--progress')).toBe('0.7')
    expect((meter.querySelector('.uam-meter__marker') as HTMLElement).style.getPropertyValue('--marker')).toBe('0.4')
    act(() => root.render(createElement(ProgressFill, { variant: 'meter', label: 'Weekly', active: false })))
    expect(host.querySelector('[role]')).toBeNull()
    expect(host.querySelector('.uam-meter')?.getAttribute('aria-hidden')).toBe('true')
    expect((host.querySelector('.uam-meter') as HTMLElement).dataset.indeterminate).toBeUndefined()
    act(() => root.render(createElement(ProgressFill, { variant: 'meter', label: 'Weekly', indeterminate: true })))
    expect((host.querySelector('.uam-meter') as HTMLElement).dataset.indeterminate).toBe('true')
    act(() => root.unmount()); host.remove()
  })
})
