import { attachmentsEquivalent } from '../store/cpp/reconcile'
import type { ContextReference } from '../types/contextReference'
import { sanitizeContextReferences } from './contextReferences'
import type { Attachment } from '../types/message'
import { sanitizeAttachment } from '../store/cpp/sanitizers'

export interface ChatComposerDraft {
  text: string
  attachments: Attachment[]
  references?: ContextReference[]
}

const chatPrefix = 'uam-chat-composer-draft-v1:'
const terminalPrefix = 'uam-terminal-steer-draft-v1:'
const chatDraftMemory = new Map<string, ChatComposerDraft>()
const chatDraftDirty = new Set<string>()
const draftListeners = new Set<(sessionId: string) => void>()

export function subscribeChatComposerDraft(listener: (sessionId: string) => void): () => void {
  draftListeners.add(listener)
  return () => { draftListeners.delete(listener) }
}

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
  const sanitized = sanitizeAttachment(item)
  if (sanitized && item.path === undefined) delete sanitized.path
  return sanitized
}

export function readChatComposerDraft(sessionId: string): ChatComposerDraft {
	if (chatDraftDirty.has(sessionId)) return chatDraftMemory.get(sessionId) ?? { text: '', attachments: [] }
  try {
    const stored = storage()?.getItem(`${chatPrefix}${sessionId}`)
    if (stored === null || stored === undefined) return chatDraftMemory.get(sessionId) ?? { text: '', attachments: [] }
    const parsed = JSON.parse(stored) as { text?: unknown; attachments?: unknown; references?: unknown }
    const references = sanitizeContextReferences(parsed.references)
    const draft = {
      ...(references.length > 0 ? { references } : {}),
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
  const references = sanitizeContextReferences(draft.references ?? readChatComposerDraft(sessionId).references)
  const snapshot = { text: draft.text, attachments: [...draft.attachments], ...(references.length > 0 ? { references } : {}) }
  if (!draft.text && draft.attachments.length === 0 && references.length === 0) chatDraftMemory.delete(sessionId)
  else chatDraftMemory.set(sessionId, snapshot)
  try {
    const key = `${chatPrefix}${sessionId}`
    const target = storage()
    if (!target) throw new Error('Draft persistence is unavailable.')
    if (!draft.text && draft.attachments.length === 0 && references.length === 0) target.removeItem(key)
    else target.setItem(key, JSON.stringify(snapshot))
    chatDraftDirty.delete(sessionId)
  } catch {
    chatDraftDirty.add(sessionId)
  }
  for (const listener of draftListeners) {
    try { listener(sessionId) } catch { /* Presentation cannot change the persistence outcome. */ }
  }
}

export function removeComposerDrafts(sessionIds: Iterable<string>): void {
  const ids = Array.from(sessionIds)
  for (const sessionId of ids) {
    chatDraftMemory.delete(sessionId)
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
    } catch {
      // Legacy terminal drafts are no longer read; cleanup is best effort.
    }
  }
}


// Successful admission owns the submitted snapshot even if its composer has remounted.
const acknowledgementListeners = new Set<(sessionId: string, createdAt: number, submitted: ChatComposerDraft) => void>()

export function withoutAcknowledgedComposerDraft(current: ChatComposerDraft, submitted: ChatComposerDraft): ChatComposerDraft {
  return { ...current, text: current.text === submitted.text ? '' : current.text,
    attachments: current.attachments.filter((attachment) => !submitted.attachments.some((sent) =>
      attachmentsEquivalent([attachment], [sent]))) }
}

export function acknowledgeChatComposerDraft(sessionId: string, createdAt: number, submitted: ChatComposerDraft): void {
  writeChatComposerDraft(sessionId, withoutAcknowledgedComposerDraft(readChatComposerDraft(sessionId), submitted))
  for (const listener of acknowledgementListeners) listener(sessionId, createdAt, submitted)
}

export function subscribeComposerDraftAcknowledgements(listener: (sessionId: string, createdAt: number, submitted: ChatComposerDraft) => void): () => void {
  acknowledgementListeners.add(listener)
  return () => { acknowledgementListeners.delete(listener) }
}
