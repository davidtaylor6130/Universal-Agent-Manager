import { describe, expect, it, vi } from 'vitest'
import type { Session } from '../types/session'
import { rememberSubagentParent, subagentParentFor } from './subagentNavigation'
const session = (id: string): Session => ({ id, name: id, folderId: null, viewMode: 'chat', providerId: 'codex-cli', executionHostId: 'local', createdAt: new Date(), updatedAt: new Date() })
describe('subagent navigation receipts', () => {
  it('returns only the existing parent on the original provider and host', () => {
    const parent = session('navigation-parent'); const child = session('navigation-child')
    rememberSubagentParent(parent, child.id)
    expect(subagentParentFor(child, [parent, child])).toBe(parent)
    expect(subagentParentFor(child, [child])).toBeUndefined()
    expect(subagentParentFor({ ...child, executionHostId: 'other-host' }, [parent, child])).toBeUndefined()
    expect(subagentParentFor(child, [{ ...parent, providerId: 'claude-cli' }, child])).toBeUndefined()
  })
  it('never mistakes ordinary branch parent metadata or the source itself for a subagent link', () => {
    const parent = session('navigation-self'); const child = { ...session('navigation-branch'), parentChatId: parent.id }
    rememberSubagentParent(parent, parent.id)
    expect(subagentParentFor(parent, [parent])).toBeUndefined()
    expect(subagentParentFor(child, [parent, child])).toBeUndefined()
  })
})

it('persists navigation context and notifies an already mounted child without creating authority', async () => {
  const savedValues = new Map<string, string>()
  vi.stubGlobal('localStorage', { getItem: (key: string) => savedValues.get(key) || null, setItem: (key: string, value: string) => savedValues.set(key, value) })
  const { subscribeSubagentNavigation } = await import('./subagentNavigation')
  let changes = 0
  const unsubscribe = subscribeSubagentNavigation(() => changes++)
  const parent = session('persisted-navigation-parent'); const child = session('persisted-navigation-child')
  try {
    rememberSubagentParent(parent, child.id)
    expect(changes).toBe(1)
    const saved = JSON.parse(localStorage.getItem('uam-subagent-navigation-v1') || '[]')
    expect(saved.some((entry: [string, unknown]) => entry[0] === child.id)).toBe(true)
    expect(subagentParentFor(child, [{ ...parent, createdAt: new Date(parent.createdAt.getTime() + 1) }, child])).toBeUndefined()
  } finally { unsubscribe(); vi.unstubAllGlobals() }
})

it('scopes a virtual managed route to its parent identity and pane without serializing a hidden Session', async () => {
  const { openSubagentView, subagentRouteFor, clearSubagentView } = await import('./subagentNavigation')
  const { defaultChatGridLayout, chatGridLeaves, readChatGridLayout, setChatInLeaf, splitChatLeaf, writeChatGridLayout } = await import('./chatGridStorage')
  const values = new Map<string, string>()
  vi.stubGlobal('localStorage', { getItem: (key: string) => values.get(key) ?? null, setItem: (key: string, value: string) => values.set(key, value) })
  const parent = session('scoped-virtual-parent')
  const other = session('scoped-other-pane')
  let layout = splitChatLeaf(defaultChatGridLayout, defaultChatGridLayout.activeLeafId, 'horizontal')
  const leaves = chatGridLeaves(layout.root)
  layout = setChatInLeaf(setChatInLeaf(layout, parent.id, leaves[0].id), other.id, leaves[1].id)
  writeChatGridLayout(layout)
  try {
    openSubagentView(parent, { kind: 'managed', runId: 'exact-run', transcriptChatId: 'hidden-chat' })
    expect(readChatGridLayout().activeLeafId).toBe(leaves[0].id)
    expect(chatGridLeaves(readChatGridLayout().root).map(leaf => leaf.sessionId)).toEqual([parent.id, other.id])
    expect([...values.values()].join('')).not.toContain('hidden-chat')
    expect(subagentRouteFor(leaves[0].id, parent)).toMatchObject({ kind: 'managed', runId: 'exact-run' })
    expect(subagentRouteFor(leaves[1].id, parent)).toBeUndefined()
    expect(subagentRouteFor(leaves[0].id, { ...parent, createdAt: new Date(parent.createdAt.getTime() + 1) })).toBeUndefined()
    expect(subagentRouteFor(leaves[0].id, { ...parent, executionHostId: 'other-host' })).toBeUndefined()
    expect(subagentRouteFor(leaves[0].id, { ...parent, providerId: 'claude-cli' })).toBeUndefined()
    expect(subagentRouteFor(leaves[0].id, { ...parent, workspaceDirectory: '/other-workspace' })).toBeUndefined()
  } finally { clearSubagentView(leaves[0].id); vi.unstubAllGlobals() }
})

