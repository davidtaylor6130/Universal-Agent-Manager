// Push message parsing and revision helpers. Extracted from useAppStore.ts (MO-1).
// parseUamPushPayload is the sole entry point for all C++ → frontend pushes.

import { MOBILE_COMPANION_ENABLED } from '../../config/buildFeatures'
import { isRecord, isString, sanitizeCppAppState, sanitizeCppStatePatch } from '../cpp/sanitizers'
import type { CompanionConfirmation, CppCliDebugState, ParsedPushResult } from '../cpp/types'

function isCompanionConfirmation(item: unknown): item is CompanionConfirmation {
  return isRecord(item) && isString(item.id) && isString(item.chatId) && isString(item.chatTitle) && isString(item.description) &&
    typeof item.expiresAt === 'number' && Number.isFinite(item.expiresAt)
}

export function parseUamPushPayload(payload: unknown): ParsedPushResult {
  let raw: unknown = payload

  if (typeof raw === 'string') {
    try {
      raw = JSON.parse(raw)
    } catch (err) {
      const reason = err instanceof Error ? err.message : 'unknown parse failure'
      return { ok: false, status: 'parse-error', error: `JSON parse failed: ${reason}`, mayHaveCarriedState: true }
    }
  }

  if (!isRecord(raw)) {
    return { ok: false, status: 'invalid-message', error: 'Payload is not an object.', mayHaveCarriedState: true }
  }

  const type = raw.type
  if (!isString(type) || type.trim().length === 0) {
    return { ok: false, status: 'invalid-message', error: 'Missing message type.', mayHaveCarriedState: true }
  }

  if (type === 'stateUpdate') {
    const sanitized = sanitizeCppAppState(raw.data)
    if (!sanitized) {
      return { ok: false, status: 'invalid-message', error: 'stateUpdate.data does not match CppAppState shape.', mayHaveCarriedState: true }
    }
    return { ok: true, message: { type, data: sanitized } }
  }

  if (type === 'statePatch') {
    const sanitized = sanitizeCppStatePatch(raw.data)
    if (!sanitized) {
      return { ok: false, status: 'invalid-message', error: 'statePatch.data does not match CppStatePatch shape.', mayHaveCarriedState: true }
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
    if (raw.startupTime !== undefined && (typeof raw.startupTime !== 'number' || !Number.isFinite(raw.startupTime) || raw.startupTime < 0)) {
      return { ok: false, status: 'invalid-message', error: 'cliOutput requires a valid launch timestamp.' }
    }
    const startupTime = typeof raw.startupTime === 'number' ? raw.startupTime : undefined

    return {
      ok: true,
      message: { type, data: raw.data, sessionId, sourceChatId, terminalId, startupTime },
    }
  }

  if (type === 'streamToken') {
    if (!isString(raw.chatId) || !isString(raw.token)) {
      return { ok: false, status: 'invalid-message', error: 'streamToken requires chatId and token.' }
    }
    if (raw.messageIndex !== undefined && (typeof raw.messageIndex !== 'number' || !Number.isSafeInteger(raw.messageIndex) || raw.messageIndex < 0)) {
      return { ok: false, status: 'invalid-message', error: 'streamToken messageIndex must be a nonnegative integer.' }
    }
    return { ok: true, message: { type, chatId: raw.chatId, token: raw.token, ...(raw.messageIndex === undefined ? {} : { messageIndex: raw.messageIndex }) } }
  }

  if (type === 'streamDone') {
    if (!isString(raw.chatId)) {
      return { ok: false, status: 'invalid-message', error: 'streamDone requires chatId.' }
    }
    return { ok: true, message: { type, chatId: raw.chatId } }
  }

  if (type === 'attachmentProgress') {
    if (!isString(raw.chatId) || !isString(raw.attachmentId) || typeof raw.copiedBytes !== 'number' || typeof raw.totalBytes !== 'number' ||
        !(raw.copiedBytes >= 0) || !(raw.totalBytes >= raw.copiedBytes)) {
      return { ok: false, status: 'invalid-message', error: 'attachmentProgress requires a chat, an attachment and byte counts.' }
    }
    return { ok: true, message: { type, chatId: raw.chatId, attachmentId: raw.attachmentId, copiedBytes: raw.copiedBytes, totalBytes: raw.totalBytes } }
  }

  // Phone-access builds only: the constant lets builds without it drop this parser and its names.
  if (MOBILE_COMPANION_ENABLED && type === 'companionConfirmations') {
    const items: unknown = raw.items
    if (!Array.isArray(items) || items.length > 64 || !items.every(isCompanionConfirmation)) {
      return { ok: false, status: 'invalid-message', error: 'companionConfirmations requires a list of requests.' }
    }
    return { ok: true, message: { type, items: items.map(({ id, chatId, chatTitle, description, expiresAt }) => ({ id, chatId, chatTitle, description, expiresAt })) } }
  }

  if (type === 'dictation') {
    const dictationId = raw.dictationId
    if (!isString(dictationId) || !dictationId || dictationId.length > 128) {
      return { ok: false, status: 'invalid-message', error: 'dictation requires a recording ID.' }
    }
    if (raw.event === 'interim' || raw.event === 'final') {
      if (!isString(raw.text)) {
        return { ok: false, status: 'invalid-message', error: `dictation ${raw.event} requires text.` }
      }
      return { ok: true, message: { type, dictationId, event: raw.event, text: raw.text } }
    }
    if (raw.event === 'error') {
      if (!isString(raw.message)) {
        return { ok: false, status: 'invalid-message', error: 'dictation error requires a message.' }
      }
      return { ok: true, message: { type, dictationId, event: raw.event, message: raw.message } }
    }
    if (raw.event === 'end') {
      return { ok: true, message: { type, dictationId, event: raw.event } }
    }
    return { ok: false, status: 'invalid-message', error: 'dictation requires a supported event.' }
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
