import { beforeEach, expect, it } from 'vitest'
import { useAppStore } from '../store/useAppStore'
import { captureContextReference } from './contextReferences'
import { readChatComposerDraft, writeChatComposerDraft } from './composerDraftStorage'
import { acknowledgeComposerReferences, captureComposerReferenceSubmission, composerReferenceSubmissionIsCurrent } from './composerReferenceSubmission'

const born = '2026-10-23T12:00:00Z'
const selected = captureContextReference({ kind: 'branch', id: 'selected-reference', sourceChatId: 'reference-send', sourceChatCreatedAt: born,
  executionHostId: 'local', workspaceDirectory: '/work', branchName: 'feature/context', capturedAt: born, label: 'feature/context', text: 'Originally selected context' })!
beforeEach(() => {
  useAppStore.setState({ folders: [], sessions: [{ id: 'reference-send', name: 'Fixture', folderId: null, createdAt: new Date(born), updatedAt: new Date(), executionHostId: 'local', workspaceDirectory: '/work' }] })
  writeChatComposerDraft('reference-send', { text: 'Original prompt', attachments: [], references: [selected] })
})

it('freezes selected reference bytes before a goal or mode wait and clears only its unchanged submitted selection', () => {
  const submission = captureComposerReferenceSubmission('reference-send')!
  const later = { ...selected, id: 'later-reference', text: 'Added while awaiting a mode or goal operation' }
  writeChatComposerDraft('reference-send', { text: 'Newer draft', attachments: [], references: [selected, later] })
  expect(submission.references).toEqual([selected])
  expect(composerReferenceSubmissionIsCurrent(submission)).toBe(true)
  acknowledgeComposerReferences(submission)
  expect(readChatComposerDraft('reference-send')).toEqual({ text: 'Newer draft', attachments: [], references: [later] })
})

it('retains a changed same-ID snapshot when the old selection is acknowledged', () => {
  const submission = captureComposerReferenceSubmission('reference-send')!
  const changed = { ...selected, text: 'A new capture retained using the same stored ID' }
  writeChatComposerDraft('reference-send', { text: 'Newer draft', attachments: [], references: [changed] })
  expect(submission.references).toEqual([selected])
  acknowledgeComposerReferences(submission)
  expect(readChatComposerDraft('reference-send').references).toEqual([changed])
})

it('keeps the original submission after branch/base and unrelated folder metadata refresh for the same target', () => {
  const submission = captureComposerReferenceSubmission('reference-send')!
  useAppStore.setState((state) => ({ sessions: state.sessions.map((chat) => ({ ...chat,
    name: 'Renamed', workspaceBranchName: 'refreshed-branch', workspaceBaseRef: 'refreshed-base', folderId: 'other-folder',
  })), folders: [{ id: 'other-folder', name: 'Different folder', parentId: null, directory: '/unused-folder', executionHostId: 'unused-host', isExpanded: true, createdAt: new Date(born) }] }))
  expect(composerReferenceSubmissionIsCurrent(submission)).toBe(true)
  expect(submission.references).toEqual([selected])
})

it('refuses a changed folder workspace when the target actually inherits that directory', () => {
  useAppStore.setState((state) => ({ sessions: state.sessions.map((chat) => ({ ...chat, folderId: 'inherited', workspaceDirectory: undefined })),
    folders: [{ id: 'inherited', name: 'Inherited workspace', parentId: null, directory: '/original', executionHostId: 'local', isExpanded: true, createdAt: new Date(born) }] }))
  const submission = captureComposerReferenceSubmission('reference-send')!
  useAppStore.setState((state) => ({ folders: state.folders.map((folder) => ({ ...folder, directory: '/changed' })) }))
  expect(composerReferenceSubmissionIsCurrent(submission)).toBe(false)
})

it.each(['', '   '])('refuses a changed local worktree fallback with an empty configured worktree %j', (worktree) => {
  useAppStore.setState((state) => ({ sessions: state.sessions.map((chat) => ({ ...chat, workspaceIsolationKind: 'gitWorktree', workspaceWorktreeDirectory: worktree })) }))
  const submission = captureComposerReferenceSubmission('reference-send')!
  expect(submission).not.toBeNull()
  useAppStore.setState((state) => ({ sessions: state.sessions.map((chat) => ({ ...chat, workspaceDirectory: '/changed' })) }))
  expect(composerReferenceSubmissionIsCurrent(submission)).toBe(false)
})

it.each(['birth', 'host', 'workspace', 'removed'])('refuses a delayed first send after target %s changes', (change) => {
  const submission = captureComposerReferenceSubmission('reference-send')!
  useAppStore.setState((state) => ({ sessions: change === 'removed' ? [] : state.sessions.map((chat) => ({ ...chat,
    ...(change === 'birth' ? { createdAt: new Date('2026-10-23T13:00:00Z') } : {}),
    ...(change === 'host' ? { executionHostId: 'other' } : {}),
    ...(change === 'workspace' ? { workspaceDirectory: '/other' } : {}),
  })) }))
  expect(composerReferenceSubmissionIsCurrent(submission)).toBe(false)
  if (change === 'birth' || change === 'removed') {
    acknowledgeComposerReferences(submission)
    expect(readChatComposerDraft('reference-send').references).toEqual([selected])
  }
})

it('preserves a configured worktree target when its unused source directory refreshes', () => {
  useAppStore.setState((state) => ({ sessions: state.sessions.map((chat) => ({ ...chat, workspaceIsolationKind: 'gitWorktree', workspaceWorktreeDirectory: '/selected-worktree' })) }))
  const submission = captureComposerReferenceSubmission('reference-send')!
  useAppStore.setState((state) => ({ sessions: state.sessions.map((chat) => ({ ...chat, workspaceDirectory: '/refreshed-unused-source' })) }))
  expect(composerReferenceSubmissionIsCurrent(submission)).toBe(true)
})

it('refuses an incomplete remote worktree without inventing a local fallback', () => {
  useAppStore.setState((state) => ({ sessions: state.sessions.map((chat) => ({ ...chat, executionHostId: 'remote', workspaceIsolationKind: 'gitWorktree', workspaceWorktreeDirectory: '' })) }))
  expect(captureComposerReferenceSubmission('reference-send')).toBeNull()
})