it('uses explicit backend lineage without a browser receipt and after the legacy cache is evicted', async () => {
  const { nativeSubagentChildren } = await import('./subagentNavigation')
  const parent = { ...session('backend-only-parent'), workspaceDirectory: '/workspace/project' }
  const child = { ...session('backend-only-child'), workspaceDirectory: '/workspace/project', subagentParentChatId: parent.id, subagentParentCreatedAt: parent.createdAt.toISOString() }
  expect(subagentParentFor(child, [parent, child])).toBe(parent)
  expect(nativeSubagentChildren(parent.id, [parent, child])).toEqual([child])
  for (let index = 0; index < 260; ++index) rememberSubagentParent(session(`eviction-parent-${index}`), `eviction-child-${index}`)
  // Reconstituted metadata proves navigation does not rely on a cached open receipt.
  const restoredParent = { ...parent, createdAt: new Date(parent.createdAt.toISOString()) }
  const restoredChild = { ...child, createdAt: new Date(child.createdAt.toISOString()) }
  expect(subagentParentFor(restoredChild, [restoredParent, restoredChild])).toBe(restoredParent)
  expect(nativeSubagentChildren(restoredParent.id, [restoredParent, restoredChild])).toEqual([restoredChild])
})

it('prefers explicit lineage over conflicting legacy visits and rejects foreign or recreated parent scope', async () => {
  const { hasNativeSubagentLineage, nativeSubagentChildren } = await import('./subagentNavigation')
  const parent = { ...session('strict-backend-parent'), workspaceDirectory: '/workspace/project' }
  const legacy = { ...parent, id: 'strict-legacy-parent' }
  const child = { ...session('strict-backend-child'), workspaceDirectory: parent.workspaceDirectory, subagentParentChatId: parent.id, subagentParentCreatedAt: parent.createdAt.toISOString() }
  rememberSubagentParent(legacy, child.id)
  expect(subagentParentFor(child, [legacy, parent, child])).toBe(parent)
  for (const mismatch of [
    { ...parent, createdAt: new Date(parent.createdAt.getTime() + 1) },
    { ...parent, providerId: 'claude-cli' },
    { ...parent, executionHostId: 'foreign-host' },
    { ...parent, workspaceDirectory: '/foreign-workspace' },
  ]) {
    expect(subagentParentFor(child, [legacy, mismatch, child])).toBeUndefined()
    expect(nativeSubagentChildren(parent.id, [legacy, mismatch, child])).toEqual([])
    expect(hasNativeSubagentLineage(child)).toBe(true)
  }
  expect(subagentParentFor(child, [legacy, child])).toBeUndefined()
  expect(subagentParentFor({ ...child, subagentParentCreatedAt: 'invalid' }, [legacy, parent, child])).toBeUndefined()
  expect(subagentParentFor({ ...child, subagentParentCreatedAt: '' }, [legacy, parent, child])).toBeUndefined()
  // Parent Session dates and lineage timestamps use the same Date parser for legacy local-formatted records.
  const localParent = { ...parent, createdAt: new Date('2026-10-16 10:00:00') }
  expect(subagentParentFor({ ...child, subagentParentCreatedAt: '2026-10-16 10:00:00' }, [localParent, child])).toBe(localParent)
})

// #675: the sidebar hides a linked child and renders it under its parent through one predicate.
const worktreeChild = (parent: Session, id: string): Session => ({
  ...session(id), workspaceDirectory: '/repo/.uam/worktrees/child', workspaceWorktreeDirectory: '/repo/.uam/worktrees/child',
  workspaceSourceDirectory: '/repo', workspaceIsolationKind: 'git-worktree', subagentParentChatId: parent.id, subagentParentCreatedAt: parent.createdAt.toISOString(),
})

it('groups a worktree child under its main-checkout parent', async () => {
  const { nativeSubagentChildren } = await import('./subagentNavigation')
  const parent = { ...session('main-checkout-parent'), workspaceDirectory: '/repo/' }
  const child = worktreeChild(parent, 'worktree-child')
  expect(subagentParentFor(child, [parent, child])).toBe(parent)
  expect(nativeSubagentChildren(parent.id, [parent, child])).toEqual([child])
})

