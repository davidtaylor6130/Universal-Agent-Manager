// Module-level buffers shared between the store actions and the uamPush handler.
// Encapsulated here (FE-4) so the shared mutable state has one home and tests can
// reset it deterministically via resetPushBuffersForTests().

export const pendingRequestIdsByKey = new Map<string, string>()
export const pendingCodexOptionsByChatId = new Map<string, { requestId: string; reasoningEffort: string; serviceTier: string }>()
export const pendingModelByChatId = new Map<string, { requestId: string; modelId: string; reasoningEffort: string; serviceTier: string }>()
export const pendingCliTranscriptChunksBySessionId = new Map<string, { terminalId: string; chunks: string[] }>()
export const CLI_TRANSCRIPT_FLUSH_DELAY_MS = 32

// Stream tokens can arrive far faster than the UI needs to repaint; applying
// each one individually re-parses the growing assistant message's markdown per
// token. Coalescing keeps streaming smooth on long answers.
export const pendingStreamTokensByChatId = new Map<string, string>()
export const STREAM_TOKEN_FLUSH_DELAY_MS = 48

// Flush timer handles. Held on a mutable object so callers can reassign across
// modules (an imported binding cannot be reassigned directly).
export const pushFlushTimers: { cliTranscript: number | null; streamToken: number | null } = {
  cliTranscript: null,
  streamToken: null,
}

export function resetPushBuffersForTests(): void {
  pendingRequestIdsByKey.clear()
  pendingCodexOptionsByChatId.clear()
  pendingModelByChatId.clear()
  pendingCliTranscriptChunksBySessionId.clear()
  pendingStreamTokensByChatId.clear()
  if (pushFlushTimers.cliTranscript !== null && typeof window !== 'undefined') {
    window.clearTimeout(pushFlushTimers.cliTranscript)
  }
  if (pushFlushTimers.streamToken !== null && typeof window !== 'undefined') {
    window.clearTimeout(pushFlushTimers.streamToken)
  }
  pushFlushTimers.cliTranscript = null
  pushFlushTimers.streamToken = null
}
