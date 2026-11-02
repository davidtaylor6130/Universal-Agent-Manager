import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it } from 'vitest'
import { UsageGauge } from './UsageGauge'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function render(usedPercent: number | null) {
  const host = document.createElement('div')
  const root = createRoot(host)
  act(() => root.render(<UsageGauge usedPercent={usedPercent} />))
  return { host, close: () => act(() => root.unmount()) }
}

describe('UsageGauge', () => {
  it.each([[0, -90], [33, -30.6], [50, 0], [66, 28.8], [100, 90], [140, 90], [-5, -90]])('points the needle at %s%% used', (used, degrees) => {
    const view = render(used)
    try {
      const needle = view.host.querySelector<SVGGElement>('[data-needle]')!
      expect(Number(needle.style.transform.match(/rotate\((-?[\d.]+)deg\)/)![1])).toBeCloseTo(degrees, 5)
      expect(needle.style.transition).toBe('transform var(--dur-base) var(--ease-out-soft)')
      const meter = view.host.querySelector('[role="meter"]')!
      expect(meter.getAttribute('aria-valuenow')).toBe(String(Math.min(100, Math.max(0, used))))
      expect(meter.getAttribute('aria-valuemin')).toBe('0')
      expect(meter.getAttribute('aria-valuemax')).toBe('100')
    } finally { view.close() }
  })

  it('splits the arc into equal success, warning and error thirds from left to right', () => {
    const view = render(10)
    try {
      const bands = [...view.host.querySelectorAll('[data-band]')]
      expect(bands.map(band => band.getAttribute('stroke'))).toEqual(['var(--success)', 'var(--warning)', 'var(--error)'])
      // Band joins sit at 60° and 120° on a radius-9 dial centred at (12, 12).
      expect(bands.map(band => band.getAttribute('d'))).toEqual([
        'M 3.00 12.00 A 9 9 0 0 1 7.50 4.21',
        'M 7.50 4.21 A 9 9 0 0 1 16.50 4.21',
        'M 16.50 4.21 A 9 9 0 0 1 21.00 12.00',
      ])
    } finally { view.close() }
  })

  it.each([null, Number.NaN])('shows a decorative neutral arc without a needle when usage is %s', (used) => {
    const view = render(used)
    try {
      const svg = view.host.querySelector('svg')!
      expect(svg.getAttribute('aria-hidden')).toBe('true')
      expect(svg.getAttribute('role')).toBeNull()
      expect(view.host.querySelector('[data-needle]')).toBeNull()
      expect([...svg.querySelectorAll('path')].map(path => path.getAttribute('stroke'))).toEqual(['var(--text-3)'])
    } finally { view.close() }
  })
})