it('keeps the dropdown after a child is opened directly from All Chats', async () => {
  const { nativeSubagentChildren } = await import('./subagentNavigation')
  const parent = { ...session('all-chats-parent'), workspaceDirectory: '/repo' }
  const listed = { ...worktreeChild(parent, 'all-chats-child'), workspaceDirectory: '/repo', workspaceWorktreeDirectory: '', workspaceIsolationKind: '' }
  expect(nativeSubagentChildren(parent.id, [parent, listed])).toEqual([listed])
  // After opening, the refreshed child reports its own worktree of the parent's checkout.
  const opened = worktreeChild(parent, listed.id)
  expect(subagentParentFor(opened, [parent, opened])).toBe(parent)
  expect(nativeSubagentChildren(parent.id, [parent, opened])).toEqual([opened])
})

it('keeps remote worktree children exact, matching the native open rule', async () => {
  const { nativeSubagentChildren } = await import('./subagentNavigation')
  const parent = { ...session('remote-main-parent'), executionHostId: 'ssh-host', workspaceDirectory: '/repo' }
  const child = { ...worktreeChild(parent, 'remote-worktree-child'), executionHostId: 'ssh-host' }
  expect(subagentParentFor(child, [parent, child])).toBeUndefined()
  expect(nativeSubagentChildren(parent.id, [parent, child])).toEqual([])
  const exact = { ...child, id: 'remote-exact-child', workspaceDirectory: '/repo', workspaceWorktreeDirectory: '' }
  expect(subagentParentFor(exact, [parent, exact])).toBe(parent)
})

it('keeps a browser-only open receipt for a worktree child without backend lineage', async () => {
  const { nativeSubagentChildren } = await import('./subagentNavigation')
  const parent = { ...session('receipt-only-parent'), workspaceDirectory: '/repo' }
  const child = { ...worktreeChild(parent, 'receipt-only-child'), subagentParentChatId: undefined, subagentParentCreatedAt: undefined }
  rememberSubagentParent(parent, child.id)
  expect(subagentParentFor(child, [parent, child])).toBe(parent)
  expect(nativeSubagentChildren(parent.id, [parent, child])).toEqual([child])
})

it('groups a child in a sibling worktree of the same project only under its exact parent', async () => {
  const { nativeSubagentChildren } = await import('./subagentNavigation')
  const parent = { ...worktreeChild(session('sibling-root'), 'sibling-parent'), workspaceDirectory: '/repo/.uam/worktrees/parent', workspaceWorktreeDirectory: '/repo/.uam/worktrees/parent', subagentParentChatId: undefined, subagentParentCreatedAt: undefined }
  const child = worktreeChild(parent, 'sibling-child')
  const stranger = { ...parent, id: 'sibling-stranger' }
  expect(subagentParentFor(child, [parent, stranger, child])).toBe(parent)
  expect(nativeSubagentChildren(parent.id, [parent, stranger, child])).toEqual([child])
  expect(nativeSubagentChildren(stranger.id, [parent, stranger, child])).toEqual([])
})

it('never groups an unrelated chat or another provider, host or project', async () => {
  const { hasNativeSubagentLineage, nativeSubagentChildren } = await import('./subagentNavigation')
  const parent = { ...session('strict-scope-parent'), workspaceDirectory: '/repo' }
  const unrelated = { ...session('strict-scope-unrelated'), workspaceDirectory: '/repo' }
  expect(nativeSubagentChildren(parent.id, [parent, unrelated])).toEqual([])
  expect(hasNativeSubagentLineage(unrelated)).toBe(false)
  const child = worktreeChild(parent, 'strict-scope-child')
  for (const mismatch of [
    { ...child, providerId: 'claude-cli' },
    { ...child, executionHostId: 'foreign-host' },
    { ...child, workspaceDirectory: '/other/.uam/worktrees/child', workspaceWorktreeDirectory: '/other/.uam/worktrees/child', workspaceSourceDirectory: '/other' },
    { ...child, workspaceDirectory: '/repo-two', workspaceWorktreeDirectory: '', workspaceSourceDirectory: '' },
  ]) {
    expect(subagentParentFor(mismatch, [parent, mismatch])).toBeUndefined()
    expect(nativeSubagentChildren(parent.id, [parent, mismatch])).toEqual([])
  }
})
