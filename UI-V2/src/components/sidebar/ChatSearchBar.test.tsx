import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { ChatSearchBar } from './ChatSearchBar'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
globalThis.ResizeObserver ??= class ResizeObserver {
  observe() {}
  unobserve() {}
  disconnect() {}
}

describe('ChatSearchBar', () => {
  it('opens filters as a focused menu and returns focus to its trigger on Escape', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(
      <ChatSearchBar
        value=""
        deepSearch={false}
        filters={{ providerIds: [], statusIds: [] }}
        providerOptions={[{ id: 'codex-cli', label: 'Codex' }]}
        onChange={vi.fn()}
        onClear={vi.fn()}
        onToggleDeepSearch={vi.fn()}
        onToggleProviderFilter={vi.fn()}
        onToggleStatusFilter={vi.fn()}
        onClearFilters={vi.fn()}
      />,
    ))

    const trigger = host.querySelector('button[aria-label="Filter chats"]') as HTMLButtonElement
    act(() => trigger.click())
    const menu = document.body.querySelector('[role="menu"][aria-label="Chat filters"]') as HTMLElement
    expect(menu).toBeTruthy()
    expect(document.activeElement).toBe(menu.querySelector('[role^="menuitem"]'))

    act(() => document.activeElement?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.querySelector('[role="menu"][aria-label="Chat filters"]')).toBeNull()
    expect(document.activeElement).toBe(trigger)

    act(() => root.unmount())
    host.remove()
  })

  it('clears the search from an IconButton with a 24px hit area that keeps its 18px place in the field', () => {
    const onClear = vi.fn()
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(
      <ChatSearchBar
        value="parser"
        deepSearch={false}
        filters={{ providerIds: [], statusIds: [] }}
        providerOptions={[]}
        onChange={vi.fn()}
        onClear={onClear}
        onToggleDeepSearch={vi.fn()}
        onToggleProviderFilter={vi.fn()}
        onToggleStatusFilter={vi.fn()}
        onClearFilters={vi.fn()}
      />,
    ))
    const clear = host.querySelector<HTMLButtonElement>('button[aria-label="Clear search"]')!
    expect(clear.classList.contains('uam-iconbtn')).toBe(true)
    expect(clear.classList.contains('uam-sb-iconbtn')).toBe(true)
    expect(clear.classList.contains('uam-sb-iconbtn--inline')).toBe(true)
    expect(clear.style.getPropertyValue('--sb-paint')).toBe('18px')
    act(() => clear.click())
    expect(onClear).toHaveBeenCalledTimes(1)
    act(() => root.unmount())
    host.remove()
  })
})

describe('providerGridColumns', () => {
  it('splits provider icons evenly between 3 and 6 columns', async () => {
    const { providerGridColumns } = await import('./ChatSearchBar')
    expect([1, 3, 4, 5, 6, 7, 8, 9, 12, 13].map(providerGridColumns)).toEqual([3, 3, 4, 5, 6, 4, 4, 5, 6, 5])
  })
})
