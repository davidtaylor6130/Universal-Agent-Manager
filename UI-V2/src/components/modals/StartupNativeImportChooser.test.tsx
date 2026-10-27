import { act, StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { StartupNativeImportChooser } from './StartupNativeImportChooser'
import { useAppStore } from '../../store/useAppStore'
import type { StartupNativeImportPage } from '../../types/startupNativeImports'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const page = (generation = 'scan-1', ids = ['one', 'two']): StartupNativeImportPage => ({ phase: 'ready', generation, totalCount: ids.length, importedCount: 0, nextOffset: ids.length, errors: [], candidates: ids.map((id) => ({ candidateId: id, nativeSessionId: id, title: id, providerId: 'codex-cli', folderId: id === 'one' ? 'folder-a' : 'folder-b', workspaceDirectory: '/workspace', executionHostId: 'local', updatedAt: '' })) })
let host: HTMLDivElement
let root: ReturnType<typeof createRoot>
let requests: { action: string; payload: Record<string, unknown>; success: (value: string) => void; failure: (code: number, message: string) => void }[]
const button = (label: string) => Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((item) => item.textContent === label || item.getAttribute('aria-label') === label)!
const answer = async (index: number, data: StartupNativeImportPage) => { await act(async () => requests[index].success(JSON.stringify(data))) }
const click = async (label: string) => { await act(async () => button(label).click()) }
function mount() { act(() => root.render(<StartupNativeImportChooser />)) }
describe('startup native history chooser', () => {
  it('keeps the reviewed page and selected histories after assignment', async () => {
    mount(); await answer(0, { ...page('scan-1', ['one']), totalCount: 101, nextOffset: 100 })
    await click('Next')
    const unassigned = { ...page('scan-1', ['last']), totalCount: 101, nextOffset: 101 }
    unassigned.candidates[0].folderId = ''
    await answer(1, unassigned)
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select last"]')!.click())
    await click('Assign workspace for last')
    expect(document.activeElement).toBe(document.body.querySelector('[aria-label="Workspace name for last"]'))
    await click('Stage workspace')
    expect(requests[2].payload).toMatchObject({ offset: 100, limit: 100, folderId: null })
    await answer(2, { ...unassigned, candidates: [{ ...unassigned.candidates[0], folderId: 'staged', newWorkspace: true }] })
    expect(document.body.textContent).toContain('Page 2 of 2')
    expect(document.body.querySelector<HTMLInputElement>('[aria-label="Select last"]')!.checked).toBe(true)
    expect(document.activeElement).toBe(document.body.querySelector('[aria-label="Select last"]'))
  })

  it('Escape cancels an assignment before closing the review', async () => {
    const unassigned = page('scan-1', ['one']); unassigned.candidates[0].folderId = ''
    mount(); await answer(0, unassigned)
    await click('Assign workspace for one')
    expect(document.activeElement).toBe(document.body.querySelector('[aria-label="Workspace name for one"]'))
    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.querySelector('[aria-label="Workspace name for one"]')).toBeNull()
    expect(document.body.querySelector('[role="dialog"]')).not.toBeNull()
    expect(document.activeElement).toBe(document.body.querySelector('[aria-label="Select one"]'))
    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
  })

  it('drops an open assignment on paging so one Escape closes the review', async () => {
    const unassigned = { ...page('scan-1', ['one']), totalCount: 101, nextOffset: 100 }; unassigned.candidates[0].folderId = ''
    mount(); await answer(0, unassigned)
    await click('Assign workspace for one')
    expect(button('Assign workspace for one').getAttribute('aria-expanded')).toBe('true')
    await click('Next'); await answer(1, { ...page('scan-1', ['two']), totalCount: 101, nextOffset: 101 })
    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
  })

  it('stages the workspace when Enter is pressed in the name field', async () => {
    const unassigned = page('scan-1', ['one']); unassigned.candidates[0].folderId = ''
    mount(); await answer(0, unassigned)
    await click('Assign workspace for one')
    await act(async () => document.body.querySelector<HTMLInputElement>('[aria-label="Workspace name for one"]')!.form!.requestSubmit())
    expect(requests[1].action).toBe('assignSystemNativeImportWorkspace')
  })

  it('requires a workspace assignment before importing and stages only the witnessed source', async () => {
    const unassigned = page('scan-1', ['one'])
    unassigned.candidates[0].folderId = ''
    unassigned.candidates[0].workspaceDirectory = '/unregistered/project'
    mount(); await answer(0, unassigned)
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
    expect(button('Import selected').disabled).toBe(true)
    expect(button('Ignore selected').disabled).toBe(false)
    await click('Assign workspace for one')
    expect(document.body.textContent).toContain('/unregistered/project')
    expect(document.body.textContent).toContain('only when a selected chat is imported')
    button('Cancel assignment').focus()
    await click('Cancel assignment')
    expect(document.activeElement).toBe(document.body.querySelector('[aria-label="Select one"]'))
    expect(requests).toHaveLength(1)
    await click('Assign workspace for one')
    button('Stage workspace').focus()
    await click('Stage workspace')
    expect(requests[1]).toMatchObject({ action: 'assignSystemNativeImportWorkspace', payload: { generation: 'scan-1', candidateId: 'one', title: 'project' } })
    const assigned = { ...unassigned, candidates: [{ ...unassigned.candidates[0], folderId: 'proposed', workspaceTitle: 'project', newWorkspace: true }] }
    await answer(1, assigned)
    expect(document.body.textContent).toContain('New workspace: project. Saved with import.')
    expect(document.activeElement).toBe(document.body.querySelector('[aria-label="Select one"]'))
    expect(requests.every((request) => request.action !== 'resolveStartupNativeImports')).toBe(true)
    expect(document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.checked).toBe(true)
    expect(button('Import selected').disabled).toBe(false)
    await click('Import selected')
    expect(requests[2]).toMatchObject({ action: 'resolveStartupNativeImports', payload: { generation: 'scan-1', selectedIds: ['one'] } })
  })

  it('retains the assignment draft after refusal without enabling import', async () => {
    const unassigned = page('scan-1', ['one']); unassigned.candidates[0].folderId = ''
    mount(); await answer(0, unassigned)
    await click('Assign workspace for one'); await click('Stage workspace')
    vi.spyOn(console, 'error').mockImplementation(() => {})
    await act(async () => requests[1].failure(409, 'The workspace changed. Review again.'))
    expect(document.body.querySelector<HTMLInputElement>('[aria-label="Workspace name for one"]')?.value).toBe('workspace')
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain('The workspace changed.')
    expect(button('Import selected').disabled).toBe(true)
    expect(requests).toHaveLength(2)
  })

  it('drops a stale workspace assignment reply after the scan generation changes', async () => {
    const unassigned = page('scan-1', ['one']); unassigned.candidates[0].folderId = ''
    mount(); await answer(0, unassigned)
    await click('Assign workspace for one'); await click('Stage workspace')
    act(() => useAppStore.setState({ startupNativeImports: page('scan-2', ['new']) }))
    await answer(2, page('scan-2', ['new']))
    await answer(1, { ...unassigned, candidates: [{ ...unassigned.candidates[0], folderId: 'stale', newWorkspace: true, workspaceTitle: 'stale assignment' }] })
    expect(document.body.textContent).not.toContain('stale assignment')
    expect(document.body.querySelector('[aria-label="Select new"]')).not.toBeNull()
  })

  it('shift-selects and deselects a range on the current page', async () => {
    mount(); await answer(0, page('scan-1', ['one', 'two', 'three', 'four']))
    const checkbox = (id: string) => document.body.querySelector<HTMLInputElement>(`[aria-label="Select ${id}"]`)!
    act(() => checkbox('one').click())
    act(() => checkbox('three').dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
    expect(['one', 'two', 'three', 'four'].map((id) => checkbox(id).checked)).toEqual([true, true, true, false])
    act(() => checkbox('three').dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
    expect(['one', 'two', 'three', 'four'].map((id) => checkbox(id).checked)).toEqual([false, false, false, false])
  })

  it('shows page counts only with more than one page and pages through pending chats', async () => {
    mount(); await answer(0, { ...page('scan-1', ['one', 'two']), totalCount: 250, nextOffset: 100 })
    expect(document.body.textContent).toContain('Page 1 of 3')
    await click('Next')
    expect(requests[1].payload).toEqual({ offset: 100, limit: 100, folderId: null })
    await answer(1, { ...page('scan-1', ['three']), totalCount: 250, nextOffset: 200 })
    expect(document.body.textContent).toContain('Page 2 of 3')
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select all chats on this page"]')!.click())
    await click('Import selected')
    expect(requests[2].payload).toEqual({ generation: 'scan-1', selectedIds: ['three'], declinedIds: [], folderId: null })
    await answer(2, { ...page('scan-1', []), phase: 'complete', totalCount: 0, importedCount: 1 })
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
  })

  it('hides paging controls when everything fits on one page', async () => {
    mount(); await answer(0, page())
    expect(button('Next')).toBeUndefined()
    expect(button('Previous')).toBeUndefined()
  })

  it('scopes imports to the reviewed workspace and reports partial completion', async () => {
    useAppStore.setState({ startupNativeImportReviewSequence: 1, startupNativeImportFolderId: 'folder-a' })
    // Mount first, then request an explicit workspace review.
    mount(); await answer(0, page())
    act(() => useAppStore.setState({ startupNativeImportReviewSequence: 2 }))
    await answer(1, { ...page('scan-1', ['one', 'two']), totalCount: 150, nextOffset: 100 })
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select all chats on this page"]')!.click())
    await click('Import selected')
    expect(requests[2].payload.folderId).toBe('folder-a')
    await answer(2, { ...page('scan-1', []), phase: 'complete', totalCount: 148, importedCount: 1 })
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain("1 chat couldn't be imported. They stay in the list so you can retry.")
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain('1 failed')
    expect(document.body.querySelector('[role="dialog"]')).not.toBeNull()
  })

  it('loads cached candidates after StrictMode replay and ignores the invalidated initial reply', async () => {
    act(() => root.render(<StrictMode><StartupNativeImportChooser /></StrictMode>))
    expect(requests.length).toBeGreaterThan(0)
    const currentRequest = requests.length - 1
    await answer(currentRequest, page('scan-1', ['current']))
    expect(document.body.querySelector('[aria-label="Select current"]')).not.toBeNull()
    if (currentRequest > 0) {
      await answer(0, page('scan-1', ['stale']))
      expect(document.body.querySelector('[aria-label="Select stale"]')).toBeNull()
      expect(document.body.querySelector('[aria-label="Select current"]')).not.toBeNull()
    }
    expect(button('Close import chats').disabled).toBe(false)
    await click('Close import chats')
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
    expect(requests.every((request) => request.action === 'getStartupNativeImports')).toBe(true)
  })
  beforeEach(() => {
    vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
    requests = []
    window.cefQuery = ({ request, onSuccess, onFailure }) => { const parsed = JSON.parse(request); requests.push({ ...parsed, success: onSuccess, failure: onFailure }) }
    useAppStore.setState({ startupNativeImports: page(), startupNativeImportReviewSequence: 0, startupNativeImportFolderId: null })
    host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host)
  })
  afterEach(() => { act(() => root.unmount()); host.remove(); delete window.cefQuery; vi.restoreAllMocks(); vi.unstubAllGlobals() })

  it('defaults unchecked and close dismisses locally without a native decision or repeated startup prompt', async () => {
    const before = useAppStore.getState()
    mount(); expect(requests[0].payload).toEqual({ offset: 0, limit: 100, folderId: null })
    await answer(0, page())
    expect(Array.from(document.body.querySelectorAll<HTMLInputElement>('input')).every((input) => !input.checked)).toBe(true)
    expect(button('Import selected').disabled).toBe(true)
    await click('Close import chats')
    act(() => useAppStore.setState({ startupNativeImports: { ...page(), importedCount: 1 } }))
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
    expect(requests.map((request) => request.action)).toEqual(['getStartupNativeImports'])
    expect(useAppStore.getState().activeSessionId).toBe(before.activeSessionId)
    expect(useAppStore.getState().sessions).toBe(before.sessions)
  })

  it('uses provider display names and omits the redundant local host label', async () => {
    mount(); await answer(0, page('scan-1', ['one']))
    const dialog = document.body.querySelector('[role="dialog"]')!
    expect(dialog.getAttribute('aria-label')).toBe('Import chats')
    expect(dialog.textContent).toContain('Import chats from other tools')
    expect(dialog.querySelector('[aria-label="Codex CLI"]')).not.toBeNull()
    expect(dialog.textContent).not.toContain('Codex CLI')
    expect(dialog.textContent).not.toContain('local ·')
    expect(dialog.textContent).toContain('workspace')
    expect(dialog.textContent).not.toContain('/workspace')
    const folder = dialog.querySelector('.uam-rv-row__meta')
    expect(folder?.getAttribute('style')).toContain('var(--text-3)')
    expect(button('Import selected').className).toContain('uam-btn--primary')
    expect(button('Close import chats').className).toContain('uam-iconbtn--ghost')
  })

  it('distinguishes history loading from decision saving', async () => {
    mount()
    expect(document.body.querySelector('[role="status"]')?.textContent).toBe('Loading history…')
    expect(document.body.querySelector('[role="status"]')?.getAttribute('style')).toContain('var(--text-3)')
    await answer(0, page())
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
    await click('Import selected')
    expect(document.body.querySelector('[role="status"]')?.textContent).toBe('Saving…')
    await act(async () => requests[1].failure(409, 'Disk full'))
  })

  it('shows remote host identity with provider display name', async () => {
    mount()
    const remotePage = page('scan-1', ['remote'])
    remotePage.candidates[0].executionHostId = 'runner-a'
    await answer(0, remotePage)
    expect(document.body.querySelector('[aria-label="Codex CLI · runner-a"]')).not.toBeNull()
    expect(document.body.querySelector('[role="dialog"]')?.textContent).toContain('workspace')
  })

  it('sends only checked import or ignore decisions and preserves selections after save failure', async () => {
    mount(); await answer(0, page())
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
    await click('Import selected'); await click('Import selected')
    expect(requests[1].payload).toEqual({ generation: 'scan-1', selectedIds: ['one'], declinedIds: [], folderId: null })
    expect(requests).toHaveLength(2)
    await act(async () => requests[1].failure(409, 'Disk full'))
    expect(document.body.querySelector('[role="alert"]')?.textContent).toBe('Disk full')
    expect(document.body.querySelector('[role="alert"]')?.getAttribute('style')).toContain('var(--error)')
    expect(document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.checked).toBe(true)
    act(() => useAppStore.setState({ startupNativeImports: { ...page(), phase: 'importing' } }))
    act(() => useAppStore.setState({ startupNativeImports: page() }))
    expect(requests).toHaveLength(2)
    expect(button('Import selected').disabled).toBe(false)
    await click('Import selected'); await answer(2, { ...page('scan-1', ['two']), phase: 'importing' })
    act(() => useAppStore.setState({ startupNativeImports: { ...page('scan-1', ['two']), phase: 'importing' } }))
    act(() => useAppStore.setState({ startupNativeImports: { ...page('scan-1', ['two']), importedCount: 1 } }))
    await answer(3, { ...page('scan-1', ['two']), importedCount: 1 })
    expect(document.body.querySelector('[aria-label="Select one"]')).toBeNull()
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select two"]')!.click())
    await click('Ignore selected')
    expect(requests[4].payload).toEqual({ generation: 'scan-1', selectedIds: [], declinedIds: ['two'], folderId: null })
    await answer(4, { ...page('scan-1', []), phase: 'complete' })
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
  })

  it('reopens cached review for a folder and ignores older page and resolve replies', async () => {
    mount(); await answer(0, page())
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
    await click('Import selected')
    act(() => useAppStore.getState().openStartupNativeImportReview('folder-b'))
    expect(requests[2].action).toBe('getStartupNativeImports')
    expect(requests[2].payload).toEqual({ offset: 0, limit: 100, folderId: 'folder-b' })
    await answer(2, page('scan-1', ['two'])); await answer(1, page('scan-1', []))
    expect(document.body.querySelector('[aria-label="Select two"]')).not.toBeNull()
    expect(document.body.querySelector('[aria-label="Select one"]')).toBeNull()
    act(() => useAppStore.setState({ startupNativeImports: page('scan-2', ['new']) }))
    expect(requests[3].action).toBe('getStartupNativeImports')
    act(() => useAppStore.getState().openStartupNativeImportReview())
    await answer(4, page('scan-2', ['new'])); await answer(3, page('scan-2', ['old']))
    expect(document.body.querySelector('[aria-label="Select new"]')).not.toBeNull()
    expect(document.body.querySelector('[aria-label="Select old"]')).toBeNull()
  })

  it.each([true, false])('keeps resolve responses scoped to the selected workspace (import: %s)', async (importSelected) => {
    mount()
    act(() => useAppStore.getState().openStartupNativeImportReview('folder-b'))
    await answer(1, page('scan-1', ['two']))
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select two"]')!.click())
    await click(importSelected ? 'Import selected' : 'Ignore selected')
    expect(requests[2].payload).toEqual({ generation: 'scan-1', selectedIds: importSelected ? ['two'] : [], declinedIds: importSelected ? [] : ['two'], folderId: 'folder-b' })
    const scopedReply = importSelected
      ? { ...page('scan-1', []), phase: 'importing' as const }
      : { ...page('scan-1', []), phase: 'complete' as const }
    await answer(2, scopedReply)
    expect(document.body.querySelector('[aria-label="Select one"]')).toBeNull()
    expect(document.body.querySelector('[aria-label="Select two"]')).toBeNull()
  })

  it('pages bounded metadata and clears page selection before deciding', async () => {
    mount(); await answer(0, { ...page(), totalCount: 150, nextOffset: 100 })
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
    await click('Next')
    expect(requests[1].payload).toEqual({ offset: 100, limit: 100, folderId: null })
    await answer(1, { ...page('scan-1', ['last']), totalCount: 150, nextOffset: 150 })
    expect(button('Import selected').disabled).toBe(true)
    expect(button('Next').disabled).toBe(true)
    await click('Previous'); expect(requests[2].payload).toEqual({ offset: 0, limit: 100, folderId: null })
  })

  it('owns Escape and Tab and restores focus without stealing from a newer dialog', async () => {
    const trigger = document.createElement('button'); document.body.appendChild(trigger); trigger.focus()
    mount(); await answer(0, { ...page(), totalCount: 150, nextOffset: 100 })
    const parent = vi.fn(); document.addEventListener('keydown', parent)
    act(() => { button('Next').focus(); window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', bubbles: true, cancelable: true })) })
    expect(document.activeElement).toBe(button('Close import chats'))
    act(() => document.activeElement!.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })))
    expect(parent).not.toHaveBeenCalled(); expect(document.activeElement).toBe(trigger)
    document.removeEventListener('keydown', parent)
    act(() => useAppStore.getState().openStartupNativeImportReview())
    const newer = document.createElement('div'); newer.setAttribute('aria-modal', 'true'); newer.style.zIndex = '90'; newer.tabIndex = -1; document.body.appendChild(newer); newer.focus()
    act(() => root.unmount())
    expect(document.activeElement).toBe(newer)
    newer.remove(); trigger.remove(); root = createRoot(host)
  })
  it('returns automatic discovery review to its trigger after the busy button lost focus', async () => {
    const trigger = document.createElement('button')
    trigger.dataset.nativeHistoryDiscovery = ''
    trigger.disabled = true
    document.body.appendChild(trigger)
    try {
      mount(); await answer(0, page())
      trigger.disabled = false
      await click('Close import chats')
      expect(document.activeElement).toBe(trigger)
    } finally { trigger.remove() }
  })
  it.each(['get', 'resolve'])('rejects malformed %s pages without losing current selection and can retry', async (operation) => {
    mount()
    if (operation === 'resolve') {
      await answer(0, page())
      act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
      await click('Import selected')
    }
    await act(async () => requests[operation === 'get' ? 0 : 1].success(JSON.stringify({ generation: 'scan-1' })))
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain('Invalid history response')
    if (operation === 'resolve') {
      expect(document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.checked).toBe(true)
      expect(button('Import selected').disabled).toBe(true)
      await click('Reload')
      await answer(2, page('scan-1', ['one']))
      expect(document.body.querySelector('[aria-label="Select one"]')).not.toBeNull()
      act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
      await click('Import selected')
      expect(requests[3].payload.selectedIds).toEqual(['one'])
    } else {
      await click('Reload'); await answer(1, page())
      expect(document.body.querySelector('[aria-label="Select two"]')).not.toBeNull()
    }
  })

  it('requires a refresh before retrying an import with an uncertain transport result', async () => {
    mount(); await answer(0, page())
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
    await click('Import selected')
    await act(async () => requests[1].failure(0, 'Connection lost. Check the chat before retrying; the action may have reached UAM.'))
    expect(button('Import selected').disabled).toBe(true)
    expect(requests).toHaveLength(2)
    await click('Reload')
    expect(requests[2].action).toBe('getStartupNativeImports')
    await answer(2, page())
    expect(button('Import selected').disabled).toBe(true)
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
    expect(button('Import selected').disabled).toBe(false)
  })

  it('keeps accepted imports open through publication and refreshes requeued failures without scanning', async () => {
    mount(); await answer(0, page('scan-1', ['one']))
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
    await click('Import selected')
    await answer(1, { ...page('scan-1', []), phase: 'importing' })
    expect(document.body.querySelector('[role="dialog"]')).not.toBeNull()
    expect(button('Import selected').disabled).toBe(true)
    act(() => useAppStore.setState({ startupNativeImports: { ...page('scan-1', []), phase: 'importing' } }))
    act(() => useAppStore.setState({ startupNativeImports: page('scan-1', ['one']) }))
    expect(requests[2].action).toBe('getStartupNativeImports')
    await answer(2, { ...page('scan-1', ['one']), errors: ['Selected chat failed to save.'] })
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain("None of the selected chats could be imported.")
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
    await click('Import selected'); await answer(3, { ...page('scan-1', []), phase: 'importing' })
    act(() => useAppStore.setState({ startupNativeImports: { ...page('scan-1', []), phase: 'complete', importedCount: 1 } }))
    await answer(4, { ...page('scan-1', []), phase: 'complete', importedCount: 1 })
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
    expect(requests.every((request) => ['getStartupNativeImports', 'resolveStartupNativeImports'].includes(request.action))).toBe(true)
  })

  it.each([false, true])('refreshes identical ready completion even when importing push is coalesced (early cache reply: %s)', async (earlyCacheReply) => {
    useAppStore.setState({ startupNativeImports: page('scan-1', ['one']) })
    mount(); await answer(0, page('scan-1', ['one']))
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
    await click('Import selected'); await answer(1, { ...page('scan-1', []), phase: 'importing' })
    expect(requests).toHaveLength(2)
    if (earlyCacheReply) {
      act(() => useAppStore.setState({ startupNativeImports: page('scan-1', ['one']) }))
      expect(requests[2].action).toBe('getStartupNativeImports')
      await answer(2, { ...page('scan-1', []), phase: 'importing' })
      expect(requests).toHaveLength(3)
    }
    act(() => useAppStore.setState({ startupNativeImports: page('scan-1', ['one']) }))
    const index = earlyCacheReply ? 3 : 2
    expect(requests[index].action).toBe('getStartupNativeImports')
    await answer(index, page('scan-1', ['one']))
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain("None of the selected chats could be imported.")
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
    expect(button('Import selected').disabled).toBe(false)
    expect(requests).toHaveLength(index + 1)
  })

  it('keeps a discarded selected import visible when publication completes without saving it', async () => {
    useAppStore.setState({ startupNativeImports: page('scan-1', ['one']) })
    mount(); await answer(0, page('scan-1', ['one']))
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
    await click('Import selected'); await answer(1, { ...page('scan-1', []), phase: 'importing' })
    act(() => useAppStore.setState({ startupNativeImports: { ...page('scan-1', []), phase: 'complete' } }))
    await answer(2, { ...page('scan-1', []), phase: 'complete' })
    expect(document.body.querySelector('[role="dialog"]')).not.toBeNull()
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain('no longer available')
    expect(document.body.querySelector('[role="alert"]')?.textContent).not.toContain('retry')
    expect(button('Import selected').disabled).toBe(true)
    expect(requests).toHaveLength(3)
    await click('Close import chats')
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
    expect(requests).toHaveLength(3)
  })

})
