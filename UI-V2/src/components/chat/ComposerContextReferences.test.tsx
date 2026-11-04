import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { ComposerReferenceChips, useComposerReferences } from './ComposerContextReferences'
import { captureWorkspaceFileReference, draftFileMentions } from '../../utils/fileContextReferences'
import { captureContextReference } from '../../utils/contextReferences'
import { captureRepositoryContextReference } from '../../utils/repositoryContextReferences'
import { readChatComposerDraft, writeChatComposerDraft } from '../../utils/composerDraftStorage'
import { useAppStore } from '../../store/useAppStore'

vi.mock('../../utils/fileContextReferences', async (original) => ({ ...await original<typeof import('../../utils/fileContextReferences')>(), captureWorkspaceFileReference: vi.fn() }))
vi.mock('../../utils/repositoryContextReferences', () => ({ captureRepositoryContextReference: vi.fn() }))

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

// The capture actions are offered by ChatView's @ palette; these buttons stand in for its selections.
function ComposerContextReferences({ chatId, draft, disabled, onBusyChange }: { chatId: string; draft: string; disabled: boolean; onBusyChange: (busy: boolean) => void }) {
  const references = useComposerReferences({ chatId, ownerKey: chatId, disabled, onBusyChange })
  const candidates = draftFileMentions(draft).filter((path) => !references.fileCaptured(path))
  return <div className="uam-composer-surface">
    {candidates.map((path) => <button key={path} type="button" disabled={references.actionsDisabled} onClick={() => references.captureFile(path)}>{`Reference @${path}`}</button>)}
    <button type="button" disabled={references.actionsDisabled} onClick={() => references.captureRepository('branch')}>Reference branch</button>
    <button type="button" disabled={references.actionsDisabled} onClick={() => references.captureRepository('worktree')}>Reference worktree</button>
    <ComposerReferenceChips references={references} />
    <textarea aria-label="Composer input" />
  </div>
}
const snapshot = captureContextReference({ kind: 'file', id: 'captured-file', sourceChatId: 'file-chip', sourcePath: '/work/readme.md',
  executionHostId: 'local', workspaceDirectory: '/work', capturedAt: '2026-10-23T12:00:00Z', label: 'readme.md', text: 'Captured text, not a current read.' })!
beforeEach(() => {
  // JSDOM has no layout observer; keep real Radix focus/tooltip behavior in these interaction cases.
  vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
  vi.mocked(captureWorkspaceFileReference).mockReset()
  vi.mocked(captureRepositoryContextReference).mockReset()
  useAppStore.setState({ sessions: [{ id: 'file-chip', name: 'Fixture', folderId: null, createdAt: new Date('2026-10-23T12:00:00Z'), updatedAt: new Date(),
    workspaceDirectory: '/work', workspaceIsolationKind: 'gitWorktree', workspaceWorktreeDirectory: '/worktree' }] })
  writeChatComposerDraft('file-chip', { text: 'Read @readme.md', attachments: [], references: [] })
})
afterEach(() => vi.unstubAllGlobals())

it.each(['branch', 'worktree'] as const)('captures, inspects and removes a %s without running a workspace action', async (kind) => {
  const repository = captureContextReference({ kind, id: `capture-${kind}`, sourceChatId: 'file-chip', sourceChatCreatedAt: '2026-10-23T12:00:00Z',
    executionHostId: 'local', workspaceDirectory: '/worktree', branchName: 'feature/context', ...(kind === 'worktree' ? { worktreeDirectory: '/worktree' } : {}),
    capturedAt: '2026-10-23T12:01:00Z', label: kind === 'branch' ? 'feature/context' : 'worktree', text: 'Captured repository observation' })!
  vi.mocked(captureRepositoryContextReference).mockResolvedValue(repository)
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  try {
    act(() => root.render(<ComposerContextReferences chatId="file-chip" draft="" disabled={false} onBusyChange={() => {}} />))
    expect(captureRepositoryContextReference).not.toHaveBeenCalled()
    await act(async () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === `Reference ${kind}`)!.click())
    expect(captureRepositoryContextReference).toHaveBeenCalledWith('file-chip', kind)
    expect(host.querySelector('.uam-context-chip')?.textContent).toContain(`${kind} snapshot`)
    act(() => host.querySelector<HTMLButtonElement>('.uam-context-chip__body')!.click())
    expect(document.querySelector('[role="dialog"]')?.textContent).toContain(repository.text)
    expect(document.querySelector(`[role="dialog"] time[datetime="${repository.sourceChatCreatedAt}"]`)).toBeTruthy()
    expect(captureRepositoryContextReference).toHaveBeenCalledTimes(1)
    expect(captureWorkspaceFileReference).not.toHaveBeenCalled()
    act(() => document.querySelector<HTMLButtonElement>('[aria-label="Close captured context"]')!.click())
    act(() => host.querySelector<HTMLButtonElement>('.uam-context-chip__remove')!.click())
    expect(readChatComposerDraft('file-chip').references).toBeUndefined()
  } finally { act(() => root.unmount()); host.remove() }
})

