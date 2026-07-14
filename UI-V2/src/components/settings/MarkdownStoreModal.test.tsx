import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { MarkdownStoreModal } from './MarkdownStoreModal'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('MarkdownStoreModal', () => {
  beforeEach(() => {
    useAppStore.setState({
      activeSessionId: 'chat-1',
      markdownStoreDirectory: '/tmp/store',
      markdownStoreLoading: false,
      markdownStoreError: '',
      markdownStoreEntries: [
        { id: 'review', title: 'Review code', maker: 'David', review: '', dateCreated: '', dateUpdated: '', preview: 'Find regressions', filePath: '/tmp/store/review.uam' },
        { id: 'notes', title: 'Release notes', maker: 'Sam', review: '', dateCreated: '', dateUpdated: '', preview: 'Summarize changes', filePath: '/tmp/store/notes.uam' },
      ],
      closeMarkdownStore: vi.fn(),
      refreshMarkdownStore: vi.fn(() => Promise.resolve(true)),
      createMarkdownStoreEntry: vi.fn(() => Promise.resolve(true)),
      revealMarkdownStoreEntry: vi.fn(() => Promise.resolve(true)),
      editMarkdownStoreEntry: vi.fn(() => Promise.resolve(true)),
      attachMarkdownStoreEntry: vi.fn(),
    })
  })

  it('finds entries by metadata and exposes explicit edit and attach actions', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MarkdownStoreModal />))

    const search = host.querySelector('input[aria-label="Search Markdown Store"]') as HTMLInputElement
    act(() => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
      valueSetter?.call(search, 'regressions')
      search.dispatchEvent(new Event('input', { bubbles: true }))
    })

    expect(host.textContent).toContain('Review code')
    expect(host.textContent).not.toContain('Release notes')
    expect(host.textContent).toContain('1 of 2')

    act(() => Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Open in editor'))?.click())
    expect(useAppStore.getState().editMarkdownStoreEntry).toHaveBeenCalledWith(expect.objectContaining({ id: 'review' }))

    act(() => Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Attach to message'))?.click())
    expect(useAppStore.getState().attachMarkdownStoreEntry).toHaveBeenCalledWith('chat-1', expect.objectContaining({ id: 'review' }))
    expect(useAppStore.getState().closeMarkdownStore).toHaveBeenCalledTimes(1)

    act(() => root.unmount())
    host.remove()
  })
})
