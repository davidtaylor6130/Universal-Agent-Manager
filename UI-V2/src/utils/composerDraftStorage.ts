import type { Attachment } from '../types/message'

export interface ChatComposerDraft {
  text: string
  attachments: Attachment[]
}

const chatPrefix = 'uam-chat-composer-draft-v1:'
const terminalPrefix = 'uam-terminal-steer-draft-v1:'

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
  try {
    const parsed = JSON.parse(storage()?.getItem(`${chatPrefix}${sessionId}`) ?? '{}') as { text?: unknown; attachments?: unknown }
    return {
      text: typeof parsed.text === 'string' ? parsed.text : '',
      attachments: Array.isArray(parsed.attachments) ? parsed.attachments.map(attachment).filter((item): item is Attachment => item !== null) : [],
    }
  } catch {
    return { text: '', attachments: [] }
  }
}

export function writeChatComposerDraft(sessionId: string, draft: ChatComposerDraft): void {
  try {
    const key = `${chatPrefix}${sessionId}`
    if (!draft.text && draft.attachments.length === 0) storage()?.removeItem(key)
    else storage()?.setItem(key, JSON.stringify(draft))
  } catch {
    // The in-memory draft remains usable when persistence is unavailable.
  }
}

export function readTerminalSteerDraft(sessionId: string): string {
  try {
    return storage()?.getItem(`${terminalPrefix}${sessionId}`) ?? ''
  } catch {
    return ''
  }
}

export function writeTerminalSteerDraft(sessionId: string, text: string): void {
  try {
    const key = `${terminalPrefix}${sessionId}`
    if (text) storage()?.setItem(key, text)
    else storage()?.removeItem(key)
  } catch {
    // The in-memory draft remains usable when persistence is unavailable.
  }
}

export function removeComposerDrafts(sessionIds: Iterable<string>): void {
  const target = storage()
  if (!target) return
  try {
    for (const sessionId of sessionIds) {
      target.removeItem(`${chatPrefix}${sessionId}`)
      target.removeItem(`${terminalPrefix}${sessionId}`)
    }
  } catch {
    // Best effort cleanup after durable chat deletion.
  }
}
