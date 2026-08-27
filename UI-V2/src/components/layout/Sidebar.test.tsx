import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'

vi.mock('../sidebar/ChatSearchBar', () => ({
  ChatSearchBar: ({ value, onChange, onClear, onToggleDeepSearch }: { value: string; onChange: (value: string) => void; onClear: () => void; onToggleDeepSearch: () => void }) => (
    <div data-testid="chat-search">
      <input value={value} onChange={(event) => onChange(event.currentTarget.value)} />
      <button type="button" onClick={onClear}>Clear</button>
      <button type="button" onClick={onToggleDeepSearch}>Deep</button>
    </div>
  ),
}))

vi.mock('../sidebar/FolderTree', () => ({
  FolderTree: ({ searchQuery }: { searchQuery: string }) => <div data-testid="folder-tree">{searchQuery}</div>,
}))

import { Sidebar } from './Sidebar'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('Sidebar', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    delete window.cefQuery
    useAppStore.setState({
      setNewChatModalOpen: vi.fn(),
      sessions: [],
      providers: [],
    })
  })

  it('distinguishes a failed deep search from no matches and retries', async () => {
    vi.useFakeTimers()
    const requests: string[] = []
    window.cefQuery = ({ request, onFailure }) => {
      requests.push(request)
      onFailure(500, 'Search index unavailable.')
    }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<Sidebar />))

    const input = host.querySelector('input') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(input, 'needle')
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    act(() => Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Deep')?.click())
    await act(async () => { await vi.advanceTimersByTimeAsync(180) })
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Search index unavailable.')
    expect(requests).toHaveLength(1)

    act(() => Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Retry')?.click())
    await act(async () => { await vi.advanceTimersByTimeAsync(180) })
    expect(requests).toHaveLength(2)

    act(() => root.unmount())
    host.remove()
    delete window.cefQuery
    vi.useRealTimers()
  })

  it('places New Chat in the bottom sidebar footer', () => {
    const setNewChatModalOpen = vi.fn()
    useAppStore.setState({ setNewChatModalOpen })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<Sidebar />)
    })

    const button = Array.from(host.querySelectorAll('button')).find((candidate) => candidate.textContent === 'New Chat') as HTMLButtonElement
    expect(button).toBeTruthy()
    expect(button.closest('.flex-shrink-0')).toBeTruthy()
    expect(host.querySelector('[data-testid="sidebar-tree-scroll"]')?.className).toContain('py-1')

    act(() => {
      button.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setNewChatModalOpen).toHaveBeenCalledWith(true)

    act(() => {
      root.unmount()
    })
    host.remove()
  })
})
