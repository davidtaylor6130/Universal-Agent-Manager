import type { ContextReference } from '../types/contextReference'
export const CONTEXT_REFERENCE_MAX_BYTES = 16 * 1024
export const CONTEXT_REFERENCE_MAX_COUNT = 4

/** Captures explicit selected text; never reads an entire transcript or terminal buffer. */
export function captureContextReference(input: Omit<ContextReference, 'id' | 'truncated'> & { id?: string }): ContextReference | null {
  if ((input.kind !== 'message' && input.kind !== 'terminal') || !input.sourceChatId || input.sourceChatId.length > 128 ||
    !input.executionHostId || input.executionHostId.length > 256 || input.workspaceDirectory.length > 4096 || input.label.length > 160 ||
    input.capturedAt.length > 64 || !Number.isFinite(Date.parse(input.capturedAt)) || !input.text.trim()) return null
  if (input.kind === 'message' && (!Number.isSafeInteger(input.sourceMessageIndex) || Number(input.sourceMessageIndex) < 0)) return null
  if (input.kind === 'terminal' && (!input.terminalId || input.terminalId.length > 256)) return null
  if (['sourceMessageId', 'nativeSessionId', 'terminalId', 'id'].some((key) => { const value = input[key as keyof typeof input]; return value !== undefined && (typeof value !== 'string' || value.length > (key === 'id' ? 128 : 256)) })) return null
  let text = ''
  let bytes = 0
  const encoder = new TextEncoder()
  for (const character of input.text) {
    const size = encoder.encode(character).length
    if (bytes + size > CONTEXT_REFERENCE_MAX_BYTES) break
    text += character
    bytes += size
  }
  return {
    id: input.id || crypto.randomUUID(), kind: input.kind, sourceChatId: input.sourceChatId,
    ...(input.kind === 'message' ? { sourceMessageIndex: input.sourceMessageIndex, ...(input.sourceMessageId ? { sourceMessageId: input.sourceMessageId } : {}) } : { terminalId: input.terminalId }),
    ...(input.nativeSessionId ? { nativeSessionId: input.nativeSessionId } : {}),
    executionHostId: input.executionHostId, workspaceDirectory: input.workspaceDirectory,
    capturedAt: input.capturedAt, label: input.label, text, truncated: text.length < input.text.length,
  }
}

/** Revalidates persisted snapshots before they enter a provider prompt. */
export function sanitizeContextReferences(value: unknown): ContextReference[] {
  if (!Array.isArray(value)) return []
  const result: ContextReference[] = []
  for (const raw of value.slice(0, CONTEXT_REFERENCE_MAX_COUNT)) {
    if (!raw || typeof raw !== 'object') continue
    const item = raw as Partial<ContextReference>
    if (typeof item.sourceChatId !== 'string' || typeof item.executionHostId !== 'string' || typeof item.workspaceDirectory !== 'string' ||
      typeof item.capturedAt !== 'string' || typeof item.label !== 'string' || typeof item.text !== 'string' || typeof item.id !== 'string') continue
    const snapshot = captureContextReference(item as ContextReference)
    if (snapshot && !result.some((entry) => entry.id === snapshot.id)) result.push({ ...snapshot, truncated: snapshot.truncated || item.truncated === true })
  }
  return result
}

/** Sends bounded snapshots as explicitly attributed quoted data rather than silently rewriting the draft. */
export function formatContextReferences(references: readonly ContextReference[]): string {
  return sanitizeContextReferences(references).map(({ text, ...source }) => `Referenced context (quoted snapshot):\n${JSON.stringify({ ...source, text })}`).join('\n\n')
}
