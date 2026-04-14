import { create } from 'zustand'
import { Session, Folder, ViewMode } from '../types/session'
import { Message } from '../types/message'
import { Provider, ProviderFeature } from '../types/provider'
import {
  initialFolders,
  initialSessions,
  initialMessages,
  initialProviders,
  defaultFeatures,
  AgentStep,
  mockAgentSteps,
} from '../mock/mockData'
import { sendToCEF, isCefContext, createRequestId } from '../ipc/cefBridge'

const GEMINI_CLI_PROVIDER_ID = 'gemini-cli'
const UI_RUNTIME_BUILD_MARKER = (() => {
  const env = (import.meta as unknown as { env?: Record<string, string | undefined> }).env
  const configured = env?.VITE_UAM_UI_BUILD_ID?.trim()
  if (configured) return configured

  try {
    const chunkName = new URL(import.meta.url).pathname.split('/').pop()?.trim()
    if (chunkName) return chunkName
  } catch {
    // no-op
  }

  return 'dev'
})()

// ---------------------------------------------------------------------------
// C++ state serialisation types (mirrors state_serializer.cpp output)
// ---------------------------------------------------------------------------

interface CppMessage {
  role: 'user' | 'assistant' | 'system'
  content: string
  createdAt: string
}

interface CppChat {
  id: string
  title: string
  folderId: string
  providerId: string
  createdAt: string
  updatedAt: string
  messages: CppMessage[]
  cliTerminal?: {
    terminalId?: string
    frontendChatId?: string
    sourceChatId?: string
    running: boolean
    turnState?: 'idle' | 'busy' | string
    processing?: boolean
    readySinceLastSelect?: boolean
    active?: boolean
    lastError: string
  }
}

interface CppCliDebugTerminal {
  terminalId: string
  frontendChatId: string
  sourceChatId: string
  attachedSessionId: string
  providerId: string
  nativeSessionId: string
  processId: string
  running: boolean
  uiAttached: boolean
  turnState: 'idle' | 'busy' | string
  inputReady: boolean
  generationInProgress: boolean
  lastUserInputAt: number
  lastAiOutputAt: number
  lastPolledAt: number
  lastError: string
}

interface CppCliDebugState {
  selectedChatId: string | null
  terminalCount: number
  runningTerminalCount: number
  busyTerminalCount: number
  terminals: CppCliDebugTerminal[]
}

interface CppFolder {
  id: string
  title: string
  directory: string
  collapsed: boolean
}

interface CppProvider {
  id: string
  name: string
  shortName: string
  outputMode?: 'structured' | 'cli' | string
}

interface CppSettings {
  activeProviderId: string
  theme: string
}

export interface CppAppState {
  stateRevision?: number
  folders: CppFolder[]
  chats: CppChat[]
  cliDebug?: CppCliDebugState
  selectedChatId: string | null
  providers: CppProvider[]
  settings: CppSettings
}

export interface CliBinding {
  terminalId: string
  boundChatId: string
  running: boolean
  turnState: 'idle' | 'busy'
  processing: boolean
  readySinceLastSelect: boolean
  active: boolean
  lastError: string
}

export interface CliTranscript {
  terminalId: string
  content: string
}

export type PushChannelStatus = 'no-push-yet' | 'connected' | 'parse-error' | 'invalid-message'

type ParsedPushMessage =
  | { type: 'stateUpdate'; data: CppAppState }
  | { type: 'streamToken'; chatId: string; token: string }
  | { type: 'streamDone'; chatId: string }
  | {
      type: 'cliOutput'
      data: string
      sessionId?: string
      sourceChatId?: string
      terminalId?: string
    }

type ParsedPushResult =
  | { ok: true; message: ParsedPushMessage }
  | { ok: false; status: Exclude<PushChannelStatus, 'connected' | 'no-push-yet'>; error: string }

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null
}

function isString(value: unknown): value is string {
  return typeof value === 'string'
}

function isCppAppState(value: unknown): value is CppAppState {
  if (!isRecord(value)) return false
  return (
    Array.isArray(value.folders) &&
    Array.isArray(value.chats) &&
    Array.isArray(value.providers) &&
    isRecord(value.settings)
  )
}

function cppStateRevision(state: CppAppState): number {
  return typeof state.stateRevision === 'number' && Number.isFinite(state.stateRevision)
    ? state.stateRevision
    : 0
}

const MAX_CLI_TRANSCRIPT_BYTES = 1024 * 1024

function decodeCliChunk(encoded: string): string {
  try {
    return atob(encoded)
  } catch {
    return encoded
  }
}

function clampCliTranscript(content: string): string {
  return content.length > MAX_CLI_TRANSCRIPT_BYTES
    ? content.slice(content.length - MAX_CLI_TRANSCRIPT_BYTES)
    : content
}

