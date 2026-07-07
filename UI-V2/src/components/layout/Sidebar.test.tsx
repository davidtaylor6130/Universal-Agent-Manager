import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'

vi.mock('../sidebar/ChatSearchBar', () => ({
  ChatSearchBar: ({ value, onChange, onClear }: { value: string; onChange: (value: string) => void; onClear: () => void }) => (
    <div data-testid="chat-search">
      <input value={value} onChange={(event) => onChange(event.currentTarget.value)} />
      <button type="button" onClick={onClear}>Clear</button>
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
    useAppStore.setState({
      setNewChatModalOpen: vi.fn(),
      sessions: [],
      providers: [],
    })
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
