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
import { sendToCEF, isCefContext } from '../ipc/cefBridge'

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
    terminalId: string
    frontendChatId: string
    sourceChatId: string
    running: boolean
    lastError: string
  }
}

interface CppFolder {
  id: string
  title: string
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
  folders: CppFolder[]
  chats: CppChat[]
  selectedChatId: string | null
  providers: CppProvider[]
  settings: CppSettings
}

export interface CliBinding {
  terminalId: string
  boundChatId: string
  running: boolean
  lastError: string
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
    messages: Record<string, Message[]>
    agentSteps: Record<string, AgentStep[]>
    activeSessionId: string | null
  }
) {
  let msgCounter = 50000 // high enough not to clash with mock IDs

  const folders: Folder[] = cpp.folders.map((f) => ({
    id: f.id,
    name: f.title,
    parentId: null,
    isExpanded: !f.collapsed,
    createdAt: new Date(),
  }))

  const geminiCliProviders = cpp.providers.filter((p) => p.id === GEMINI_CLI_PROVIDER_ID)
  const visibleProviders = geminiCliProviders.length > 0 ? geminiCliProviders : cpp.providers
  const fallbackProviderId =
    visibleProviders[0]?.id || cpp.settings.activeProviderId || GEMINI_CLI_PROVIDER_ID

  const sessions: Session[] = cpp.chats.map((c) => {
    return {
      id: c.id,
      name: c.title || 'Untitled',
      viewMode: 'cli',
      folderId: c.folderId || null,
      createdAt: new Date(c.createdAt || Date.now()),
      updatedAt: new Date(c.updatedAt || Date.now()),
    }
  })

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

  return {
    folders,
    sessions,
    messages,
    agentSteps,
    providers,
    activeSessionId: effectiveActiveSessionId,
    theme: (cpp.settings.theme as 'dark' | 'light') || 'dark',
    activeProviderId: Object.fromEntries(
      sessions.map((s) => [
        s.id,
        providers.find((p) => p.id === GEMINI_CLI_PROVIDER_ID)?.id ?? fallbackProviderId,
      ])
    ),
    cliBindingBySessionId: Object.fromEntries(
      cpp.chats
        .filter((c) => c.cliTerminal)
        .map((c) => [
          c.id,
          {
            terminalId: c.cliTerminal?.terminalId ?? '',
            boundChatId: c.cliTerminal?.sourceChatId ?? c.id,
            running: Boolean(c.cliTerminal?.running),
            lastError: c.cliTerminal?.lastError ?? '',
          } satisfies CliBinding,
        ])
    ),
  }
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
  messages: Record<string, Message[]>
  agentSteps: Record<string, AgentStep[]>

  // Providers
  providers: Provider[]
  features: ProviderFeature[]
  activeProviderId: Record<string, string>
  cliBindingBySessionId: Record<string, CliBinding>

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
  addFolder: (name: string, parentId: string | null) => void
  toggleFolder: (id: string) => void
  renameFolder: (id: string, name: string) => void

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
        const deserialized = deserializeState(resp.data, {
          sessions: current.sessions,
          messages: current.messages,
          agentSteps: current.agentSteps,
          activeSessionId: current.activeSessionId,
        })
        set(deserialized)
        // Sync theme to DOM
        if (deserialized.theme) {
          document.documentElement.setAttribute('data-theme', deserialized.theme)
          localStorage.setItem('uam-theme', deserialized.theme)
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
          window.dispatchEvent(
            new CustomEvent('uam-cli-output', {
              detail: {
                sessionId: msg.sessionId ?? '',
                sourceChatId: msg.sourceChatId ?? '',
                terminalId: msg.terminalId ?? '',
                data: msg.data,
              },
            })
          )
          if (msg.sessionId) {
            store.setCliBinding(msg.sessionId, {
              terminalId: msg.terminalId ?? '',
              boundChatId: msg.sourceChatId ?? msg.sessionId,
              running: true,
              lastError: '',
            })
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
    messages: inCef ? {} : initialMessages,
    agentSteps: inCef ? {} : mockAgentSteps,

    providers: inCef ? [] : initialProviders,
    features: defaultFeatures,
    activeProviderId: {},
    cliBindingBySessionId: {},

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
      const deserialized = deserializeState(cppState, {
        sessions: current.sessions,
        messages: current.messages,
        agentSteps: current.agentSteps,
        activeSessionId: current.activeSessionId,
      })
      set(deserialized)
      if (deserialized.theme) {
        document.documentElement.setAttribute('data-theme', deserialized.theme)
        localStorage.setItem('uam-theme', deserialized.theme)
      }
    },

    // ---- Session actions ----

    setActiveSession: (id) => {
      set({ activeSessionId: id })
      if (isCefContext()) {
        sendToCEF({ action: 'selectSession', payload: { chatId: id } })
      }
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
          if (!resp.ok) console.error('[CEF] createSession failed:', resp.error)
        })
        set({ isNewChatModalOpen: false })
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
      set((state) => ({
        sessions: state.sessions.map((s) =>
          s.id === id ? { ...s, name, updatedAt: new Date() } : s
        ),
      }))
      if (isCefContext()) {
        sendToCEF({ action: 'renameSession', payload: { chatId: id, title: name } })
      }
    },

    deleteSession: (id) => {
      set((state) => {
        const remaining = state.sessions.filter((s) => s.id !== id)
        const { [id]: _, ...msgs } = state.messages
        const { [id]: __, ...bindings } = state.cliBindingBySessionId
        return {
          sessions: remaining,
          messages: msgs,
          cliBindingBySessionId: bindings,
          activeSessionId:
            state.activeSessionId === id ? (remaining[0]?.id ?? null) : state.activeSessionId,
        }
      })
      if (isCefContext()) {
        sendToCEF({ action: 'deleteSession', payload: { chatId: id } })
      }
    },

    setViewMode: (id, viewMode) =>
      set((state) => ({
        sessions: state.sessions.map((s) =>
          s.id === id ? { ...s, viewMode } : s
        ),
      })),

    // ---- Folder actions ----

    addFolder: (name, _parentId) => {
      if (isCefContext()) {
        sendToCEF({ action: 'createFolder', payload: { title: name } })
        return
      }

      folderCounter++
      const folder: Folder = {
        id: makeId('f', folderCounter),
        name,
        parentId: _parentId,
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

    renameFolder: (id, name) =>
      set((state) => ({
        folders: state.folders.map((f) => (f.id === id ? { ...f, name } : f)),
      })),

    // ---- Message actions ----

    sendMessage: (sessionId, content) => {
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
        sendToCEF({ action: 'sendMessage', payload: { chatId: sessionId, content } })
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
      set((state) => ({
        cliBindingBySessionId: {
          ...state.cliBindingBySessionId,
          [sessionId]: {
            terminalId: binding.terminalId ?? state.cliBindingBySessionId[sessionId]?.terminalId ?? '',
            boundChatId: binding.boundChatId ?? state.cliBindingBySessionId[sessionId]?.boundChatId ?? sessionId,
            running: binding.running ?? state.cliBindingBySessionId[sessionId]?.running ?? false,
            lastError: binding.lastError ?? state.cliBindingBySessionId[sessionId]?.lastError ?? '',
          },
        },
      })),

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