function appendCliTranscriptChunk(
  existing: CliTranscript | undefined,
  terminalId: string,
  chunk: string
): CliTranscript {
  const nextTerminalId = terminalId || existing?.terminalId || ''
  const resetTranscript =
    Boolean(existing?.terminalId) && Boolean(terminalId) && existing?.terminalId !== terminalId
  const nextContent = clampCliTranscript((resetTranscript ? '' : existing?.content ?? '') + chunk)

  return {
    terminalId: nextTerminalId,
    content: nextContent,
  }
}

function normalizeCliTranscript(
  existing: CliTranscript | undefined,
  terminalId: string
): CliTranscript | undefined {
  if (!existing) {
    return undefined
  }

  if (existing.terminalId && terminalId && existing.terminalId !== terminalId) {
    return undefined
  }

  return {
    terminalId: terminalId || existing.terminalId || '',
    content: clampCliTranscript(existing.content),
  }
}

const pendingRequestIdsByKey = new Map<string, string>()

function rememberPendingRequest(key: string, requestId: string) {
  pendingRequestIdsByKey.set(key, requestId)
}

function isLatestPendingRequest(key: string, requestId?: string) {
  return typeof requestId === 'string' && pendingRequestIdsByKey.get(key) === requestId
}

function clearPendingRequest(key: string, requestId?: string) {
  if (typeof requestId === 'string' && pendingRequestIdsByKey.get(key) === requestId) {
    pendingRequestIdsByKey.delete(key)
  }
}

function parseUamPushPayload(payload: unknown): ParsedPushResult {
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
    if (!isCppAppState(raw.data)) {
      return { ok: false, status: 'invalid-message', error: 'stateUpdate.data does not match CppAppState shape.' }
    }
    return { ok: true, message: { type, data: raw.data } }
  }

  if (type === 'streamToken') {
    if (!isString(raw.chatId) || !isString(raw.token)) {
      return { ok: false, status: 'invalid-message', error: 'streamToken requires string chatId and token.' }
    }
    return { ok: true, message: { type, chatId: raw.chatId, token: raw.token } }
  }

  if (type === 'streamDone') {
    if (!isString(raw.chatId)) {
      return { ok: false, status: 'invalid-message', error: 'streamDone requires string chatId.' }
    }
    return { ok: true, message: { type, chatId: raw.chatId } }
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

  return { ok: false, status: 'invalid-message', error: `Unsupported push message type: ${type}` }
}

// ---------------------------------------------------------------------------
// Deserialiser — convert C++ format → React store format
// ---------------------------------------------------------------------------

