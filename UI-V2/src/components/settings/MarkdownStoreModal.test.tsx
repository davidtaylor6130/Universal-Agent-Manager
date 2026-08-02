import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import type { MarkdownStoreImportCandidate, MarkdownStoreImportResult } from '../../types/markdownStore'
import { MarkdownStoreModal } from './MarkdownStoreModal'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true;
(globalThis as typeof globalThis & { ResizeObserver?: typeof ResizeObserver }).ResizeObserver ??= class {
  observe() {}
  unobserve() {}
  disconnect() {}
} as unknown as typeof ResizeObserver

describe('MarkdownStoreModal', () => {
  beforeEach(() => {
    useAppStore.setState({
      activeSessionId: 'chat-1',
      markdownStoreDirectory: '/tmp/store',
      markdownStoreLoading: false,
      markdownStoreError: '',
      markdownStoreEntries: [
        { id: 'review', title: 'Review code', maker: 'David', review: '', dateCreated: '', dateUpdated: '', preview: 'Find regressions', body: '# Review\n\nFind regressions', favorite: false, sourceProvider: 'codex', sourcePath: '/tmp/codex/review.md', commandName: 'review-code', group: 'Coding', filePath: '/tmp/store/review.uam' },
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

  it('keeps the preview and actions on the visible filtered entry', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MarkdownStoreModal />))

    act(() => Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Review code'))?.click())
    const search = host.querySelector('input[aria-label="Search Skills"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(search, 'Release notes')
      search.dispatchEvent(new Event('input', { bubbles: true }))
    })

    expect(host.querySelector('button[aria-label="Edit Release notes in app"]')).toBeTruthy()
    act(() => (host.querySelector('button[aria-label="Attach to message"]') as HTMLButtonElement).click())
    expect(useAppStore.getState().attachMarkdownStoreEntry).toHaveBeenCalledWith(
      'chat-1',
      expect.objectContaining({ id: 'notes' }),
    )

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
    act(() => { filter.value = 'group:Coding'; filter.dispatchEvent(new Event('change', { bubbles: true })) })
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
    const group = host.querySelector('input[aria-label="Entry group"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(group, 'Coding / Review')
      group.dispatchEvent(new Event('input', { bubbles: true }))
    })
    act(() => Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Preview Markdown')?.click())
    expect(host.textContent).toContain('Find regressions')

    await act(async () => { Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Save')?.click(); await Promise.resolve() })
    expect(update).toHaveBeenCalledWith(expect.objectContaining({ id: 'review' }), expect.objectContaining({ title: 'Review carefully', group: 'Coding / Review' }))
    expect(host.querySelector('input[aria-label="Entry title"]')).toBeTruthy()
    await act(async () => { Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Save')?.click(); await Promise.resolve() })
    expect(host.querySelector('input[aria-label="Entry title"]')).toBeFalsy()

    act(() => root.unmount())
    host.remove()
  })

  it('keeps Escape and save errors inside the active editor dialog', async () => {
    const update = vi.fn(async () => {
      useAppStore.setState({ markdownStoreError: 'Disk is read only.' })
      return false
    })
    useAppStore.setState({ updateMarkdownStoreEntry: update })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MarkdownStoreModal />))

    const editTrigger = host.querySelector('button[aria-label="Edit Review code in app"]') as HTMLButtonElement
    act(() => {
      editTrigger.focus()
      editTrigger.click()
    })
    const editor = host.querySelector('[role="dialog"][aria-label="Edit skill"]') as HTMLElement
    expect(editor).toBeTruthy()
    expect(editor.contains(document.activeElement)).toBe(true)
    await act(async () => {
      Array.from(editor.querySelectorAll('button')).find((button) => button.textContent === 'Save')?.click()
      await Promise.resolve()
    })
    expect(editor.querySelector('[role="alert"]')?.textContent).toContain('Disk is read only.')

    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' })))
    expect(useAppStore.getState().closeMarkdownStore).not.toHaveBeenCalled()
    expect(host.querySelector('[role="dialog"][aria-label="Edit skill"]')).toBeNull()
    expect(host.querySelector('[role="dialog"][aria-label="Skills"]')).toBeTruthy()
    expect(document.activeElement).toBe(editTrigger)

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

    const importTrigger = host.querySelector('button[aria-label="Import provider skills"]') as HTMLButtonElement
    await act(async () => {
      importTrigger.focus()
      importTrigger.click()
      await new Promise((resolve) => setTimeout(resolve, 0))
    })
    expect(host.textContent).toContain('Import preview')
    expect(host.textContent).toContain('Unsupported file type')
    expect((host.querySelector('[role="dialog"][aria-label="Import skills"]') as HTMLElement).contains(document.activeElement)).toBe(true)

    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' })))
    expect(useAppStore.getState().closeMarkdownStore).not.toHaveBeenCalled()
    expect(host.querySelector('[role="dialog"][aria-label="Import skills"]')).toBeNull()
    expect(host.querySelector('[role="dialog"][aria-label="Skills"]')).toBeTruthy()
    expect(document.activeElement).toBe(importTrigger)

    await act(async () => {
      importTrigger.click()
      await new Promise((resolve) => setTimeout(resolve, 0))
    })
    const collision = host.querySelector('select[aria-label="Collision action for Review code"]') as HTMLSelectElement
    act(() => { collision.value = 'separate'; collision.dispatchEvent(new Event('change', { bubbles: true })) })
    await act(async () => { Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Import selected')?.click(); await new Promise((resolve) => setTimeout(resolve, 0)) })
    expect(importEntries).toHaveBeenCalledWith([{ sourceProvider: 'codex', sourcePath: '/tmp/source.md', conflictAction: 'separate' }])

    act(() => root.unmount())
    host.remove()
  })

  it('clears a previous workflow error before creating another skill', () => {
    useAppStore.setState({ markdownStoreError: 'Previous save failed.' })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MarkdownStoreModal />))

    act(() => (host.querySelector('button[aria-label="Create skill"]') as HTMLButtonElement).click())

    expect(host.querySelector('[role="dialog"][aria-label="Create skill"]')).toBeTruthy()
    expect(host.querySelector('[role="alert"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('clears stale edit and import errors while keeping import failures in the active preview', async () => {
    const candidate = { id: 'one', title: 'One', maker: '', review: '', preview: '', sourceProvider: 'codex', sourcePath: '/tmp/one.md', supported: true, validationError: '', collisionPath: '' }
    useAppStore.setState({
      markdownStoreError: 'Previous workflow failed.',
      previewMarkdownStoreImports: vi.fn(() => Promise.resolve([candidate])),
      importMarkdownStoreEntries: vi.fn(async () => {
        useAppStore.setState({ markdownStoreError: 'Import failed.' })
        return []
      }),
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MarkdownStoreModal />))

    act(() => (host.querySelector('button[aria-label="Edit Review code in app"]') as HTMLButtonElement).click())
    let child = host.querySelector('[role="dialog"][aria-label="Edit skill"]') as HTMLElement
    expect(useAppStore.getState().markdownStoreError).toBe('')
    act(() => useAppStore.setState({ markdownStoreError: 'Edit failed.' }))
    expect(child.querySelector('[role="alert"]')?.textContent).toContain('Edit failed.')
    act(() => Array.from(child.querySelectorAll('button')).find((button) => button.textContent === 'Cancel')?.click())
    expect(useAppStore.getState().markdownStoreError).toBe('')

    act(() => useAppStore.setState({ markdownStoreError: 'Another stale error.' }))
    await act(async () => {
      (host.querySelector('button[aria-label="Import provider skills"]') as HTMLButtonElement).click()
      await Promise.resolve()
    })
    child = host.querySelector('[role="dialog"][aria-label="Import skills"]') as HTMLElement
    expect(useAppStore.getState().markdownStoreError).toBe('')
    expect(child.querySelector('[role="alert"]')).toBeNull()

    await act(async () => {
      Array.from(child.querySelectorAll('button')).find((button) => button.textContent === 'Import selected')?.click()
      await Promise.resolve()
    })
    child = host.querySelector('[role="dialog"][aria-label="Import skills"]') as HTMLElement
    expect(child.querySelector('[role="alert"]')?.textContent).toContain('Import failed.')
    act(() => Array.from(child.querySelectorAll('button')).find((button) => button.textContent === 'Cancel')?.click())
    expect(useAppStore.getState().markdownStoreError).toBe('')
    expect(host.querySelector('[role="dialog"][aria-label="Import skills"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('keeps a long import preview usable and shows skipped results inside it', async () => {
    const candidates = Array.from({ length: 40 }, (_, index) => ({
      id: `skill-${index}`,
      title: `Skill ${index}`,
      maker: '',
      review: '',
      preview: '',
      sourceProvider: 'codex',
      sourcePath: `/tmp/skill-${index}.md`,
      supported: true,
      validationError: '',
      collisionPath: `/tmp/store/skill-${index}.uam`,
    }))
    useAppStore.setState({
      previewMarkdownStoreImports: vi.fn(() => Promise.resolve(candidates)),
      importMarkdownStoreEntries: vi.fn(() => Promise.resolve(candidates.map((candidate) => ({
        sourcePath: candidate.sourcePath,
        status: 'skipped' as const,
        message: 'Already exists',
      })))),
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MarkdownStoreModal />))

    await act(async () => {
      (host.querySelector('button[aria-label="Import provider skills"]') as HTMLButtonElement).click()
      await Promise.resolve()
    })
    const preview = host.querySelector('[role="dialog"][aria-label="Import skills"]') as HTMLElement
    expect(preview).toBeTruthy()
    expect(preview.className).toContain('max-h-[calc(100vh-2rem)]')
    expect(preview.querySelector('[data-import-candidates]')?.className).toContain('overflow-y-auto')

    await act(async () => {
      Array.from(preview.querySelectorAll('button')).find((button) => button.textContent === 'Import selected')?.click()
      await Promise.resolve()
    })
    expect(host.querySelector('[role="dialog"][aria-label="Import skills"]')).toBeNull()
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Already exists')

    act(() => root.unmount())
    host.remove()
  })

  it('keeps the most recently requested import preview', async () => {
    let resolveProvider: (value: MarkdownStoreImportCandidate[]) => void = () => {}
    let resolveFile: (value: MarkdownStoreImportCandidate[]) => void = () => {}
    useAppStore.setState({
      browseMarkdownStoreImport: vi.fn(() => Promise.resolve('/tmp/recent.md')),
      previewMarkdownStoreImports: vi.fn((options) => new Promise<MarkdownStoreImportCandidate[]>((resolve) => {
        if (options.includeProviders) resolveProvider = resolve
        else resolveFile = resolve
      })),
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MarkdownStoreModal />))

    act(() => (host.querySelector('button[aria-label="Import provider skills"]') as HTMLButtonElement).click())
    await act(async () => {
      (host.querySelector('button[aria-label="Import file"]') as HTMLButtonElement).click()
      await Promise.resolve()
    })
    await act(async () => {
      resolveFile([{ id: 'recent', title: 'Recent file', maker: '', review: '', preview: '', sourceProvider: 'file', sourcePath: '/tmp/recent.md', supported: true, validationError: '', collisionPath: '' }])
      await Promise.resolve()
    })
    expect(host.textContent).toContain('Recent file')

    await act(async () => {
      resolveProvider([{ id: 'stale', title: 'Stale provider skill', maker: '', review: '', preview: '', sourceProvider: 'provider', sourcePath: '/tmp/stale.md', supported: true, validationError: '', collisionPath: '' }])
      await Promise.resolve()
    })
    expect(host.textContent).toContain('Recent file')
    expect(host.textContent).not.toContain('Stale provider skill')

    act(() => root.unmount())
    host.remove()
  })

  it('saves an entry only once before the submitting state rerenders', async () => {
    const finishes: Array<(ok: boolean) => void> = []
    const update = vi.fn(() => new Promise<boolean>((resolve) => finishes.push(resolve)))
    useAppStore.setState({ updateMarkdownStoreEntry: update })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MarkdownStoreModal />))
    act(() => (host.querySelector('button[aria-label="Edit Review code in app"]') as HTMLButtonElement).click())
    const save = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Save') as HTMLButtonElement

    act(() => {
      save.click()
      save.click()
    })

    expect(update).toHaveBeenCalledTimes(1)
    await act(async () => {
      finishes.forEach((finish) => finish(false))
      await Promise.resolve()
    })
    act(() => root.unmount())
    host.remove()
  })

  it('runs an import only once before the importing state rerenders', async () => {
    const finishes: Array<(results: MarkdownStoreImportResult[]) => void> = []
    const importEntries = vi.fn(() => new Promise<MarkdownStoreImportResult[]>((resolve) => finishes.push(resolve)))
    useAppStore.setState({
      previewMarkdownStoreImports: vi.fn(() => Promise.resolve([
        { id: 'one', title: 'One', maker: '', review: '', preview: '', sourceProvider: 'file', sourcePath: '/tmp/one.md', supported: true, validationError: '', collisionPath: '' },
      ])),
      importMarkdownStoreEntries: importEntries,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MarkdownStoreModal />))
    await act(async () => {
      (host.querySelector('button[aria-label="Import provider skills"]') as HTMLButtonElement).click()
      await Promise.resolve()
    })
    const run = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Import selected') as HTMLButtonElement

    act(() => {
      run.click()
      run.click()
    })

    expect(importEntries).toHaveBeenCalledTimes(1)
    await act(async () => {
      finishes.forEach((finish) => finish([]))
      await Promise.resolve()
    })
    act(() => root.unmount())
    host.remove()
  })
})
