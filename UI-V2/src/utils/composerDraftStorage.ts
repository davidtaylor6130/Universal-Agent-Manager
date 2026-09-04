import type { Attachment } from '../types/message'

export interface ChatComposerDraft {
  text: string
  attachments: Attachment[]
}

const chatPrefix = 'uam-chat-composer-draft-v1:'
const terminalPrefix = 'uam-terminal-steer-draft-v1:'
const chatDraftMemory = new Map<string, ChatComposerDraft>()
const terminalDraftMemory = new Map<string, string>()
const chatDraftDirty = new Set<string>()
const terminalDraftDirty = new Set<string>()

function storage(): Storage | null {
  try {
    return globalThis.localStorage ?? null
  } catch {
    return null
  }
}

function attachment(value: unknown): Attachment | null {
  if (!value || typeof value !== 'object') return null
  const item = value as Partial<Attachment>
  if (typeof item.id !== 'string' || typeof item.name !== 'string' || typeof item.type !== 'string' || typeof item.size !== 'number') return null
  return { id: item.id, name: item.name, type: item.type, size: item.size, ...(typeof item.path === 'string' ? { path: item.path } : {}) }
}

export function readChatComposerDraft(sessionId: string): ChatComposerDraft {
	if (chatDraftDirty.has(sessionId)) return chatDraftMemory.get(sessionId) ?? { text: '', attachments: [] }
  try {
    const stored = storage()?.getItem(`${chatPrefix}${sessionId}`)
    if (stored === null || stored === undefined) return chatDraftMemory.get(sessionId) ?? { text: '', attachments: [] }
    const parsed = JSON.parse(stored) as { text?: unknown; attachments?: unknown }
    const draft = {
      text: typeof parsed.text === 'string' ? parsed.text : '',
      attachments: Array.isArray(parsed.attachments) ? parsed.attachments.map(attachment).filter((item): item is Attachment => item !== null) : [],
    }
    chatDraftMemory.set(sessionId, draft)
    return draft
  } catch {
    return chatDraftMemory.get(sessionId) ?? { text: '', attachments: [] }
  }
}

export function writeChatComposerDraft(sessionId: string, draft: ChatComposerDraft): void {
  const snapshot = { text: draft.text, attachments: [...draft.attachments] }
  if (!draft.text && draft.attachments.length === 0) chatDraftMemory.delete(sessionId)
  else chatDraftMemory.set(sessionId, snapshot)
  try {
    const key = `${chatPrefix}${sessionId}`
    const target = storage()
    if (!target) throw new Error('Draft persistence is unavailable.')
    if (!draft.text && draft.attachments.length === 0) target.removeItem(key)
    else target.setItem(key, JSON.stringify(snapshot))
    chatDraftDirty.delete(sessionId)
  } catch {
    chatDraftDirty.add(sessionId)
  }
}

export function readTerminalSteerDraft(sessionId: string): string {
	if (terminalDraftDirty.has(sessionId)) return terminalDraftMemory.get(sessionId) ?? ''
  try {
    return storage()?.getItem(`${terminalPrefix}${sessionId}`) ?? terminalDraftMemory.get(sessionId) ?? ''
  } catch {
    return terminalDraftMemory.get(sessionId) ?? ''
  }
}

export function writeTerminalSteerDraft(sessionId: string, text: string): void {
  if (text) terminalDraftMemory.set(sessionId, text)
  else terminalDraftMemory.delete(sessionId)
  try {
    const key = `${terminalPrefix}${sessionId}`
    const target = storage()
    if (!target) throw new Error('Draft persistence is unavailable.')
    if (text) target.setItem(key, text)
    else target.removeItem(key)
    terminalDraftDirty.delete(sessionId)
  } catch {
    terminalDraftDirty.add(sessionId)
  }
}

export function removeComposerDrafts(sessionIds: Iterable<string>): void {
  const ids = Array.from(sessionIds)
  for (const sessionId of ids) {
    chatDraftMemory.delete(sessionId)
    terminalDraftMemory.delete(sessionId)
  }
  const target = storage()
  for (const sessionId of ids) {
    try {
      if (!target) throw new Error('Draft persistence is unavailable.')
      target.removeItem(`${chatPrefix}${sessionId}`)
      chatDraftDirty.delete(sessionId)
    } catch {
      chatDraftDirty.add(sessionId)
    }
    try {
      if (!target) throw new Error('Draft persistence is unavailable.')
      target.removeItem(`${terminalPrefix}${sessionId}`)
      terminalDraftDirty.delete(sessionId)
    } catch {
      terminalDraftDirty.add(sessionId)
    }
  }
}
