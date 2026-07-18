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
        { id: 'review', title: 'Review code', maker: 'David', review: '', dateCreated: '', dateUpdated: '', preview: 'Find regressions', body: '# Review\n\nFind regressions', favorite: false, sourceProvider: 'codex', sourcePath: '/tmp/codex/review.md', commandName: 'review-code', filePath: '/tmp/store/review.uam' },
        { id: 'notes', title: 'Release notes', maker: 'Sam', review: '', dateCreated: '', dateUpdated: '', preview: 'Summarize changes', body: '# Notes', favorite: true, sourceProvider: 'gemini-cli', sourcePath: '/tmp/gemini/notes.md', commandName: 'release-notes', filePath: '/tmp/store/notes.uam' },
      ],
      closeMarkdownStore: vi.fn(),
      refreshMarkdownStore: vi.fn(() => Promise.resolve(true)),
      browseMarkdownStoreDirectory: vi.fn(() => Promise.resolve(null)),
      setMarkdownStoreDirectory: vi.fn(() => Promise.resolve(true)),
      createMarkdownStoreEntry: vi.fn(() => Promise.resolve(true)),
      updateMarkdownStoreEntry: vi.fn(() => Promise.resolve(true)),
      setMarkdownStoreFavorite: vi.fn(() => Promise.resolve(true)),
      browseMarkdownStoreImport: vi.fn(() => Promise.resolve(null)),
      previewMarkdownStoreImports: vi.fn(() => Promise.resolve([])),
      importMarkdownStoreEntries: vi.fn(() => Promise.resolve([])),
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

    const dialog = host.querySelector('[role="dialog"][aria-label="Skills"]') as HTMLElement
    const create = host.querySelector('button[aria-label="Create skill"]') as HTMLButtonElement
    expect(dialog.className).toContain('h-[min(780px,90vh)]')
    expect(dialog.querySelector('.overflow-y-auto')).toBeTruthy()
    expect(create.style.border).toBe('0px')
    expect(create.textContent).toBe('')

    const search = host.querySelector('input[aria-label="Search Skills"]') as HTMLInputElement
    act(() => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
      valueSetter?.call(search, 'regressions')
      search.dispatchEvent(new Event('input', { bubbles: true }))
    })

    expect(host.textContent).toContain('Review code')
    expect(host.textContent).not.toContain('Release notes')
    expect(host.textContent).toContain('1 of 2')

    act(() => (host.querySelector('button[aria-label="Open in external editor"]') as HTMLButtonElement)?.click())
    expect(useAppStore.getState().editMarkdownStoreEntry).toHaveBeenCalledWith(expect.objectContaining({ id: 'review' }))

    act(() => (host.querySelector('button[aria-label="Attach to message"]') as HTMLButtonElement)?.click())
    expect(useAppStore.getState().attachMarkdownStoreEntry).toHaveBeenCalledWith('chat-1', expect.objectContaining({ id: 'review' }))
    expect(useAppStore.getState().closeMarkdownStore).toHaveBeenCalledTimes(1)

    act(() => root.unmount())
    host.remove()
  })

  it('configures an unconfigured store from its warning and leaves cancellation unchanged', async () => {
    const browse = vi.fn<() => Promise<string | null>>()
      .mockResolvedValueOnce(null)
      .mockResolvedValueOnce('/tmp/new-store')
    useAppStore.setState({
      markdownStoreDirectory: '',
      markdownStoreEntries: [],
      browseMarkdownStoreDirectory: browse,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MarkdownStoreModal />))

    const choose = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Choose folder')
    const publish = host.querySelector('button[aria-label="Create skill"]') as HTMLButtonElement
    expect(choose).toBeTruthy()
    expect(publish.disabled).toBe(true)

    await act(async () => { choose?.click(); await Promise.resolve() })
    expect(useAppStore.getState().setMarkdownStoreDirectory).not.toHaveBeenCalled()
    expect(useAppStore.getState().refreshMarkdownStore).not.toHaveBeenCalled()

    await act(async () => { choose?.click(); await Promise.resolve(); await Promise.resolve() })
    expect(useAppStore.getState().setMarkdownStoreDirectory).toHaveBeenCalledWith('/tmp/new-store')
    expect(useAppStore.getState().refreshMarkdownStore).toHaveBeenCalledTimes(1)

    act(() => root.unmount())
    host.remove()
  })

  it('filters by favorites and provider and updates favorites inline', async () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MarkdownStoreModal />))

    const filter = host.querySelector('select[aria-label="Filter Skills"]') as HTMLSelectElement
    act(() => { filter.value = 'favorites'; filter.dispatchEvent(new Event('change', { bubbles: true })) })
    expect(host.textContent).toContain('Release notes')
    expect(host.textContent).not.toContain('Review code')
    act(() => { filter.value = 'source:codex'; filter.dispatchEvent(new Event('change', { bubbles: true })) })
    expect(host.textContent).toContain('Review code')
    expect(host.textContent).not.toContain('Release notes')

    const favorite = host.querySelector('[aria-label="Add Review code to favorites"]') as HTMLElement
    await act(async () => { favorite.dispatchEvent(new MouseEvent('click', { bubbles: true })); await Promise.resolve() })
    expect(useAppStore.getState().setMarkdownStoreFavorite).toHaveBeenCalledWith(expect.objectContaining({ id: 'review' }), true)

    act(() => root.unmount())
    host.remove()
  })

  it('edits with preview, saves atomically through the store, and keeps failed edits open', async () => {
    const update = vi.fn().mockResolvedValueOnce(false).mockResolvedValueOnce(true)
    useAppStore.setState({ updateMarkdownStoreEntry: update })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MarkdownStoreModal />))

    act(() => (host.querySelector('button[aria-label="Edit Review code in app"]') as HTMLButtonElement)?.click())
    const title = host.querySelector('input[aria-label="Entry title"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(title, 'Review carefully')
      title.dispatchEvent(new Event('input', { bubbles: true }))
    })
    act(() => Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Preview Markdown')?.click())
    expect(host.textContent).toContain('Find regressions')

    await act(async () => { Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Save')?.click(); await Promise.resolve() })
    expect(update).toHaveBeenCalledWith(expect.objectContaining({ id: 'review' }), expect.objectContaining({ title: 'Review carefully' }))
    expect(host.querySelector('input[aria-label="Entry title"]')).toBeTruthy()
    await act(async () => { Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Save')?.click(); await Promise.resolve() })
    expect(host.querySelector('input[aria-label="Entry title"]')).toBeFalsy()

    act(() => root.unmount())
    host.remove()
  })

  it('previews imports and requires an explicit collision action', async () => {
    const preview = vi.fn(() => Promise.resolve([
      { id: 'one', title: 'Review code', maker: '', review: '', preview: 'Preview', sourceProvider: 'codex', sourcePath: '/tmp/source.md', supported: true, validationError: '', collisionPath: '/tmp/store/review.uam' },
      { id: 'bad', title: '', maker: '', review: '', preview: '', sourceProvider: 'codex', sourcePath: '/tmp/source.exe', supported: false, validationError: 'Unsupported file type', collisionPath: '' },
    ]))
    const importEntries = vi.fn(() => Promise.resolve([{ sourcePath: '/tmp/source.md', status: 'imported' as const, message: 'Imported' }]))
    useAppStore.setState({ previewMarkdownStoreImports: preview, importMarkdownStoreEntries: importEntries })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MarkdownStoreModal />))

    await act(async () => { (host.querySelector('button[aria-label="Import provider skills"]') as HTMLButtonElement)?.click(); await new Promise((resolve) => setTimeout(resolve, 0)) })
    expect(host.textContent).toContain('Import preview')
    expect(host.textContent).toContain('Unsupported file type')
    const collision = host.querySelector('select[aria-label="Collision action for Review code"]') as HTMLSelectElement
    act(() => { collision.value = 'separate'; collision.dispatchEvent(new Event('change', { bubbles: true })) })
    await act(async () => { Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Import selected')?.click(); await new Promise((resolve) => setTimeout(resolve, 0)) })
    expect(importEntries).toHaveBeenCalledWith([{ sourceProvider: 'codex', sourcePath: '/tmp/source.md', conflictAction: 'separate' }])

    act(() => root.unmount())
    host.remove()
  })
})
