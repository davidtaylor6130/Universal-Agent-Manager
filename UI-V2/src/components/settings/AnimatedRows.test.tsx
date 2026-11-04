import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { AnimatedRows } from './SettingsModal'
import { EXIT_MS } from '../ui'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

let host: HTMLDivElement
let root: Root
const render = (ids: string[]) => root.render(<AnimatedRows items={ids.map((id) => ({ id }))} keyOf={(item) => item.id} render={(item) => <span>{item.id}</span>} />)
const shown = () => Array.from(host.querySelectorAll('span')).map((node) => node.textContent)

function setReducedMotion(reduce: boolean) {
  window.matchMedia = vi.fn().mockImplementation((query: string) => ({ matches: reduce && query.includes('reduce'), media: query, addEventListener() {}, removeEventListener() {} })) as unknown as typeof window.matchMedia
}

beforeEach(() => {
  vi.useFakeTimers()
  host = document.createElement('div')
  document.body.appendChild(host)
  root = createRoot(host)
})

afterEach(() => {
  act(() => root.unmount())
  host.remove()
  vi.useRealTimers()
  delete (window as { matchMedia?: unknown }).matchMedia
})

describe('AnimatedRows', () => {
  it('holds a removed row for the exit length before unmounting it', () => {
    setReducedMotion(false)
    act(() => render(['a', 'b']))
    act(() => render(['a']))
    expect(shown()).toEqual(['a', 'b'])
    expect(host.querySelector('[data-state="closed"]')?.textContent).toBe('b')
    act(() => { vi.advanceTimersByTime(EXIT_MS) })
    expect(shown()).toEqual(['a'])
  })

  it('removes immediately under reduced motion', () => {
    setReducedMotion(true)
    act(() => render(['a', 'b']))
    act(() => render(['a']))
    expect(shown()).toEqual(['a'])
  })

  it('keeps added rows in list order', () => {
    setReducedMotion(true)
    act(() => render(['a', 'c']))
    act(() => render(['a', 'b', 'c']))
    expect(shown()).toEqual(['a', 'b', 'c'])
  })
})
