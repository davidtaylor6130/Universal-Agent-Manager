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
  it('toggles the Active filter from the header quick toggle', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const onToggleStatusFilter = vi.fn()
    const render = (statusIds: string[]) => root.render(
      <ChatSearchBar
        value=""
        deepSearch={false}
        filters={{ providerIds: [], statusIds: statusIds as never[] }}
        providerOptions={[]}
        onChange={vi.fn()}
        onClear={vi.fn()}
        onToggleDeepSearch={vi.fn()}
        onToggleProviderFilter={vi.fn()}
        onToggleStatusFilter={onToggleStatusFilter}
        onClearFilters={vi.fn()}
      />,
    )
    act(() => render([]))
    const quick = host.querySelector<HTMLButtonElement>('button[aria-label="Show only active chats"]')!
    expect(quick.getAttribute('aria-pressed')).toBe('false')
    act(() => quick.click())
    expect(onToggleStatusFilter).toHaveBeenCalledWith('active')

    act(() => render(['active']))
    expect(host.querySelector('button[aria-label="Show only active chats"]')?.getAttribute('aria-pressed')).toBe('true')

    act(() => root.unmount())
    host.remove()
  })
})
