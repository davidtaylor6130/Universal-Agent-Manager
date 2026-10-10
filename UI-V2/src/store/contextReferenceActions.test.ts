import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from './useAppStore'
import { captureContextReference } from '../utils/contextReferences'
import { readChatComposerDraft, writeChatComposerDraft } from '../utils/composerDraftStorage'
const source = { sourceChatId: 'source', executionHostId: 'local', workspaceDirectory: '/isolated', capturedAt: '2026-10-07T12:00:00.000Z', label: 'Selected context', text: 'Selected text' }
beforeEach(() => {
  const values = new Map<string, string>()
  vi.stubGlobal('localStorage', { getItem: (key: string) => values.get(key) ?? null, setItem: (key: string, value: string) => values.set(key, value), removeItem: (key: string) => values.delete(key) })
  useAppStore.setState({ sessions: ['source', 'target'].map((id) => ({ id, name: id, folderId: null, viewMode: 'chat', executionHostId: 'local', workspaceDirectory: '/base', workspaceWorktreeDirectory: '/isolated', createdAt: new Date(), updatedAt: new Date() })),
    messages: { source: [{ id: 'saved', sessionId: 'source', role: 'assistant', content: 'Selected text from a saved message', createdAt: new Date() }] },
    historyStartIndexBySessionId: { source: 4 }, cliBindingBySessionId: { source: { terminalId: 'term' } } })
  writeChatComposerDraft('target', { text: 'Unsent draft', references: [], attachments: [{ id: 'attachment', name: 'file.txt', type: 'file', size: 1 }] })
})
describe('staged context provenance', () => {
  it('stages a loaded saved message using its absolute index without changing the composer draft', () => {
    const reference = captureContextReference({ ...source, kind: 'message', sourceMessageIndex: 4, sourceMessageId: 'saved' })!
    expect(useAppStore.getState().stageChatContextReference('target', reference)).toBe(true)
    expect(readChatComposerDraft('target')).toMatchObject({ text: 'Unsent draft', attachments: [{ id: 'attachment' }], references: [reference] })
  })
  it('rejects stale terminal instances and mismatched hosts or workspaces', () => {
    const reference = captureContextReference({ ...source, kind: 'terminal', terminalId: 'term' })!
    useAppStore.setState({ cliBindingBySessionId: { source: { terminalId: 'restarted' } } })
    expect(useAppStore.getState().stageChatContextReference('target', reference)).toBe(false)
    expect(useAppStore.getState().stageChatContextReference('target', { ...reference, terminalId: 'restarted', executionHostId: 'remote' })).toBe(false)
    expect(useAppStore.getState().stageChatContextReference('target', { ...reference, terminalId: 'restarted', workspaceDirectory: '/base' })).toBe(false)
    expect(readChatComposerDraft('target').references).toBeUndefined()
  })
  it('rejects missing or altered saved messages instead of reading extra history', () => {
    const reference = captureContextReference({ ...source, kind: 'message', sourceMessageIndex: 3 })!
    expect(useAppStore.getState().stageChatContextReference('target', reference)).toBe(false)
    expect(useAppStore.getState().stageChatContextReference('target', { ...reference, sourceMessageIndex: 4, text: 'Invented source text' })).toBe(false)
    expect(readChatComposerDraft('target').text).toBe('Unsent draft')
  })
})