function deserializeState(
  cpp: CppAppState,
  existing: {
    sessions: Session[]
    folders: Folder[]
    messages: Record<string, Message[]>
    agentSteps: Record<string, AgentStep[]>
    activeSessionId: string | null
    cliTranscriptBySessionId: Record<string, CliTranscript>
    cliBindingBySessionId: Record<string, CliBinding>
  }
) {
  let msgCounter = 50000 // high enough not to clash with mock IDs

  // Build lookup maps for reference-identity preservation
  const existingSessionsById = Object.fromEntries(existing.sessions.map((s) => [s.id, s]))
  const existingFoldersById = Object.fromEntries(existing.folders.map((f) => [f.id, f]))

  const newFolders: Folder[] = cpp.folders.map((f) => {
    const prev = existingFoldersById[f.id]
    const name = f.title
    const directory = f.directory ?? ''
    const isExpanded = !f.collapsed
    // Reuse reference if nothing changed — keeps memoized children stable
    if (prev && prev.name === name && prev.directory === directory && prev.isExpanded === isExpanded) {
      return prev
    }
    return {
      id: f.id,
      name,
      parentId: null,
      directory,
      isExpanded,
      createdAt: prev?.createdAt ?? new Date(),
    }
  })
  const folders: Folder[] = newFolders.length === existing.folders.length && newFolders.every((f) => f === existingFoldersById[f.id])
    ? existing.folders
    : newFolders

  const geminiCliProviders = cpp.providers.filter((p) => p.id === GEMINI_CLI_PROVIDER_ID)
  const visibleProviders = geminiCliProviders.length > 0 ? geminiCliProviders : cpp.providers
  const fallbackProviderId =
    visibleProviders[0]?.id || cpp.settings.activeProviderId || GEMINI_CLI_PROVIDER_ID

  const newSessions: Session[] = cpp.chats.map((c) => {
    const prev = existingSessionsById[c.id]
    const name = c.title || 'Untitled'
    const folderId = c.folderId || null
    // Reuse reference if nothing changed — keeps memoized children stable
    if (prev && prev.name === name && prev.folderId === folderId && prev.viewMode === 'cli') {
      return prev
    }
    return {
      id: c.id,
      name,
      viewMode: 'cli',
      folderId,
      createdAt: new Date(c.createdAt || Date.now()),
      updatedAt: new Date(c.updatedAt || Date.now()),
    }
  })
  // Reuse array reference if all elements are identical
  const sessions: Session[] = newSessions.length === existing.sessions.length && newSessions.every((s, i) => s === existing.sessions[i])
    ? existing.sessions
    : newSessions

  const messages: Record<string, Message[]> = {}
  for (const c of cpp.chats) {
    const existingMsgs = existing.messages[c.id] ?? []
    const isCurrentlyStreaming = existingMsgs.some((m) => m.isStreaming)
    const existingRealCount = existingMsgs.filter((m) => !m.isStreaming).length

    // While C++ is processing a request, the React store holds an optimistic
    // streaming placeholder.  Preserve it until C++ delivers the response
    // (detected by the C++ message count exceeding our real message count).
    if (isCurrentlyStreaming && c.messages.length <= existingRealCount) {
      messages[c.id] = existingMsgs
    } else {
      messages[c.id] = c.messages.map((m) => {
        msgCounter++
        return {
          id: `cef-m-${msgCounter}`,
          sessionId: c.id,
          role: m.role,
          content: m.content,
          createdAt: new Date(m.createdAt || Date.now()),
        } satisfies Message
      })
    }
  }

  // Preserve agentSteps for sessions that already have them.
  const agentSteps: Record<string, AgentStep[]> = { ...existing.agentSteps }

  const providers: Provider[] = visibleProviders.map((p) => ({
    id: p.id,
    name: p.name,
    shortName: p.shortName,
    color: '#f97316',  // default accent; could be persisted later
    description: '',
  }))

  const selectedByBackend =
    typeof cpp.selectedChatId === 'string' && sessions.some((s) => s.id === cpp.selectedChatId)
      ? cpp.selectedChatId
      : null
  const selectedFromCurrent =
    existing.activeSessionId && sessions.some((s) => s.id === existing.activeSessionId)
      ? existing.activeSessionId
      : null
  const effectiveActiveSessionId = selectedByBackend ?? selectedFromCurrent ?? sessions[0]?.id ?? null
  const existingBindings = existing.cliBindingBySessionId
  const cliBindingBySessionId = Object.fromEntries(
    cpp.chats
      .filter((c) => c.cliTerminal)
      .map((c) => {
        const next: CliBinding = {
          terminalId: c.cliTerminal?.terminalId ?? '',
          boundChatId: c.cliTerminal?.sourceChatId ?? c.id,
          running: Boolean(c.cliTerminal?.running),
          turnState: c.cliTerminal?.turnState === 'busy' ? 'busy' : 'idle',
          processing: Boolean(c.cliTerminal?.processing),
          readySinceLastSelect: Boolean(c.cliTerminal?.readySinceLastSelect),
          active: Boolean(c.cliTerminal?.active),
          lastError: c.cliTerminal?.lastError ?? '',
        }
        const prev = existingBindings[c.id]
        // Reuse reference if all fields match — prevents SessionItem re-renders
        if (
          prev &&
          prev.terminalId === next.terminalId &&
          prev.boundChatId === next.boundChatId &&
          prev.running === next.running &&
          prev.turnState === next.turnState &&
          prev.processing === next.processing &&
          prev.readySinceLastSelect === next.readySinceLastSelect &&
          prev.active === next.active &&
          prev.lastError === next.lastError
        ) {
          return [c.id, prev]
        }
        return [c.id, next]
      })
  ) as Record<string, CliBinding>

  const cliTranscriptBySessionId = Object.fromEntries(
    sessions.flatMap((session) => {
      const transcript = normalizeCliTranscript(
        existing.cliTranscriptBySessionId[session.id],
        cliBindingBySessionId[session.id]?.terminalId ?? ''
      )

      return transcript ? [[session.id, transcript]] : []
    })
  ) as Record<string, CliTranscript>

  return {
    folders,
    sessions,
    messages,
    agentSteps,
    providers,
    activeSessionId: effectiveActiveSessionId,
    lastAppliedStateRevision: cppStateRevision(cpp),
    theme: (cpp.settings.theme as 'dark' | 'light') || 'dark',
    activeProviderId: Object.fromEntries(
      sessions.map((s) => [
        s.id,
        providers.find((p) => p.id === GEMINI_CLI_PROVIDER_ID)?.id ?? fallbackProviderId,
      ])
    ),
    cliBindingBySessionId,
    cliTranscriptBySessionId,
    cliDebugState: cpp.cliDebug ?? null,
  }
}

function cliDebugSignature(debug: CppCliDebugState | null | undefined) {
  if (!debug) return 'none'
  return JSON.stringify(
    debug.terminals.map((terminal) => ({
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
    }))
  )
}

function isNewerStateRevision(nextRevision: number, currentRevision: number) {
  return nextRevision > currentRevision
}

// ---------------------------------------------------------------------------
// Mock responses (dev mode only)
// ---------------------------------------------------------------------------