it('captures only on explicit selection, shows the saved snapshot and removes just that reference', async () => {
  vi.mocked(captureWorkspaceFileReference).mockResolvedValue(snapshot)
  const busy = vi.fn()
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  try {
    act(() => root.render(<ComposerContextReferences chatId="file-chip" draft="Read @readme.md" disabled={false} onBusyChange={busy} />))
    expect(captureWorkspaceFileReference).not.toHaveBeenCalled()
    await act(async () => host.querySelector<HTMLButtonElement>('button')!.click())
    expect(captureWorkspaceFileReference).toHaveBeenCalledWith('file-chip', 'readme.md')
    expect(busy.mock.calls.slice(0, 2)).toEqual([[true], [false]])
    expect(readChatComposerDraft('file-chip')).toMatchObject({ text: 'Read @readme.md', references: [snapshot] })
    act(() => host.querySelector<HTMLButtonElement>('.uam-context-chip__body')!.click())
    const dialog = document.querySelector('[role="dialog"]')!
    expect(dialog.textContent).toContain(snapshot.text)
    expect(dialog.querySelector('time')?.dateTime).toBe(snapshot.capturedAt)
    expect(dialog.textContent).toContain(snapshot.workspaceDirectory)
    expect(captureWorkspaceFileReference).toHaveBeenCalledTimes(1)
    act(() => document.querySelector<HTMLButtonElement>('[aria-label="Close captured context"]')!.click())
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Remove readme.md reference"]')!.click())
    expect(readChatComposerDraft('file-chip')).toEqual({ text: 'Read @readme.md', attachments: [] })
    expect(host.querySelector('.uam-context-chip')).toBeNull()
  } finally { act(() => root.unmount()); host.remove() }
})

it('does not publish a completed capture after its composer unmounts', async () => {
  let finish!: (value: typeof snapshot) => void
  vi.mocked(captureWorkspaceFileReference).mockImplementation(() => new Promise((resolve) => { finish = resolve }))
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  act(() => root.render(<ComposerContextReferences chatId="file-chip" draft="@readme.md" disabled={false} onBusyChange={() => {}} />))
  await act(async () => host.querySelector<HTMLButtonElement>('button')!.click())
  act(() => root.unmount())
  await act(async () => { finish(snapshot); await Promise.resolve() })
  expect(readChatComposerDraft('file-chip').references).toBeUndefined()
  host.remove()
})

it('keeps the draft after a refused file read and displays a recoverable error', async () => {
  vi.mocked(captureWorkspaceFileReference).mockRejectedValue(new Error('File is outside this workspace.'))
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  try {
    act(() => root.render(<ComposerContextReferences chatId="file-chip" draft="@readme.md" disabled={false} onBusyChange={() => {}} />))
    await act(async () => host.querySelector<HTMLButtonElement>('button')!.click())
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('outside this workspace')
    expect(readChatComposerDraft('file-chip').text).toBe('Read @readme.md')
    expect(readChatComposerDraft('file-chip').references).toBeUndefined()
  } finally { act(() => root.unmount()); host.remove() }
})

