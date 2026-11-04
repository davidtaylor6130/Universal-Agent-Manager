import { afterEach, expect, it, vi } from 'vitest'
import { fitColumnTooltips } from './UsagePage'

const box = (left: number, top: number, width: number, height: number) => ({ left, top, width, height, right: left + width, bottom: top + height, x: left, y: top, toJSON() {} }) as DOMRect
// Mock layout: the card is 240px wide; each tooltip is centred on its column (data-center) and shifted by its --tip-dx.
function rectOf(el: HTMLElement) {
  if (el.classList.contains('uam-tokens-chart')) return box(0, 0, 240, 300)
  if (el.classList.contains('uam-usage-col-tip')) {
    const dx = parseFloat(el.style.getPropertyValue('--tip-dx') || '0')
    return box(Number(el.dataset.center) - Number(el.dataset.width) / 2 + dx, Number(el.dataset.top ?? 40), Number(el.dataset.width), 80)
  }
  return box(0, 0, 0, 0)
}
afterEach(() => { vi.restoreAllMocks(); document.body.innerHTML = '' })

function narrowPlot(centers: number[], width: number, top?: number) {
  document.body.innerHTML = `<div class="uam-tokens-chart"><div class="uam-usage-plot"></div></div>`
  const plot = document.querySelector<HTMLElement>('.uam-usage-plot')!
  for (const center of centers) {
    plot.insertAdjacentHTML('beforeend', `<div class="uam-usage-col"><span class="uam-usage-col-tip" data-center="${center}" data-width="${width}"${top === undefined ? '' : ` data-top="${top}"`}></span></div>`)
  }
  vi.spyOn(HTMLElement.prototype, 'getBoundingClientRect').mockImplementation(function (this: HTMLElement) { return rectOf(this) })
  return plot
}

it('keeps a middle column tooltip inside the card on a narrow plot, shifting it rather than clipping', () => {
  // 14 columns in 240px: column 7 sits at x=128, and its 220px tooltip would reach x=238 past the 232px limit.
  const plot = narrowPlot(Array.from({ length: 14 }, (_, index) => index * 17 + 8.5), 220)
  fitColumnTooltips(plot)
  const tip = plot.querySelectorAll<HTMLElement>('.uam-usage-col-tip')[7]
  const dx = parseFloat(tip.style.getPropertyValue('--tip-dx'))
  expect(dx).toBeLessThan(0)
  const rect = rectOf(tip)
  expect(rect.left).toBeGreaterThanOrEqual(8)
  expect(rect.right).toBeLessThanOrEqual(240 - 8)
  expect(tip.dataset.flip).toBeUndefined()
})

it('drops a tooltip to the plot top when it would cross the card top', () => {
  const plot = narrowPlot([120], 100, -20)
  fitColumnTooltips(plot)
  expect(plot.querySelector<HTMLElement>('.uam-usage-col-tip')!.dataset.flip).toBe('')
})