const MOCK_RESPONSES = [
  "That's a great question. Let me walk through this carefully.\n\nThe key insight here is that you need to consider both the **immediate** and **long-term** implications. Starting with the immediate:\n\n1. The current approach works but has hidden complexity\n2. A cleaner abstraction would reduce cognitive load for future maintainers\n3. Performance characteristics are largely equivalent at this scale\n\nI'd recommend proceeding with option B — it aligns better with the existing patterns in your codebase.",
  "Looking at this from first principles:\n\n```ts\n// Before\nconst result = items.reduce((acc, item) => {\n  acc[item.id] = item\n  return acc\n}, {} as Record<string, Item>)\n\n// After — same result, cleaner\nconst result = Object.fromEntries(items.map(i => [i.id, i]))\n```\n\nThe `Object.fromEntries` version is more idiomatic modern TypeScript. Both are O(n) — no performance difference.",
  "Yes, this is a well-known pattern. The tradeoff is:\n\n- **Pros**: Simple, predictable, easy to test in isolation\n- **Cons**: Couples the caller to the implementation detail\n\nFor your use case I'd lean toward the event-driven approach since you already have an event bus in place. Reusing existing infrastructure reduces overall system complexity.",
]

let msgCounter = 100
let sessionCounter = 10
let folderCounter = 10

function makeId(prefix: string, counter: number) {
  return `${prefix}-${counter}`
}

// ---------------------------------------------------------------------------
// Store interface
// ---------------------------------------------------------------------------

interface AppState {
  // Data
  folders: Folder[]
  sessions: Session[]
  activeSessionId: string | null
  lastAppliedStateRevision: number
  messages: Record<string, Message[]>
  agentSteps: Record<string, AgentStep[]>

  // Providers
  providers: Provider[]
  features: ProviderFeature[]
  activeProviderId: Record<string, string>
  cliBindingBySessionId: Record<string, CliBinding>
  cliTranscriptBySessionId: Record<string, CliTranscript>
  cliDebugState: CppCliDebugState | null

  // UI
  theme: 'dark' | 'light'
  isNewChatModalOpen: boolean
  isSettingsOpen: boolean
  streamingMessageId: string | null
  pushChannelStatus: PushChannelStatus
  pushChannelError: string
  lastPushAtMs: number | null
  uiBuildId: string

  // Session actions
  setActiveSession: (id: string) => void
  addSession: (name: string, viewMode: ViewMode, folderId: string | null) => void
  renameSession: (id: string, name: string) => void
  deleteSession: (id: string) => void
  setViewMode: (id: string, viewMode: ViewMode) => void

  // Folder actions
  addFolder: (name: string, parentId: string | null, directory: string) => void
  toggleFolder: (id: string) => void
  renameFolder: (id: string, name: string, directory: string) => void
  deleteFolder: (id: string) => void
  browseFolderDirectory: (currentValue: string) => Promise<string | null>

  // Message actions
  sendMessage: (sessionId: string, content: string) => void
  appendStreamToken: (chatId: string, token: string) => void
  finalizeStream: (chatId: string) => void

  // Provider actions
  setActiveProvider: (sessionId: string, providerId: string) => void
  toggleFeature: (featureId: string) => void
  setCliBinding: (sessionId: string, binding: Partial<CliBinding>) => void

  // UI actions
  setTheme: (theme: 'dark' | 'light') => void
  setNewChatModalOpen: (open: boolean) => void
  setSettingsOpen: (open: boolean) => void