it('keeps duplicate snapshots distinguishable and moves removal focus to the next chip then the composer input', () => {
  const other = { ...snapshot, id: 'other-same-label', sourcePath: '/other/readme.md' }
  writeChatComposerDraft('file-chip', { text: '', attachments: [], references: [snapshot, other] })
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  try {
    act(() => root.render(<ComposerContextReferences chatId="file-chip" draft="" disabled={false} onBusyChange={() => {}} />))
    const removes = [...host.querySelectorAll<HTMLButtonElement>('.uam-context-chip__remove')]
    expect(removes.map((button) => button.getAttribute('aria-label'))).toEqual(['Remove readme.md (1) reference', 'Remove readme.md (2) reference'])
    removes[0].focus()
    act(() => removes[0].click())
    expect(document.activeElement).toBe(host.querySelector('.uam-context-chip__body'))
    expect(readChatComposerDraft('file-chip').references).toEqual([other])
    act(() => host.querySelector<HTMLButtonElement>('.uam-context-chip__remove')!.click())
    expect(document.activeElement).toBe(host.querySelector('textarea'))
  } finally { act(() => root.unmount()); host.remove() }
})

it.each([0, 1, 2])('keeps literal suffixes distinct and removes only selected snapshot %i', (removeIndex) => {
  const selected = ['notes', 'notes', 'notes (1)'].map((label, index) => ({ ...snapshot,
    id: `literal-label-${index}`, label, sourcePath: `/work/${index}/${label}`, text: `Snapshot ${index}` }))
  writeChatComposerDraft('file-chip', { text: 'Keep this draft', attachments: [], references: selected })
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  try {
    act(() => root.render(<ComposerContextReferences chatId="file-chip" draft="" disabled={false} onBusyChange={() => {}} />))
    const names = [...host.querySelectorAll('.uam-context-chip__name')].map((chip) => chip.textContent)
    const removes = [...host.querySelectorAll<HTMLButtonElement>('.uam-context-chip__remove')]
    expect(names).toHaveLength(3)
    expect(new Set(names).size).toBe(3)
    // Meta numbers match the chosen suffixes so truncated names stay distinguishable.
    expect([...host.querySelectorAll('.uam-context-chip__meta')].map((meta) => meta.textContent)).toEqual(['snapshot · 2', 'snapshot · 3', 'snapshot'])
    expect(names).toEqual(['notes (2)', 'notes (3)', 'notes (1)'])
    expect(new Set(removes.map((button) => button.getAttribute('aria-label'))).size).toBe(3)
    expect(readChatComposerDraft('file-chip').references).toEqual(selected)
    act(() => removes[removeIndex].click())
    expect(readChatComposerDraft('file-chip')).toEqual({ text: 'Keep this draft', attachments: [],
      references: selected.filter((_, index) => index !== removeIndex) })
    expect(captureWorkspaceFileReference).not.toHaveBeenCalled()
    expect(captureRepositoryContextReference).not.toHaveBeenCalled()
  } finally { act(() => root.unmount()); host.remove() }
})

it('names the captured text region and preserves chip focus when the backdrop dismisses inspection', () => {
  writeChatComposerDraft('file-chip', { text: '', attachments: [], references: [snapshot] })
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  try {
    act(() => root.render(<ComposerContextReferences chatId="file-chip" draft="" disabled={false} onBusyChange={() => {}} />))
    const chip = host.querySelector<HTMLButtonElement>('.uam-context-chip__body')!
    chip.focus(); act(() => chip.click())
    const dialog = document.querySelector('[role="dialog"]')!
    expect(dialog.querySelector('[role="region"][aria-label="Captured snapshot text"]')?.textContent).toBe(snapshot.text)
    expect(dialog.textContent).toContain('Host: local')
    expect(dialog.textContent).toContain('Workspace: /work')
    expect(dialog.querySelector('time')?.getAttribute('datetime')).toBe(snapshot.capturedAt)
    const down = new MouseEvent('mousedown', { bubbles: true, cancelable: true })
    act(() => document.querySelector('.uam-overlay')!.dispatchEvent(down))
    expect(down.defaultPrevented).toBe(true)
    expect(document.querySelector('[role="dialog"]')).toBeNull()
    expect(document.activeElement).toBe(chip)
    expect(captureWorkspaceFileReference).not.toHaveBeenCalled()
  } finally { act(() => root.unmount()); host.remove() }
})

