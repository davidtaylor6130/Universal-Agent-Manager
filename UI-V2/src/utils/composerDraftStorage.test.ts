import { beforeEach, describe, expect, it, vi } from 'vitest'
import {
  readChatComposerDraft,
  readTerminalSteerDraft,
  removeComposerDrafts,
  writeChatComposerDraft,
  writeTerminalSteerDraft,
} from './composerDraftStorage'

describe('composer draft storage fallback', () => {
  beforeEach(() => vi.unstubAllGlobals())

  it('keeps and removes drafts in memory when localStorage is unavailable', () => {
    const unavailable = { getItem: () => { throw new Error('blocked') }, setItem: () => { throw new Error('blocked') }, removeItem: () => { throw new Error('blocked') } }
    vi.stubGlobal('localStorage', unavailable)
    const sessionId = `blocked-${Math.random()}`
    const attachment = { id: 'file-1', name: 'file.txt', type: 'file', size: 4 }

    writeChatComposerDraft(sessionId, { text: 'draft', attachments: [attachment] })
    writeTerminalSteerDraft(sessionId, 'steer')
    expect(readChatComposerDraft(sessionId)).toEqual({ text: 'draft', attachments: [attachment] })
    expect(readTerminalSteerDraft(sessionId)).toBe('steer')

    removeComposerDrafts([sessionId])
    expect(readChatComposerDraft(sessionId)).toEqual({ text: '', attachments: [] })
    expect(readTerminalSteerDraft(sessionId)).toBe('')
  })

  it('keeps failed writes and clears authoritative over readable stale storage', () => {
    const values = new Map<string, string>()
    const sessionId = `quota-${Math.random()}`
    values.set(`uam-chat-composer-draft-v1:${sessionId}`, JSON.stringify({ text: 'old', attachments: [] }))
    values.set(`uam-terminal-steer-draft-v1:${sessionId}`, 'old steer')
    vi.stubGlobal('localStorage', {
      getItem: (key: string) => values.get(key) ?? null,
      setItem: () => { throw new Error('quota') },
      removeItem: () => { throw new Error('read only') },
    })

    writeChatComposerDraft(sessionId, { text: 'new', attachments: [] })
    writeTerminalSteerDraft(sessionId, 'new steer')
    expect(readChatComposerDraft(sessionId).text).toBe('new')
    expect(readTerminalSteerDraft(sessionId)).toBe('new steer')

    writeChatComposerDraft(sessionId, { text: '', attachments: [] })
    writeTerminalSteerDraft(sessionId, '')
    expect(readChatComposerDraft(sessionId)).toEqual({ text: '', attachments: [] })
    expect(readTerminalSteerDraft(sessionId)).toBe('')
  })
})
