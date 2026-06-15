// Push message parsing and revision helpers. Extracted from useAppStore.ts (MO-1).
// parseUamPushPayload is the sole entry point for all C++ → frontend pushes.

import { isRecord, isString, sanitizeCppAppState, sanitizeCppStatePatch } from '../cpp/sanitizers'
import type { CppCliDebugState, ParsedPushResult } from '../cpp/types'

export function parseUamPushPayload(payload: unknown): ParsedPushResult {
  let raw: unknown = payload

  if (typeof raw === 'string') {
    try {
      raw = JSON.parse(raw)
    } catch (err) {
      const reason = err instanceof Error ? err.message : 'unknown parse failure'
      return { ok: false, status: 'parse-error', error: `JSON parse failed: ${reason}` }
    }
  }

  if (!isRecord(raw)) {
    return { ok: false, status: 'invalid-message', error: 'Payload is not an object.' }
  }

  const type = raw.type
  if (!isString(type) || type.trim().length === 0) {
    return { ok: false, status: 'invalid-message', error: 'Missing message type.' }
  }

  if (type === 'stateUpdate') {
    const sanitized = sanitizeCppAppState(raw.data)
    if (!sanitized) {
      return { ok: false, status: 'invalid-message', error: 'stateUpdate.data does not match CppAppState shape.' }
    }
    return { ok: true, message: { type, data: sanitized } }
  }

  if (type === 'statePatch') {
    const sanitized = sanitizeCppStatePatch(raw.data)
    if (!sanitized) {
      return { ok: false, status: 'invalid-message', error: 'statePatch.data does not match CppStatePatch shape.' }
    }
    return { ok: true, message: { type, data: sanitized } }
  }

  if (type === 'cliOutput') {
    if (!isString(raw.data)) {
      return { ok: false, status: 'invalid-message', error: 'cliOutput requires string data.' }
    }

    const sessionId = isString(raw.sessionId) ? raw.sessionId : isString(raw.chatId) ? raw.chatId : undefined
    const sourceChatId = isString(raw.sourceChatId) ? raw.sourceChatId : isString(raw.chatId) ? raw.chatId : undefined
    const terminalId = isString(raw.terminalId) ? raw.terminalId : undefined

    return {
      ok: true,
      message: { type, data: raw.data, sessionId, sourceChatId, terminalId },
    }
  }

  if (type === 'streamToken') {
    if (!isString(raw.chatId) || !isString(raw.token)) {
      return { ok: false, status: 'invalid-message', error: 'streamToken requires chatId and token.' }
    }
    return { ok: true, message: { type, chatId: raw.chatId, token: raw.token } }
  }

  if (type === 'streamDone') {
    if (!isString(raw.chatId)) {
      return { ok: false, status: 'invalid-message', error: 'streamDone requires chatId.' }
    }
    return { ok: true, message: { type, chatId: raw.chatId } }
  }

  return { ok: false, status: 'invalid-message', error: `Unsupported push message type: ${type}` }
}

export function cliDebugSignature(debug: CppCliDebugState | null | undefined) {
  if (!debug) return 'none'
  // Intentionally excludes volatile timestamps to avoid churn on stateUpdate pushes.
  return JSON.stringify({
    selectedChatId: debug.selectedChatId,
    terminalCount: debug.terminalCount,
    runningTerminalCount: debug.runningTerminalCount,
    busyTerminalCount: debug.busyTerminalCount,
    terminals: debug.terminals.map((terminal) => ({
      terminalId: terminal.terminalId,
      frontendChatId: terminal.frontendChatId,
      sourceChatId: terminal.sourceChatId,
      attachedSessionId: terminal.attachedSessionId,
      providerId: terminal.providerId,
      nativeSessionId: terminal.nativeSessionId,
      processId: terminal.processId,
      running: terminal.running,
      uiAttached: terminal.uiAttached,
      turnState: terminal.turnState,
      lifecycleState: terminal.lifecycleState,
      inputReady: terminal.inputReady,
      generationInProgress: terminal.generationInProgress,
      lastError: terminal.lastError,
    })),
  })
}

export function isNewerStateRevision(nextRevision: number, currentRevision: number) {
  return nextRevision > currentRevision
}
