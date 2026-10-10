import { describe, expect, it } from 'vitest'
import { nextAttentionSession } from './nextAttention'
import type { Session } from '../types/session'
const session = (id: string, timestamp = 0): Session => ({ id, name: id, folderId: null, viewMode: 'chat', createdAt: new Date(timestamp), updatedAt: new Date(timestamp) })
describe('next attention chat', () => {
  it('follows visible order and wraps once without choosing the current chat', () => {
    const chats = [session('a'), session('b'), session('c')]
    const bindings = { a: { attentionKind: 'question' as const }, b: { attentionKind: 'permission' as const }, c: { attentionKind: 'file' as const } }
    expect(nextAttentionSession(chats, bindings, 'a', ['a', 'c', 'b'])).toBe('c')
    expect(nextAttentionSession(chats, bindings, 'b', ['a', 'c', 'b'])).toBe('a')
    expect(nextAttentionSession([session('a')], bindings, 'a')).toBeNull()
  })
  it('includes input and failures but excludes generic busy and completed chats', () => {
    const chats = ['busy', 'done', 'failed', 'question'].map((id) => session(id))
    expect(nextAttentionSession(chats, { busy: { processing: true }, done: { readySinceLastSelect: true }, failed: { lifecycleState: 'error' } }, null, ['busy', 'done', 'failed'])).toBe('failed')
    expect(nextAttentionSession(chats, { question: { attentionKind: 'question' } }, null)).toBe('question')
    expect(nextAttentionSession(chats, { busy: { processing: true } }, null)).toBeNull()
  })
  it('ignores side chats and removed rows; falls back to recent order for collapsed sections', () => {
    const chats = [session('older', 1), session('newer', 2), { ...session('side', 3), temporaryParentChatId: 'older' }]
    const bindings = { older: { attentionKind: 'permission' as const }, newer: { attentionKind: 'question' as const }, side: { attentionKind: 'question' as const } }
    expect(nextAttentionSession(chats, bindings, null, ['removed', 'side'])).toBe('newer')
    expect(nextAttentionSession(chats, bindings, 'newer', ['newer', 'newer'])).toBe('older')
  })
})
