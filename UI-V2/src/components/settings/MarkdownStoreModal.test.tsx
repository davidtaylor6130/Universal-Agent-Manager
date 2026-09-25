import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import type { MarkdownStoreImportCandidate, MarkdownStoreImportResult } from '../../types/markdownStore'
import { MarkdownStoreModal } from './MarkdownStoreModal'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true;
(globalThis as typeof globalThis & { ResizeObserver?: typeof ResizeObserver }).ResizeObserver ??= class {
  observe() {}
  unobserve() {}
  disconnect() {}
} as unknown as typeof ResizeObserver


const mounts: { root: ReturnType<typeof createRoot>; container: HTMLDivElement }[] = []
function mountSkills(embedded = false) {
  const container = document.createElement('div')
  document.body.appendChild(container)
  const root = createRoot(container)
  mounts.push({ root, container })
  act(() => root.render(<MarkdownStoreModal embedded={embedded} />))
  return { host: document.body, root }
}
function button(label: string, host: ParentNode = document.body) {
  const found = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((item) => item.getAttribute('aria-label') === label || item.textContent === label)
  expect(found, `Button: ${label}`).toBeTruthy()
  return found!
}
function click(label: string) { act(() => button(label).click()) }
function changeField(label: string, value: string) {
  const field = document.querySelector<HTMLInputElement | HTMLTextAreaElement>(`[aria-label="${label}"]`)!
  act(() => {
    const prototype = field.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype
    Object.getOwnPropertyDescriptor(prototype, 'value')!.set!.call(field, value)
    field.dispatchEvent(new Event('input', { bubbles: true }))
  })
}
function escape() {
  const event = new KeyboardEvent('keydown', { key: 'Escape', cancelable: true, bubbles: true })
  act(() => window.dispatchEvent(event))
  return event
}
async function openProviderImports(provider = 'Codex CLI') {
  click('Add skill')
  click('Import from provider')
  click(provider)
  await act(async () => { button('Review imports').click(); await Promise.resolve() })
}
async function confirmSave() {
  click('Save')
  await act(async () => { button('Overwrite').click(); await Promise.resolve() })
}
afterEach(() => {
  for (const { root, container } of mounts.splice(0)) { act(() => root.unmount()); container.remove() }
})

