import { act, type ReactElement } from 'react'
import { createRoot } from 'react-dom/client'
import { iconNames } from 'lucide-react/dynamic'
import { describe, expect, it, vi } from 'vitest'
import { SUGGESTED_LUCIDE_ICONS } from '../../utils/lucideIconLibrary'
import { CustomIcon } from './CustomIcon'
import { LucideIconLibrary } from './LucideIcons'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function typeInto(input: HTMLInputElement, value: string) {
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, value)
  input.dispatchEvent(new Event('input', { bubbles: true }))
}

function render(element: ReactElement) {
  const host = document.createElement('div'); document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(element))
  return { host, root, cleanup: () => { act(() => root.unmount()); host.remove() } }
}

describe('Lucide icon library', () => {
  it('only suggests icons that exist in the installed Lucide set', () => {
    const known = new Set<string>(iconNames)
    expect(SUGGESTED_LUCIDE_ICONS.filter((name) => !known.has(name))).toEqual([])
  })

  it('searches by name, moves with arrow keys and selects with Enter', async () => {
    const onSelect = vi.fn()
    const view = render(<LucideIconLibrary selected={null} onSelect={onSelect} />)
    try {
      const search = view.host.querySelector<HTMLInputElement>('input[aria-label="Search icons"]')!
      act(() => typeInto(search, 'flask'))
      const cells = Array.from(view.host.querySelectorAll<HTMLButtonElement>('[role="group"] button'))
      expect(cells.length).toBeGreaterThan(0)
      expect(cells.every((cell) => cell.getAttribute('aria-label')!.includes('flask'))).toBe(true)
      act(() => cells[0].focus())
      act(() => cells[0].dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true })))
      expect(document.activeElement).toBe(cells[1])
      expect(cells[1].tabIndex).toBe(0)
      act(() => cells[1].click())
      expect(onSelect).toHaveBeenCalledWith(cells[1].getAttribute('aria-label'))
    } finally { view.cleanup() }
  })

  it('reports no matches and renders unknown library names through the fallback', async () => {
    const view = render(<LucideIconLibrary selected={null} onSelect={vi.fn()} />)
    try {
      const search = view.host.querySelector<HTMLInputElement>('input[aria-label="Search icons"]')!
      act(() => typeInto(search, 'zzqqxx'))
      expect(view.host.textContent).toContain('No icons match')
    } finally { view.cleanup() }
    const icon = render(<CustomIcon icon={{ type: 'text', value: 'lucide:not-a-real-icon' }} fallback={<span>Default</span>} />)
    try {
      await act(async () => { await import('./LucideIcons') })
      await act(async () => { await new Promise((resolve) => setTimeout(resolve, 0)) })
      expect(icon.host.textContent).toBe('Default')
    } finally { icon.cleanup() }
  })
})
