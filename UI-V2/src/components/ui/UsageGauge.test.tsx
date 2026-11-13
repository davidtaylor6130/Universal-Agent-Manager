import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it } from 'vitest'
import { UsageGauge, usageGaugeBand, usageGaugeRotation } from './UsageGauge'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function render(usedPercent: number | null, decorative = false) {
  const host = document.createElement('div')
  const root = createRoot(host)
  act(() => root.render(<UsageGauge usedPercent={usedPercent} decorative={decorative} />))
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

  it.each([[10, ['var(--success)', 'currentColor', 'currentColor']], [50, ['currentColor', 'var(--meter-warning)', 'currentColor']], [100, ['currentColor', 'currentColor', 'var(--error)']]] as const)(
    'colours only the third holding %s%% used', (used, strokes) => {
      const view = render(used)
      try {
        const bands = [...view.host.querySelectorAll('[data-band]')]
        expect(bands.map(band => band.getAttribute('stroke'))).toEqual(strokes)
        expect(view.host.querySelectorAll('[data-active]')).toHaveLength(1)
      } finally { view.close() }
    })

  it('separates the equal thirds with gaps at the inner joins only', () => {
    const view = render(10)
    try {
      // Joins sit at 60° and 120° on a radius-9 dial centred at (12, 12), trimmed by 3% either side.
      expect([...view.host.querySelectorAll('[data-band]')].map(band => band.getAttribute('d'))).toEqual([
        'M 3.00 12.00 A 9 9 0 0 1 6.79 4.66',
        'M 8.25 3.82 A 9 9 0 0 1 15.75 3.82',
        'M 17.21 4.66 A 9 9 0 0 1 21.00 12.00',
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
      expect([...svg.querySelectorAll('path')].map(path => path.getAttribute('stroke'))).toEqual(['currentColor'])
    } finally { view.close() }
  })

  it('stays hidden from assistive technology when decorative, while still drawing the needle', () => {
    const view = render(40, true)
    try {
      expect(view.host.querySelector('svg')?.getAttribute('aria-hidden')).toBe('true')
      expect(view.host.querySelector('[role="meter"]')).toBeNull()
      expect(view.host.querySelector<SVGGElement>('[data-needle]')?.style.transform).toBe('rotate(-18deg)')
    } finally { view.close() }
  })

  it.each([NaN, Infinity, -Infinity])('treats non-finite %s like an unknown value', (used) => {
    expect(usageGaugeRotation(used)).toBe(-90)
    expect(usageGaugeBand(used)).toBe(0)
  })

  it('draws inactive strokes in the button text colour in forced-colours mode', () => {
    const styles = readFileSync(resolve(process.cwd(), 'src/index.css'), 'utf8')
    expect(styles).toMatch(/@media \(forced-colors: active\) \{\s*\.uam-usage-gauge path \{\s*stroke:\s*ButtonText;/)
  })
})