describe('MarkdownStoreModal', () => {
  beforeEach(() => {
    useAppStore.setState({
      activeSessionId: 'chat-1',
      markdownStoreDirectory: '/tmp/store',
      markdownStoreLoading: false,
      markdownStoreError: '',
      clearMarkdownStoreError: () => useAppStore.setState({ markdownStoreError: '' }),
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
    const { host } = mountSkills()

    const dialog = host.querySelector('[role="dialog"][aria-label="Skills"]') as HTMLElement
    const create = host.querySelector('button[aria-label="Add skill"]') as HTMLButtonElement
    expect(dialog.className).toContain('h-[min(780px,90vh)]')
    expect(dialog.querySelector('.overflow-y-auto')).toBeTruthy()
    expect(create.textContent).toBe('')

    const search = host.querySelector('input[aria-label="Search Skills"]') as HTMLInputElement
    act(() => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
      valueSetter?.call(search, 'regressions')
      search.dispatchEvent(new Event('input', { bubbles: true }))
    })

    expect(host.textContent).toContain('Review code')
    expect(host.textContent).not.toContain('Release notes')
    expect(host.querySelector('[aria-current="true"]')?.textContent).toContain('Review code')

    act(() => (host.querySelector('button[aria-label="Open in external editor"]') as HTMLButtonElement)?.click())
    expect(useAppStore.getState().editMarkdownStoreEntry).toHaveBeenCalledWith(expect.objectContaining({ id: 'review' }))

    act(() => (host.querySelector('button[aria-label="Attach to message"]') as HTMLButtonElement)?.click())
    expect(useAppStore.getState().attachMarkdownStoreEntry).toHaveBeenCalledWith('chat-1', expect.objectContaining({ id: 'review' }))
    expect(useAppStore.getState().closeMarkdownStore).toHaveBeenCalledTimes(1)
  })

  it('keeps the preview and actions on the visible filtered entry', () => {
    const { host } = mountSkills()

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
    const { host } = mountSkills()

    const choose = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Choose folder')
    const publish = host.querySelector('button[aria-label="Add skill"]') as HTMLButtonElement
    expect(choose).toBeTruthy()
    expect(publish.disabled).toBe(true)

    await act(async () => { choose?.click(); await Promise.resolve() })
    expect(useAppStore.getState().setMarkdownStoreDirectory).not.toHaveBeenCalled()
    expect(useAppStore.getState().refreshMarkdownStore).not.toHaveBeenCalled()

    await act(async () => { choose?.click(); await Promise.resolve(); await Promise.resolve() })
    expect(useAppStore.getState().setMarkdownStoreDirectory).toHaveBeenCalledWith('/tmp/new-store')
    expect(useAppStore.getState().refreshMarkdownStore).toHaveBeenCalledTimes(1)
  })

  it('filters by favorites and provider and updates favorites inline', async () => {
    const { host } = mountSkills()

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

    const favorite = host.querySelector('[aria-label="Pin Review code"]') as HTMLElement
    await act(async () => { favorite.dispatchEvent(new MouseEvent('click', { bubbles: true })); await Promise.resolve() })
    expect(useAppStore.getState().setMarkdownStoreFavorite).toHaveBeenCalledWith(expect.objectContaining({ id: 'review' }), true)
  })

  it('edits with preview, saves atomically through the store, and keeps failed edits open', async () => {
    const update = vi.fn().mockResolvedValueOnce(false).mockResolvedValueOnce(true)
    useAppStore.setState({ updateMarkdownStoreEntry: update })
    const { host } = mountSkills()

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

    await confirmSave()
    expect(update).toHaveBeenCalledWith(expect.objectContaining({ id: 'review' }), expect.objectContaining({ title: 'Review carefully', group: 'Coding / Review' }))
    expect(host.querySelector('input[aria-label="Entry title"]')).toBeTruthy()
    await confirmSave()
    expect(host.querySelector('input[aria-label="Entry title"]')).toBeFalsy()
  })

  it('keeps Escape and save errors inside the active editor dialog', async () => {
    const update = vi.fn(async () => {
      useAppStore.setState({ markdownStoreError: 'Disk is read only.' })
      return false
    })
    useAppStore.setState({ updateMarkdownStoreEntry: update })
    const { host } = mountSkills()

    const editTrigger = host.querySelector('button[aria-label="Edit Review code in app"]') as HTMLButtonElement
    act(() => {
      editTrigger.focus()
      editTrigger.click()
    })
    const editor = host.querySelector('[role="dialog"][aria-label="Edit skill"]') as HTMLElement
    expect(editor).toBeTruthy()
    expect(editor.contains(document.activeElement)).toBe(true)
    await confirmSave()
    expect(editor.querySelector('[role="alert"]')?.textContent).toContain('Disk is read only.')

    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' })))
    expect(useAppStore.getState().closeMarkdownStore).not.toHaveBeenCalled()
    expect(host.querySelector('[role="dialog"][aria-label="Edit skill"]')).toBeNull()
    expect(host.querySelector('[role="dialog"][aria-label="Skills"]')).toBeTruthy()
    expect(document.activeElement).toBe(editTrigger)
  })

  it('previews imports and requires an explicit collision action', async () => {
    const preview = vi.fn(() => Promise.resolve([
      { id: 'one', title: 'Review code', maker: '', review: '', preview: 'Preview', sourceProvider: 'codex', sourcePath: '/tmp/source.md', supported: true, validationError: '', collisionPath: '/tmp/store/review.uam' },
      { id: 'bad', title: '', maker: '', review: '', preview: '', sourceProvider: 'codex', sourcePath: '/tmp/source.exe', supported: false, validationError: 'Unsupported file type', collisionPath: '' },
    ]))
    const importEntries = vi.fn(() => Promise.resolve([{ sourcePath: '/tmp/source.md', status: 'imported' as const, message: 'Imported' }]))
    useAppStore.setState({ previewMarkdownStoreImports: preview, importMarkdownStoreEntries: importEntries })
    const { host } = mountSkills()

    const importTrigger = button('Add skill')
    await openProviderImports()
    expect(host.textContent).toContain('Import preview')
    expect(host.textContent).toContain('Unsupported file type')
    expect((host.querySelector('[role="dialog"][aria-label="Import skills"]') as HTMLElement).contains(document.activeElement)).toBe(true)

    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' })))
    expect(useAppStore.getState().closeMarkdownStore).not.toHaveBeenCalled()
    expect(host.querySelector('[role="dialog"][aria-label="Import skills"]')).toBeNull()
    expect(host.querySelector('[role="dialog"][aria-label="Skills"]')).toBeTruthy()
    expect(document.activeElement).toBe(importTrigger)

    await openProviderImports()
    const collision = host.querySelector('select[aria-label="Collision action for Review code"]') as HTMLSelectElement
    act(() => { collision.value = 'separate'; collision.dispatchEvent(new Event('change', { bubbles: true })) })
    await act(async () => { Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Import selected')?.click(); await new Promise((resolve) => setTimeout(resolve, 0)) })
    expect(importEntries).toHaveBeenCalledWith([{ sourceProvider: 'codex', sourcePath: '/tmp/source.md', conflictAction: 'separate' }])
  })

  it('clears a previous workflow error before creating another skill', () => {
    useAppStore.setState({ markdownStoreError: 'Previous save failed.' })
    const { host } = mountSkills()

    click('Add skill')
    click('Create')

    expect(host.querySelector('[role="dialog"][aria-label="Create skill"]')).toBeTruthy()
    expect(host.querySelector('[role="alert"]')).toBeNull()
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
    const { host } = mountSkills()

    act(() => (host.querySelector('button[aria-label="Edit Review code in app"]') as HTMLButtonElement).click())
    let child = host.querySelector('[role="dialog"][aria-label="Edit skill"]') as HTMLElement
    expect(useAppStore.getState().markdownStoreError).toBe('')
    act(() => useAppStore.setState({ markdownStoreError: 'Edit failed.' }))
    expect(child.querySelector('[role="alert"]')?.textContent).toContain('Edit failed.')
    act(() => Array.from(child.querySelectorAll('button')).find((button) => button.textContent === 'Cancel')?.click())
    expect(useAppStore.getState().markdownStoreError).toBe('')

    act(() => useAppStore.setState({ markdownStoreError: 'Another stale error.' }))
    await openProviderImports()
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
    const { host } = mountSkills()

    await openProviderImports()
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
    const { host } = mountSkills()

    await openProviderImports()
    click('Back')
    await act(async () => { button('Import file').click(); await Promise.resolve() })
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
  })

  it('saves an entry only once before the submitting state rerenders', async () => {
    const finishes: Array<(ok: boolean) => void> = []
    const update = vi.fn(() => new Promise<boolean>((resolve) => finishes.push(resolve)))
    useAppStore.setState({ updateMarkdownStoreEntry: update })
    const { host } = mountSkills()
    act(() => (host.querySelector('button[aria-label="Edit Review code in app"]') as HTMLButtonElement).click())
    click('Save')
    expect(update).not.toHaveBeenCalled()
    const save = button('Overwrite')

    act(() => {
      save.click()
      save.click()
    })

    expect(update).toHaveBeenCalledTimes(1)
    await act(async () => {
      finishes.forEach((finish) => finish(false))
      await Promise.resolve()
    })

  })

  it('runs an import only once before the importing state rerenders', async () => {
    const finishes: Array<(results: MarkdownStoreImportResult[]) => void> = []
    const importEntries = vi.fn(() => new Promise<MarkdownStoreImportResult[]>((resolve) => finishes.push(resolve)))
    useAppStore.setState({
      previewMarkdownStoreImports: vi.fn(() => Promise.resolve([
        { id: 'one', title: 'One', maker: '', review: '', preview: '', sourceProvider: 'file', sourcePath: '/tmp/one.md', supported: true, validationError: '', collisionPath: '' },
      ])),
      browseMarkdownStoreImport: vi.fn(() => Promise.resolve('/tmp/one.md')),
      importMarkdownStoreEntries: importEntries,
    })
    const { host } = mountSkills()
    click('Add skill')
    await act(async () => { button('Import file').click(); await Promise.resolve() })
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

  })

  it('embeds the library, loads real store data, and keeps Add as four square choices', async () => {
    const { host } = mountSkills(true)
    await act(async () => { await Promise.resolve() })
    expect(useAppStore.getState().refreshMarkdownStore).toHaveBeenCalledTimes(1)
    const region = host.querySelector('[role="region"][aria-label="Skills"]')!
    expect(region.hasAttribute('aria-modal')).toBe(false)
    expect(region.className).toContain('min-h-0')
    expect(host.querySelector('[role="dialog"]')).toBeNull()
    expect(host.querySelector('[aria-label="Close Skills"]')).toBeNull()
    expect(host.querySelector('[aria-label="Search Skills"]')?.closest('.uam-search-field')).toBeTruthy()
    expect(escape().defaultPrevented).toBe(false)
    expect(useAppStore.getState().closeMarkdownStore).not.toHaveBeenCalled()
    click('Add skill')
    const choices = host.querySelector('[aria-label="Add skill choices"]') as HTMLElement
    expect(choices.style.gridTemplateColumns).toContain('auto-fit')
    expect(Array.from(choices.querySelectorAll('button')).map((item) => item.textContent)).toEqual(['Create', 'Import from provider', 'Import file', 'Import folder'])
    for (const choice of choices.querySelectorAll('button')) {
      expect(choice.style.aspectRatio).toBe('1 / 1')
      expect(choice.querySelector('svg')).toBeTruthy()
    }
    expect(escape().defaultPrevented).toBe(true)
    expect(host.querySelector('[role="dialog"]')).toBeNull()
    expect(document.activeElement).toBe(button('Add skill'))
  })

  it('renders subfolder lines and keeps all four preview actions outside long content', () => {
    const entry = useAppStore.getState().markdownStoreEntries[0]
    useAppStore.setState({ markdownStoreEntries: [{ ...entry, group: 'Coding/Review/Safety', body: '# Review\n\n' + 'Long content\n'.repeat(300) }] })
    const { host } = mountSkills()
    const folders = host.querySelector('[aria-label="Skill folders"]')!
    expect(folders.querySelectorAll('details').length).toBe(3)
    expect(folders.querySelector('details details details')).toBeTruthy()
    for (const details of folders.querySelectorAll('details')) {
      expect((details.querySelector(':scope > div') as HTMLElement).style.borderLeft).toContain('var(--border-bright)')
      expect(details.querySelector('summary svg')).toBeTruthy()
    }
    const preview = host.querySelector('[aria-label="Skill preview"]')!
    expect(preview.children[0].querySelectorAll('button').length).toBe(4)
    expect(preview.children[0].className).toContain('shrink-0')
    expect(preview.children[1].className).toContain('overflow-auto')
    expect(preview.children[1].querySelector('[aria-label="Attach to message"]')).toBeNull()
    const row = folders.querySelector('[aria-current="true"]')!
    expect(row.children.length).toBe(2)
    expect(row.children[1].className).toContain('truncate')
  })

  it('guards dirty edits on Escape and Cancel, then confirms overwrite before saving', async () => {
    const { host } = mountSkills()
    click('Edit Review code in app')
    changeField('Entry Markdown body', 'Authored replacement')
    const unload = new Event('beforeunload', { cancelable: true })
    window.dispatchEvent(unload)
    expect(unload.defaultPrevented).toBe(true)
    escape()
    expect(host.querySelector('[aria-label="Unsaved skill changes"]')).toBeTruthy()
    expect(useAppStore.getState().closeMarkdownStore).not.toHaveBeenCalled()
    click('Keep editing')
    expect((host.querySelector('textarea') as HTMLTextAreaElement).value).toBe('Authored replacement')
    click('Cancel')
    click('Save & close')
    expect(host.querySelector('[aria-label="Overwrite skill"]')).toBeTruthy()
    expect(useAppStore.getState().updateMarkdownStoreEntry).not.toHaveBeenCalled()
    await act(async () => { button('Overwrite').click(); await Promise.resolve() })
    expect(useAppStore.getState().updateMarkdownStoreEntry).toHaveBeenCalledWith(expect.objectContaining({ id: 'review' }), expect.objectContaining({ body: 'Authored replacement' }))
    expect(host.querySelector('[aria-label="Edit skill"]')).toBeNull()
    const afterSave = new Event('beforeunload', { cancelable: true })
    window.dispatchEvent(afterSave)
    expect(afterSave.defaultPrevented).toBe(false)
  })

  it('never redirects an existing edit to another entry after a store refresh', async () => {
    const { host } = mountSkills()
    const target = useAppStore.getState().markdownStoreEntries[0]
    click('Edit Review code in app')
    changeField('Entry Markdown body', 'Keep on the original path')
    act(() => useAppStore.setState({ markdownStoreEntries: useAppStore.getState().markdownStoreEntries.slice(1) }))
    await confirmSave()
    expect(useAppStore.getState().updateMarkdownStoreEntry).toHaveBeenCalledWith(target, expect.objectContaining({ body: 'Keep on the original path' }))
    expect(useAppStore.getState().createMarkdownStoreEntry).not.toHaveBeenCalled()
    expect(host.querySelector('[aria-label="Edit skill"]')).toBeNull()
  })

  it('creates through the store and retains rejected new drafts until explicitly discarded', async () => {
    useAppStore.setState({ createMarkdownStoreEntry: vi.fn().mockRejectedValueOnce(new Error('Disk full')).mockResolvedValueOnce(true) })
    const { host } = mountSkills()
    click('Add skill'); click('Create')
    expect(button('Save').disabled).toBe(true)
    changeField('Entry title', 'New skill')
    changeField('Entry Markdown body', 'New body')
    await act(async () => { button('Save').click(); await Promise.resolve() })
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Disk full')
    expect((host.querySelector('textarea') as HTMLTextAreaElement).value).toBe('New body')
    click('Cancel'); click('Keep editing')
    await act(async () => { button('Save').click(); await Promise.resolve() })
    expect(useAppStore.getState().createMarkdownStoreEntry).toHaveBeenCalledWith({ title: 'New skill', maker: '', review: '', body: 'New body', group: '' })
    expect(host.querySelector('[aria-label="Create skill"]')).toBeNull()
    click('Add skill'); click('Create'); changeField('Entry title', 'Discard me'); click('Cancel'); click('Discard')
    expect(host.querySelector('[aria-label="Create skill"]')).toBeNull()
    expect(useAppStore.getState().createMarkdownStoreEntry).toHaveBeenCalledTimes(2)
  })

  it('filters all five native provider names without rewriting import source identities', async () => {
    const providers = [
      ['Gemini CLI', 'gemini-cli'], ['Codex CLI', 'codex'], ['Claude Code', 'claude-code'], ['OpenCode', 'opencode'], ['GitHub Copilot CLI', 'github-copilot'],
    ]
    const candidates = providers.map(([, sourceProvider]) => ({ id: sourceProvider, title: `${sourceProvider} skill`, maker: '', review: '', preview: '', sourceProvider, sourcePath: `/tmp/${sourceProvider}.md`, supported: true, validationError: '', collisionPath: '' }))
    const importEntries = vi.fn(async (requests: { sourcePath: string }[]) => requests.map((request) => ({ sourcePath: request.sourcePath, status: 'imported' as const, message: 'Imported' })))
    useAppStore.setState({ previewMarkdownStoreImports: vi.fn(async () => candidates), importMarkdownStoreEntries: importEntries })
    const { host } = mountSkills()
    for (const [label, sourceProvider] of providers) {
      await openProviderImports(label)
      const candidateList = host.querySelector('[data-import-candidates]')!
      expect(candidateList.querySelectorAll('input').length).toBe(1)
      expect(candidateList.textContent).toContain(`${sourceProvider} skill`)
      expect(candidateList.querySelector('select')).toBeNull()
      await act(async () => { button('Import selected').click(); await Promise.resolve() })
      expect(importEntries).toHaveBeenLastCalledWith([{ sourceProvider, sourcePath: `/tmp/${sourceProvider}.md`, conflictAction: 'skip' }])
    }
    expect(useAppStore.getState().previewMarkdownStoreImports).toHaveBeenCalledWith({ includeProviders: true })
  })

  it('keeps partial import failures available without retrying successful items', async () => {
    const candidates = ['one', 'two'].map((id) => ({ id, title: id, maker: '', review: '', preview: '', sourceProvider: 'codex', sourcePath: `/tmp/${id}.md`, supported: true, validationError: '', collisionPath: '' }))
    const importEntries = vi.fn().mockResolvedValueOnce([
      { sourcePath: '/tmp/one.md', status: 'imported', message: 'Imported one' },
      { sourcePath: '/tmp/two.md', status: 'error', message: 'Permission denied' },
    ]).mockResolvedValueOnce([{ sourcePath: '/tmp/two.md', status: 'imported', message: 'Imported two' }])
    useAppStore.setState({ previewMarkdownStoreImports: vi.fn(async () => candidates), importMarkdownStoreEntries: importEntries })
    const { host } = mountSkills()
    await openProviderImports()
    await act(async () => { button('Import selected').click(); await Promise.resolve() })
    expect(host.querySelector('[aria-label="Import skills"]')).toBeTruthy()
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Permission denied')
    expect(host.querySelectorAll('[data-import-candidates] input').length).toBe(1)
    await act(async () => { button('Import selected').click(); await Promise.resolve() })
    expect(importEntries).toHaveBeenLastCalledWith([{ sourceProvider: 'codex', sourcePath: '/tmp/two.md', conflictAction: 'skip' }])
    expect(host.querySelector('[aria-label="Import skills"]')).toBeNull()
  })

  it('ignores a native picker result after its chooser closes and preserves picker cancellation', async () => {
    let resolvePicker: (path: string | null) => void = () => {}
    const browse = vi.fn(() => new Promise<string | null>((resolve) => { resolvePicker = resolve }))
    useAppStore.setState({ browseMarkdownStoreImport: browse })
    const { host } = mountSkills()
    click('Add skill'); click('Import folder'); click('Close add skill')
    await act(async () => { resolvePicker('/tmp/stale-folder'); await Promise.resolve() })
    expect(useAppStore.getState().previewMarkdownStoreImports).not.toHaveBeenCalled()
    expect(host.querySelector('[aria-label="Import skills"]')).toBeNull()
    click('Add skill'); click('Import file')
    await act(async () => { resolvePicker(null); await Promise.resolve() })
    expect(useAppStore.getState().previewMarkdownStoreImports).not.toHaveBeenCalled()
    expect(button('Import file').disabled).toBe(false)
    expect(host.querySelector('[role="alert"]')).toBeNull()
  })

})
