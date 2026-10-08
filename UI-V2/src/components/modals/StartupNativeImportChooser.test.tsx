import { act } from 'react'
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
const button = (label: string) => Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((item) => item.textContent === label)!
const answer = async (index: number, data: StartupNativeImportPage) => { await act(async () => requests[index].success(JSON.stringify(data))) }
const click = async (label: string) => { await act(async () => button(label).click()) }
function mount() { act(() => root.render(<StartupNativeImportChooser />)) }
describe('startup native history chooser', () => {
  beforeEach(() => {
    requests = []
    window.cefQuery = ({ request, onSuccess, onFailure }) => { const parsed = JSON.parse(request); requests.push({ ...parsed, success: onSuccess, failure: onFailure }) }
    useAppStore.setState({ startupNativeImports: page(), startupNativeImportReviewSequence: 0, startupNativeImportFolderId: null })
    host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host)
  })
  afterEach(() => { act(() => root.unmount()); host.remove(); delete window.cefQuery; vi.restoreAllMocks() })

  it('defaults unchecked and Later dismisses locally without a native decision or repeated startup prompt', async () => {
    const before = useAppStore.getState()
    mount(); expect(requests[0].payload).toEqual({ offset: 0, limit: 100 })
    await answer(0, page())
    expect(Array.from(document.body.querySelectorAll<HTMLInputElement>('input')).every((input) => !input.checked)).toBe(true)
    expect(button('Import selected').disabled).toBe(true)
    await click('Later')
    act(() => useAppStore.setState({ startupNativeImports: { ...page(), importedCount: 1 } }))
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
    expect(requests.map((request) => request.action)).toEqual(['getStartupNativeImports'])
    expect(useAppStore.getState().activeSessionId).toBe(before.activeSessionId)
    expect(useAppStore.getState().sessions).toBe(before.sessions)
  })

  it('sends only checked import or ignore decisions and preserves selections after save failure', async () => {
    mount(); await answer(0, page())
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
    await click('Import selected'); await click('Import selected')
    expect(requests[1].payload).toEqual({ generation: 'scan-1', selectedIds: ['one'], declinedIds: [] })
    expect(requests).toHaveLength(2)
    await act(async () => requests[1].failure(409, 'Disk full'))
    expect(document.body.querySelector('[role="alert"]')?.textContent).toBe('Disk full')
    expect(document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.checked).toBe(true)
    await click('Import selected'); await answer(2, { ...page('scan-1', ['two']), phase: 'importing' })
    act(() => useAppStore.setState({ startupNativeImports: { ...page('scan-1', ['two']), phase: 'importing' } }))
    act(() => useAppStore.setState({ startupNativeImports: { ...page('scan-1', ['two']), importedCount: 1 } }))
    await answer(3, { ...page('scan-1', ['two']), importedCount: 1 })
    expect(document.body.querySelector('[aria-label="Select one"]')).toBeNull()
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select two"]')!.click())
    await click('Ignore selected')
    expect(requests[4].payload).toEqual({ generation: 'scan-1', selectedIds: [], declinedIds: ['two'] })
    await answer(4, { ...page('scan-1', []), phase: 'complete' })
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
  })

  it('reopens cached review for a folder and ignores older page and resolve replies', async () => {
    mount(); await answer(0, page())
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
    await click('Import selected')
    act(() => useAppStore.getState().openStartupNativeImportReview('folder-b'))
    expect(requests[2].action).toBe('getStartupNativeImports')
    await answer(2, page()); await answer(1, page('scan-1', []))
    expect(document.body.querySelector('[aria-label="Select two"]')).not.toBeNull()
    expect(document.body.querySelector('[aria-label="Select one"]')).toBeNull()
    act(() => useAppStore.setState({ startupNativeImports: page('scan-2', ['new']) }))
    expect(requests[3].action).toBe('getStartupNativeImports')
    act(() => useAppStore.getState().openStartupNativeImportReview())
    await answer(4, page('scan-2', ['new'])); await answer(3, page('scan-2', ['old']))
    expect(document.body.querySelector('[aria-label="Select new"]')).not.toBeNull()
    expect(document.body.querySelector('[aria-label="Select old"]')).toBeNull()
  })

  it('pages bounded metadata and clears page selection before deciding', async () => {
    mount(); await answer(0, { ...page(), totalCount: 150, nextOffset: 100 })
    act(() => document.body.querySelector<HTMLInputElement>('[aria-label="Select one"]')!.click())
    await click('Next')
    expect(requests[1].payload).toEqual({ offset: 100, limit: 100 })
    await answer(1, { ...page('scan-1', ['last']), totalCount: 150, nextOffset: 150 })
    expect(button('Import selected').disabled).toBe(true)
    expect(button('Next').disabled).toBe(true)
    await click('Previous'); expect(requests[2].payload).toEqual({ offset: 0, limit: 100 })
  })

  it('owns Escape and Tab and restores focus without stealing from a newer dialog', async () => {
    const trigger = document.createElement('button'); document.body.appendChild(trigger); trigger.focus()
    mount(); await answer(0, page())
    const parent = vi.fn(); document.addEventListener('keydown', parent)
    act(() => { button('Next').focus(); window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', bubbles: true, cancelable: true })) })
    expect(document.activeElement).toBe(button('Later'))
    act(() => document.activeElement!.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })))
    expect(parent).not.toHaveBeenCalled(); expect(document.activeElement).toBe(trigger)
    document.removeEventListener('keydown', parent)
    act(() => useAppStore.getState().openStartupNativeImportReview())
    const newer = document.createElement('div'); newer.setAttribute('aria-modal', 'true'); newer.style.zIndex = '90'; newer.tabIndex = -1; document.body.appendChild(newer); newer.focus()
    act(() => root.unmount())
    expect(document.activeElement).toBe(newer)
    newer.remove(); trigger.remove(); root = createRoot(host)
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
      await click('Import selected')
      expect(requests[2].payload.selectedIds).toEqual(['one'])
      await answer(2, page('scan-1', ['two']))
    } else {
      await click('Reload'); await answer(1, page())
    }
    expect(document.body.querySelector('[aria-label="Select two"]')).not.toBeNull()
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
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain('could not be imported')
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
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain('could not be imported')
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
    await click('Later')
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
    expect(requests).toHaveLength(3)
  })

})