it('requires a projected configured worktree before offering its capture and suppresses a canonical dot-prefix file spelling', async () => {
  useAppStore.setState((state) => ({ sessions: state.sessions.map((chat) => ({ ...chat, workspaceIsolationKind: undefined, workspaceWorktreeDirectory: undefined })) }))
  writeChatComposerDraft('file-chip', { text: '@./docs/readme.md', attachments: [], references: [{ ...snapshot, sourcePath: 'docs/readme.md' }] })
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  try {
    act(() => root.render(<ComposerContextReferences chatId="file-chip" draft="@./docs/readme.md" disabled={false} onBusyChange={() => {}} />))
    const worktree = [...host.querySelectorAll<HTMLButtonElement>('button')].find((button) => button.textContent === 'Reference worktree')!
    act(() => worktree.click())
    expect(captureRepositoryContextReference).not.toHaveBeenCalled()
    expect(host.textContent).not.toContain('Reference @')
    expect(host.textContent).toContain('Snapshots are included with your message')
    act(() => useAppStore.setState((state) => ({ sessions: state.sessions.map((chat) => ({ ...chat, workspaceIsolationKind: 'gitWorktree', workspaceWorktreeDirectory: '/configured/worktree' })) })))
    await act(async () => worktree.click())
    expect(captureRepositoryContextReference).toHaveBeenCalledWith('file-chip', 'worktree')
  } finally { act(() => root.unmount()); host.remove() }
})

it('numbers duplicates by occurrence in shrink-safe meta text, names the dialog with that label and suppresses backslash spellings', () => {
  const between = { ...snapshot, id: 'between', label: 'main.ts', sourcePath: 'src/main.ts' }
  const other = { ...snapshot, id: 'other-same-label', sourcePath: 'docs/readme.md' }
  writeChatComposerDraft('file-chip', { text: '', attachments: [], references: [snapshot, between, other] })
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  try {
    act(() => root.render(<ComposerContextReferences chatId="file-chip" draft={'@.\\docs\\readme.md'} disabled={false} onBusyChange={() => {}} />))
    expect([...host.querySelectorAll('.uam-context-chip__name')].map((chip) => chip.textContent)).toEqual(['readme.md (1)', 'main.ts', 'readme.md (2)'])
    expect([...host.querySelectorAll('.uam-context-chip__meta')].map((meta) => meta.textContent)).toEqual(['snapshot · 1', 'snapshot', 'snapshot · 2'])
    expect(host.textContent).not.toContain('Reference @')
    act(() => host.querySelectorAll<HTMLButtonElement>('.uam-context-chip__body')[2].click())
    expect(document.querySelector('[role="dialog"]')?.getAttribute('aria-label')).toBe('Captured readme.md (2)')
  } finally { act(() => root.unmount()); host.remove() }
})


it('discards a capture that finishes after the composer switches to another chat in place', async () => {
  let finish!: (value: typeof snapshot) => void
  vi.mocked(captureWorkspaceFileReference).mockImplementation(() => new Promise((resolve) => { finish = resolve }))
  useAppStore.setState((state) => ({ sessions: [...state.sessions, { ...state.sessions[0], id: 'other-chat' }] }))
  writeChatComposerDraft('other-chat', { text: '', attachments: [], references: [] })
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  try {
    act(() => root.render(<ComposerContextReferences chatId="file-chip" draft="@readme.md" disabled={false} onBusyChange={() => {}} />))
    await act(async () => host.querySelector<HTMLButtonElement>('button')!.click())
    expect(host.querySelector('[data-reference-status]')?.textContent).toContain('Reading readme.md')
    act(() => root.render(<ComposerContextReferences chatId="other-chat" draft="" disabled={false} onBusyChange={() => {}} />))
    expect(host.querySelector('[data-reference-status]')?.textContent).toBe('')
    await act(async () => { finish(snapshot); await Promise.resolve() })
    expect(readChatComposerDraft('file-chip').references).toBeUndefined()
    expect(readChatComposerDraft('other-chat').references ?? []).toEqual([])
    expect(host.querySelector('.uam-context-chip')).toBeNull()
  } finally { act(() => root.unmount()); host.remove() }
})