  // CEF bootstrap
  loadFromCef: (state: CppAppState) => void
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

export const useAppStore = create<AppState>((set, get) => {
  // Bootstrap from CEF if available (non-blocking — state arrives via uamPush later too)
  if (isCefContext()) {
    sendToCEF<CppAppState>({ action: 'getInitialState' }).then((resp) => {
      // resp.data is the raw CppAppState object. Validate shape before deserializing.
      if (resp.ok && isCppAppState(resp.data)) {
        const current = get()
        const nextRevision = cppStateRevision(resp.data)
        if (isNewerStateRevision(nextRevision, current.lastAppliedStateRevision)) {
          const deserialized = deserializeState(resp.data, {
            sessions: current.sessions,
            folders: current.folders,
            messages: current.messages,
            agentSteps: current.agentSteps,
            activeSessionId: current.activeSessionId,
            cliTranscriptBySessionId: current.cliTranscriptBySessionId,
            cliBindingBySessionId: current.cliBindingBySessionId,
          })
          set(deserialized)
          // Sync theme to DOM
          if (deserialized.theme) {
            document.documentElement.setAttribute('data-theme', deserialized.theme)
            localStorage.setItem('uam-theme', deserialized.theme)
          }
        }
      }
    })
  }

  // Register window.uamPush SYNCHRONOUSLY at module load time (before React mounts)
  // so that C++'s OnLoadEnd push is never missed.  Using getState() avoids stale closures.
  if (isCefContext() && typeof window !== 'undefined') {
    window.uamPush = (payload: unknown) => {
      const parsed = parseUamPushPayload(payload)
      if (!parsed.ok) {
        set((state) => ({
          pushChannelStatus: parsed.status,
          pushChannelError: parsed.error,
          lastPushAtMs: state.lastPushAtMs,
        }))
        console.error('[uamPush] Rejected payload:', parsed.error, payload)
        return
      }

      set({
        pushChannelStatus: 'connected',
        pushChannelError: '',
        lastPushAtMs: Date.now(),
      })

      const store = get()
      const msg = parsed.message

      switch (msg.type) {
        case 'stateUpdate':
          store.loadFromCef(msg.data)
          break
        case 'streamToken':
          store.appendStreamToken(msg.chatId, msg.token)
          break
        case 'streamDone':
          store.finalizeStream(msg.chatId)
          break
        case 'cliOutput':
          {
            const decodedData = decodeCliChunk(msg.data)
            const sessionId = msg.sessionId ?? msg.sourceChatId ?? ''

            if (sessionId) {
              set((state) => {
                const currentBinding = state.cliBindingBySessionId[sessionId]
                const terminalId = msg.terminalId ?? currentBinding?.terminalId ?? ''
                const boundChatId = msg.sourceChatId ?? currentBinding?.boundChatId ?? sessionId

                // Reuse cliBinding reference if it's already in the expected state
                const bindingUnchanged =
                  currentBinding &&
                  currentBinding.terminalId === terminalId &&
                  currentBinding.boundChatId === boundChatId &&
                  currentBinding.running === true &&
                  currentBinding.turnState === 'busy' &&
                  currentBinding.processing === true &&
                  currentBinding.readySinceLastSelect === false &&
                  currentBinding.active === false &&
                  currentBinding.lastError === ''

                return {
                  cliTranscriptBySessionId: {
                    ...state.cliTranscriptBySessionId,
                    [sessionId]: appendCliTranscriptChunk(
                      state.cliTranscriptBySessionId[sessionId],
                      terminalId,
                      decodedData
                    ),
                  },
                  ...(bindingUnchanged ? {} : {
                    cliBindingBySessionId: {
                      ...state.cliBindingBySessionId,
                      [sessionId]: {
                        terminalId,
                        boundChatId,
                        running: true,
                        turnState: 'busy',
                        processing: true,
                        readySinceLastSelect: false,
                        active: false,
                        lastError: '',
                      },
                    },
                  }),
                }
              })
            }

            window.dispatchEvent(
              new CustomEvent('uam-cli-output', {
                detail: {
                  sessionId: msg.sessionId ?? '',
                  sourceChatId: msg.sourceChatId ?? '',
                  terminalId: msg.terminalId ?? '',
                  data: decodedData,
                },
              })
            )
          }
          break
        }
      }
    }

  // In CEF context, start with empty state — real data arrives via getInitialState above.
  // In dev/browser mode, use mock data so the UI is immediately interactive.
  const inCef = isCefContext()

  return {
    folders: inCef ? [] : initialFolders,
    sessions: inCef ? [] : initialSessions,
    activeSessionId: inCef ? null : 's1',
    lastAppliedStateRevision: -1,
    messages: inCef ? {} : initialMessages,
    agentSteps: inCef ? {} : mockAgentSteps,

    providers: inCef ? [] : initialProviders,
    features: defaultFeatures,
    activeProviderId: {},
    cliBindingBySessionId: {},
    cliTranscriptBySessionId: {},
    cliDebugState: null,

    theme: (document.documentElement.getAttribute('data-theme') as 'dark' | 'light') || 'dark',
    isNewChatModalOpen: false,
    isSettingsOpen: false,
    streamingMessageId: null,
    pushChannelStatus: inCef ? 'no-push-yet' : 'connected',
    pushChannelError: '',
    lastPushAtMs: null,
    uiBuildId: UI_RUNTIME_BUILD_MARKER,

    // ---- CEF bootstrap ----

    loadFromCef: (cppState) => {
      if (!cppState || !Array.isArray(cppState.chats)) return
      const current = get()
      const nextRevision = cppStateRevision(cppState)
      if (!isNewerStateRevision(nextRevision, current.lastAppliedStateRevision)) return
      const deserialized = deserializeState(cppState, {
        sessions: current.sessions,
        folders: current.folders,
        messages: current.messages,
        agentSteps: current.agentSteps,
        activeSessionId: current.activeSessionId,
        cliTranscriptBySessionId: current.cliTranscriptBySessionId,
        cliBindingBySessionId: current.cliBindingBySessionId,
      })
      set(deserialized)
      if (deserialized.theme) {
        document.documentElement.setAttribute('data-theme', deserialized.theme)
        localStorage.setItem('uam-theme', deserialized.theme)
      }
    },

    // ---- Session actions ----

    setActiveSession: (id) => {
      if (isCefContext()) {
        const previousActiveSessionId = get().activeSessionId
        const requestKey = 'selectSession'
        const requestId = createRequestId('selectSession')
        rememberPendingRequest(requestKey, requestId)
        set({ activeSessionId: id })
        sendToCEF({ action: 'selectSession', payload: { chatId: id }, requestId }).then((resp) => {
          if (resp.ok) {
            clearPendingRequest(requestKey, resp.requestId)
            return
          }

          if (!isLatestPendingRequest(requestKey, resp.requestId)) {
            return
          }

          set({ activeSessionId: previousActiveSessionId })
          pendingRequestIdsByKey.delete(requestKey)
        })
        return
      }

      set({ activeSessionId: id })
    },

    addSession: (name, _viewMode, folderId) => {
      if (isCefContext()) {
        const state = get()
        const selectedProviderId =
          state.providers.find((p) => p.id === GEMINI_CLI_PROVIDER_ID)?.id ??
          state.activeProviderId[state.activeSessionId ?? ''] ??
          state.providers[0]?.id ??
          GEMINI_CLI_PROVIDER_ID
        sendToCEF({
          action: 'createSession',
          payload: { title: name, folderId: folderId ?? '', providerId: selectedProviderId },
        }).then((resp) => {
          if (!resp.ok) {
            console.error('[CEF] createSession failed:', resp.error)
            return
          }

          set({ isNewChatModalOpen: false })
        })
        return
      }

      // Dev/mock path
      sessionCounter++
      const id = makeId('s', sessionCounter)
      const now = new Date()
      const session: Session = { id, name, viewMode: 'cli', folderId, createdAt: now, updatedAt: now }
      set((state) => ({
        sessions: [...state.sessions, session],
        messages: { ...state.messages, [id]: [] },
        activeSessionId: id,
        isNewChatModalOpen: false,
      }))
    },

    renameSession: (id, name) => {
      if (isCefContext()) {
        const previousSession = get().sessions.find((s) => s.id === id)
        if (!previousSession) {
          return
        }

        const requestKey = `renameSession:${id}`
        const requestId = createRequestId('renameSession')
        rememberPendingRequest(requestKey, requestId)
        set((state) => ({
          sessions: state.sessions.map((s) =>
            s.id === id ? { ...s, name, updatedAt: new Date() } : s
          ),
        }))
        sendToCEF({ action: 'renameSession', payload: { chatId: id, title: name }, requestId }).then(
          (resp) => {
            if (resp.ok) {
              clearPendingRequest(requestKey, resp.requestId)
              return
            }

            if (!isLatestPendingRequest(requestKey, resp.requestId)) {
              return
            }

            set((state) => ({
              sessions: state.sessions.map((s) => (s.id === id ? previousSession : s)),
            }))
            pendingRequestIdsByKey.delete(requestKey)
          }
        )
        return
      }

      set((state) => ({
        sessions: state.sessions.map((s) =>
          s.id === id ? { ...s, name, updatedAt: new Date() } : s
        ),
      }))
    },

    deleteSession: (id) => {
      if (isCefContext()) {
        const current = get()
        const deletedSession = current.sessions.find((s) => s.id === id)
        if (!deletedSession) {
          return
        }

        const deletedIndex = current.sessions.findIndex((s) => s.id === id)
        const deletedMessages = current.messages[id] ?? []
        const deletedBinding = current.cliBindingBySessionId[id]
        const deletedTranscript = current.cliTranscriptBySessionId[id]
        const previousActiveSessionId = current.activeSessionId
        const requestKey = `deleteSession:${id}`
        const requestId = createRequestId('deleteSession')
        rememberPendingRequest(requestKey, requestId)
        set((state) => {
          const remaining = state.sessions.filter((s) => s.id !== id)
          const { [id]: _, ...msgs } = state.messages
          const { [id]: __, ...bindings } = state.cliBindingBySessionId
          const { [id]: ___, ...transcripts } = state.cliTranscriptBySessionId
          return {
            sessions: remaining,
            messages: msgs,
            cliBindingBySessionId: bindings,
            cliTranscriptBySessionId: transcripts,
            activeSessionId:
              state.activeSessionId === id ? (remaining[0]?.id ?? null) : state.activeSessionId,
          }
        })

        sendToCEF({ action: 'deleteSession', payload: { chatId: id }, requestId }).then((resp) => {
          if (resp.ok) {
            clearPendingRequest(requestKey, resp.requestId)
            return
          }

          if (!isLatestPendingRequest(requestKey, resp.requestId)) {
            return
          }

          set((state) => {
            const sessions = state.sessions.some((s) => s.id === id)
              ? state.sessions
              : [
                  ...state.sessions.slice(0, Math.min(deletedIndex, state.sessions.length)),
                  deletedSession,
                  ...state.sessions.slice(Math.min(deletedIndex, state.sessions.length)),
                ]

            return {
              sessions,
              messages: {
                ...state.messages,
                [id]: deletedMessages,
              },
              cliBindingBySessionId: deletedBinding
                ? { ...state.cliBindingBySessionId, [id]: deletedBinding }
                : state.cliBindingBySessionId,
              cliTranscriptBySessionId: deletedTranscript
                ? { ...state.cliTranscriptBySessionId, [id]: deletedTranscript }
                : state.cliTranscriptBySessionId,
              activeSessionId: previousActiveSessionId,
            }
          })
          pendingRequestIdsByKey.delete(requestKey)
        })
        return
      }

      set((state) => {
        const remaining = state.sessions.filter((s) => s.id !== id)
        const { [id]: _, ...msgs } = state.messages
        const { [id]: __, ...bindings } = state.cliBindingBySessionId
        const { [id]: ___, ...transcripts } = state.cliTranscriptBySessionId
        return {
          sessions: remaining,
          messages: msgs,
          cliBindingBySessionId: bindings,
          cliTranscriptBySessionId: transcripts,
          activeSessionId:
            state.activeSessionId === id ? (remaining[0]?.id ?? null) : state.activeSessionId,
        }
      })
    },

    setViewMode: (id, viewMode) =>
      set((state) => ({
        sessions: state.sessions.map((s) =>
          s.id === id ? { ...s, viewMode } : s
        ),
      })),

    // ---- Folder actions ----

    addFolder: (name, _parentId, directory) => {
      if (isCefContext()) {
        sendToCEF<CppFolder>({ action: 'createFolder', payload: { title: name, directory } }).then((resp) => {
          if (!resp.ok || !resp.data?.id) {
            if (!resp.ok) console.error('[CEF] createFolder failed:', resp.error)
            return
          }

          set((state) => {
            if (state.folders.some((folder) => folder.id === resp.data!.id)) {
              return {}
            }

            const createdFolder: Folder = {
              id: resp.data!.id,
              name: resp.data!.title,
              parentId: null,
              directory: resp.data!.directory ?? '',
              isExpanded: !resp.data!.collapsed,
              createdAt: new Date(),
            }

            return {
              folders: [...state.folders, createdFolder],
            }
          })
        })
        return
      }

      folderCounter++
      const folder: Folder = {
        id: makeId('f', folderCounter),
        name,
        parentId: _parentId,
        directory,
        isExpanded: true,
        createdAt: new Date(),
      }
      set((state) => ({ folders: [...state.folders, folder] }))
    },

    toggleFolder: (id) => {
      set((state) => ({
        folders: state.folders.map((f) =>
          f.id === id ? { ...f, isExpanded: !f.isExpanded } : f
        ),
      }))
      if (isCefContext()) {
        sendToCEF({ action: 'toggleFolder', payload: { folderId: id } })
      }
    },

    renameFolder: (id, name, directory) => {
      if (isCefContext()) {
        sendToCEF({ action: 'renameFolder', payload: { folderId: id, title: name, directory } })
        return
      }

      set((state) => ({
        folders: state.folders.map((f) => (f.id === id ? { ...f, name, directory } : f)),
      }))
    },

    deleteFolder: (id) => {
      if (isCefContext()) {
        sendToCEF({ action: 'deleteFolder', payload: { folderId: id } })
        return
      }

      set((state) => {
        const remainingFolders = state.folders.filter((f) => f.id !== id)
        const sessions = state.sessions.map((session) =>
          session.folderId === id ? { ...session, folderId: null } : session
        )
        return {
          folders: remainingFolders,
          sessions,
        }
      })
    },

    browseFolderDirectory: async (currentValue) => {
      if (!isCefContext()) {
        return null
      }

      const response = await sendToCEF<{ selectedPath?: string }>({
        action: 'browseFolderDirectory',
        payload: { currentValue },
      })

      const selectedPath = response.ok ? response.data?.selectedPath?.trim() ?? '' : ''
      return selectedPath.length > 0 ? selectedPath : null
    },

    // ---- Message actions ----

    sendMessage: (sessionId, content) => {
      const previousMessages = get().messages[sessionId] ?? []
      const previousStreamingMessageId = get().streamingMessageId

      msgCounter++
      const userMsg: Message = {
        id: makeId('m', msgCounter),
        sessionId,
        role: 'user',
        content,
        createdAt: new Date(),
      }

      msgCounter++
      const aiMsgId = makeId('m', msgCounter)
      const aiMsg: Message = {
        id: aiMsgId,
        sessionId,
        role: 'assistant',
        content: '',
        isStreaming: true,
        createdAt: new Date(),
      }

      set((state) => ({
        messages: {
          ...state.messages,
          [sessionId]: [...(state.messages[sessionId] ?? []), userMsg, aiMsg],
        },
        streamingMessageId: aiMsgId,
      }))

      if (isCefContext()) {
        const requestKey = `sendMessage:${sessionId}`
        const requestId = createRequestId('sendMessage')
        rememberPendingRequest(requestKey, requestId)
        sendToCEF({ action: 'sendMessage', payload: { chatId: sessionId, content }, requestId }).then(
          (resp) => {
            if (resp.ok) {
              clearPendingRequest(requestKey, resp.requestId)
              return
            }

            if (!isLatestPendingRequest(requestKey, resp.requestId)) {
              return
            }

            set((state) => ({
              messages: {
                ...state.messages,
                [sessionId]: previousMessages,
              },
              streamingMessageId: previousStreamingMessageId,
            }))
            pendingRequestIdsByKey.delete(requestKey)
          }
        )
        // Streaming tokens arrive via window.uamPush → appendStreamToken / finalizeStream
        return
      }

      // Dev/mock path — simulate streaming
      const fullResponse = MOCK_RESPONSES[Math.floor(Math.random() * MOCK_RESPONSES.length)]
      let index = 0
      const chunkSize = 4
      const interval = setInterval(() => {
        index += chunkSize
        const partial = fullResponse.slice(0, index)
        const done = index >= fullResponse.length

        set((state) => ({
          messages: {
            ...state.messages,
            [sessionId]: state.messages[sessionId].map((m) =>
              m.id === aiMsgId
                ? { ...m, content: done ? fullResponse : partial, isStreaming: !done }
                : m
            ),
          },
          streamingMessageId: done ? null : aiMsgId,
        }))

        if (done) clearInterval(interval)
      }, 20)
    },

    appendStreamToken: (chatId, token) => {
      set((state) => {
        const msgs = state.messages[chatId]
        if (!msgs) return {}
        // Append to the last streaming message (or last assistant message).
        const lastIdx = msgs.length - 1
        if (lastIdx < 0) return {}
        const last = msgs[lastIdx]
        if (last.role !== 'assistant') return {}
        return {
          messages: {
            ...state.messages,
            [chatId]: msgs.map((m, i) =>
              i === lastIdx ? { ...m, content: m.content + token, isStreaming: true } : m
            ),
          },
          streamingMessageId: last.id,
        }
      })
    },

    finalizeStream: (chatId) => {
      set((state) => {
        const msgs = state.messages[chatId]
        if (!msgs) return {}
        const lastIdx = msgs.length - 1
        if (lastIdx < 0) return {}
        return {
          messages: {
            ...state.messages,
            [chatId]: msgs.map((m, i) =>
              i === lastIdx ? { ...m, isStreaming: false } : m
            ),
          },
          streamingMessageId: null,
        }
      })
    },

    // ---- Provider actions ----

    setActiveProvider: (sessionId, providerId) =>
      set((state) => ({
        activeProviderId: { ...state.activeProviderId, [sessionId]: providerId },
      })),

    toggleFeature: (featureId) =>
      set((state) => ({
        features: state.features.map((f) =>
          f.id === featureId ? { ...f, enabled: !f.enabled } : f
        ),
      })),

    setCliBinding: (sessionId, binding) =>
      set((state) => {
        const existingBinding = state.cliBindingBySessionId[sessionId]
        const resolvedTerminalId = binding.terminalId ?? existingBinding?.terminalId ?? ''
        let nextTranscripts = state.cliTranscriptBySessionId
        const existingTranscript = state.cliTranscriptBySessionId[sessionId]

        if (existingTranscript && resolvedTerminalId) {
          if (existingTranscript.terminalId && existingTranscript.terminalId !== resolvedTerminalId) {
            nextTranscripts = {
              ...state.cliTranscriptBySessionId,
              [sessionId]: {
                terminalId: resolvedTerminalId,
                content: '',
              },
            }
          } else if (existingTranscript.terminalId !== resolvedTerminalId) {
            nextTranscripts = {
              ...state.cliTranscriptBySessionId,
              [sessionId]: {
                ...existingTranscript,
                terminalId: resolvedTerminalId,
              },
            }
          }
        }

        return {
          cliBindingBySessionId: {
            ...state.cliBindingBySessionId,
            [sessionId]: {
              terminalId: resolvedTerminalId,
              boundChatId: binding.boundChatId ?? existingBinding?.boundChatId ?? sessionId,
              running: binding.running ?? existingBinding?.running ?? false,
              turnState: binding.turnState ?? existingBinding?.turnState ?? 'idle',
              processing: binding.processing ?? existingBinding?.processing ?? false,
              readySinceLastSelect: binding.readySinceLastSelect ?? existingBinding?.readySinceLastSelect ?? false,
              active: binding.active ?? existingBinding?.active ?? false,
              lastError: binding.lastError ?? existingBinding?.lastError ?? '',
            },
          },
          cliTranscriptBySessionId: nextTranscripts,
        }
      }),

    // ---- UI actions ----

    setTheme: (theme) => {
      document.documentElement.setAttribute('data-theme', theme)
      localStorage.setItem('uam-theme', theme)
      set({ theme })
      if (isCefContext()) {
        sendToCEF({ action: 'setTheme', payload: { theme } })
      }
    },

    setNewChatModalOpen: (open) => set({ isNewChatModalOpen: open }),
    setSettingsOpen: (open) => set({ isSettingsOpen: open }),
  }
})
