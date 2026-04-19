import { create } from 'zustand'
import { Session, Folder } from '../types/session'
import { Message } from '../types/message'
import { Provider } from '../types/provider'
import { sendToCEF, isCefContext, createRequestId } from '../ipc/cefBridge'
import { applyDocumentTheme, writeStoredTheme } from '../utils/themeStorage'

const GEMINI_CLI_PROVIDER_ID = 'gemini-cli'
const ACP_APPROVAL_MODE_IDS = ['default', 'plan'] as const
const initialFolders: Folder[] = [
  {
    id: 'default',
    name: 'General',
    parentId: null,
    directory: '',
    isExpanded: true,
    createdAt: new Date(),
  },
]
const initialSessions: Session[] = [
  {
    id: 's1',
    name: 'Gemini CLI',
    viewMode: 'chat',
    folderId: 'default',
    createdAt: new Date(),
    updatedAt: new Date(),
  },
]
const initialProviders: Provider[] = [
  {
    id: GEMINI_CLI_PROVIDER_ID,
    name: 'Gemini CLI',
    shortName: 'Gemini',
    color: '#f97316',
    description: '',
    outputMode: 'cli',
    supportsCli: true,
    supportsStructured: true,
    structuredProtocol: 'gemini-acp',
  },
]
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

export type CliLifecycleState = 'disabled' | 'stopped' | 'idle' | 'busy' | 'shuttingDown'
export type AcpLifecycleState =
  | 'stopped'
  | 'starting'
  | 'ready'
  | 'processing'
  | 'waitingPermission'
  | 'error'

interface CppMessage {
  role: 'user' | 'assistant' | 'system'
  content: string
  thoughts?: string
  toolCalls?: AcpToolCall[]
  createdAt: string
}

interface CppChat {
  id: string
  title: string
	  folderId: string
	  providerId: string
	  modelId?: string
	  approvalMode?: string
	  workspaceDirectory?: string
  createdAt: string
  updatedAt: string
  lastOpenedAt?: string
  messages: CppMessage[]
  cliTerminal?: {
    terminalId?: string
    frontendChatId?: string
    sourceChatId?: string
    running: boolean
    lifecycleState?: CliLifecycleState | string
    turnState?: 'idle' | 'busy' | string
    processing?: boolean
    readySinceLastSelect?: boolean
    active?: boolean
    lastError: string
  }
  acpSession?: CppAcpSession
}

export interface AcpToolCall {
  id: string
  title: string
  kind: string
  status: string
  content: string
}

export interface AcpPlanEntry {
  content: string
  priority: string
  status: string
}

export interface AcpMode {
  id: string
  name: string
  description: string
}

export interface AcpModel {
  id: string
  name: string
  description: string
}

export type AcpTurnEvent =
  | { type: 'assistant_text'; text: string; toolCallId?: string; requestId?: string }
  | { type: 'thought'; text: string; toolCallId?: string; requestId?: string }
  | { type: 'tool_call'; toolCallId: string; text?: string; requestId?: string }
  | { type: 'permission_request'; requestId: string; toolCallId?: string; text?: string }

export interface AcpPermissionOption {
  id: string
  name: string
  kind: string
}

export interface AcpPendingPermission {
  requestId: string
  toolCallId: string
  title: string
  kind: string
  status: string
  content: string
  options: AcpPermissionOption[]
}

export interface AcpAgentInfo {
  name: string
  title: string
  version: string
}

export interface AcpDiagnosticEntry {
  time: string
  event: string
  reason: string
  method: string
  requestId: string
  code: number | null
  message: string
  detail: string
  lifecycleState: string
}

export interface CppAcpSession {
  sessionId?: string
  providerId?: string
  protocolKind?: string
  threadId?: string
  running?: boolean
  processing?: boolean
  readySinceLastSelect?: boolean
  lifecycleState?: AcpLifecycleState | string
  lastError?: string
  recentStderr?: string
  lastExitCode?: number | null
  diagnostics?: AcpDiagnosticEntry[]
  agentInfo?: Partial<AcpAgentInfo>
	  toolCalls?: AcpToolCall[]
	  planEntries?: AcpPlanEntry[]
	  availableModes?: AcpMode[]
	  currentModeId?: string
	  availableModels?: AcpModel[]
	  currentModelId?: string
	  turnEvents?: AcpTurnEvent[]
  turnUserMessageIndex?: number
  turnAssistantMessageIndex?: number
  turnSerial?: number
  pendingPermission?: AcpPendingPermission | null
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
  lifecycleState?: CliLifecycleState | string
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
  supportsCli?: boolean
  supportsStructured?: boolean
  structuredProtocol?: string
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
  selectedChatIndex?: number
  providers: CppProvider[]
  settings: CppSettings
}

export interface CliBinding {
  terminalId: string
  boundChatId: string
  running: boolean
  lifecycleState: CliLifecycleState
  turnState: 'idle' | 'busy'
  processing: boolean
  readySinceLastSelect: boolean
  active: boolean
  lastError: string
}

export interface AcpBinding {
  sessionId: string
  providerId: string
  protocolKind: string
  threadId: string
  running: boolean
  lifecycleState: AcpLifecycleState
  processing: boolean
  readySinceLastSelect: boolean
  processingStartedAtMs: number | null
  lastError: string
  recentStderr: string
  lastExitCode: number | null
  diagnostics: AcpDiagnosticEntry[]
	  toolCalls: AcpToolCall[]
	  planEntries: AcpPlanEntry[]
	  availableModes: AcpMode[]
	  currentModeId: string
	  availableModels: AcpModel[]
	  currentModelId: string
	  turnEvents: AcpTurnEvent[]
  turnUserMessageIndex: number
  turnAssistantMessageIndex: number
  turnSerial: number
  pendingPermission: AcpPendingPermission | null
  agentInfo: AcpAgentInfo | null
}

export interface CliTranscript {
  terminalId: string
  content: string
}

export type PushChannelStatus = 'no-push-yet' | 'connected' | 'parse-error' | 'invalid-message'

type ParsedPushMessage =
  | { type: 'stateUpdate'; data: CppAppState }
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

function stringOr(value: unknown, fallback = ''): string {
  return isString(value) ? value : fallback
}

function finiteNumberOr(value: unknown, fallback: number): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : fallback
}

function booleanOr(value: unknown, fallback = false): boolean {
  return typeof value === 'boolean' ? value : fallback
}

function normalizeAcpModelId(value: unknown): string {
  const modelId = stringOr(value).trim()
  return isAllowedAcpModelId(modelId) ? modelId : ''
}

function isAllowedAcpModelId(modelId: string): boolean {
  if (modelId === '') return true
  if (modelId.length > 160 || modelId.startsWith('-')) return false
  return /^[A-Za-z0-9._:/-]+$/.test(modelId)
}

function normalizeAcpApprovalMode(value: unknown): string {
  const modeId = stringOr(value).trim() || 'default'
  return (ACP_APPROVAL_MODE_IDS as readonly string[]).includes(modeId) ? modeId : 'default'
}

function sanitizeCppMessage(value: unknown): CppMessage | null {
  if (!isRecord(value)) return null
  const role = value.role
  if (role !== 'user' && role !== 'assistant' && role !== 'system') return null
  if (!isString(value.content)) return null

  return {
    role,
    content: value.content,
    thoughts: isString(value.thoughts) ? value.thoughts : undefined,
    toolCalls: Array.isArray(value.toolCalls)
      ? value.toolCalls.flatMap((toolCall) => {
          const sanitized = sanitizeToolCall(toolCall)
          return sanitized ? [sanitized] : []
        })
      : [],
    createdAt: stringOr(value.createdAt),
  }
}

function sanitizeCppCliTerminal(value: unknown): CppChat['cliTerminal'] | undefined {
  if (!isRecord(value)) return undefined

  return {
    terminalId: isString(value.terminalId) ? value.terminalId : undefined,
    frontendChatId: isString(value.frontendChatId) ? value.frontendChatId : undefined,
    sourceChatId: isString(value.sourceChatId) ? value.sourceChatId : undefined,
    running: booleanOr(value.running),
    lifecycleState: isString(value.lifecycleState) ? value.lifecycleState : undefined,
    turnState: isString(value.turnState) ? value.turnState : undefined,
    processing: typeof value.processing === 'boolean' ? value.processing : undefined,
    readySinceLastSelect: typeof value.readySinceLastSelect === 'boolean' ? value.readySinceLastSelect : undefined,
    active: typeof value.active === 'boolean' ? value.active : undefined,
    lastError: stringOr(value.lastError),
  }
}

function sanitizeDiagnostic(value: unknown): AcpDiagnosticEntry | null {
  if (!isRecord(value)) return null
  return {
    time: stringOr(value.time),
    event: stringOr(value.event),
    reason: stringOr(value.reason),
    method: stringOr(value.method),
    requestId: stringOr(value.requestId),
    code: typeof value.code === 'number' && Number.isFinite(value.code) ? value.code : null,
    message: stringOr(value.message),
    detail: stringOr(value.detail),
    lifecycleState: stringOr(value.lifecycleState),
  }
}

function sanitizeToolCall(value: unknown): AcpToolCall | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id)
  if (!id) return null
  return {
    id,
    title: stringOr(value.title),
    kind: stringOr(value.kind),
    status: stringOr(value.status),
    content: stringOr(value.content),
  }
}

function sanitizePlanEntry(value: unknown): AcpPlanEntry | null {
  if (!isRecord(value)) return null
  return {
    content: stringOr(value.content),
    priority: stringOr(value.priority),
    status: stringOr(value.status),
  }
}

function sanitizeAcpMode(value: unknown): AcpMode | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id).trim()
  if (!id) return null
  return {
    id,
    name: stringOr(value.name, id),
    description: stringOr(value.description),
  }
}

function sanitizeAcpModel(value: unknown): AcpModel | null {
  if (!isRecord(value)) return null
  const id = normalizeAcpModelId(value.id)
  if (!id) return null
  return {
    id,
    name: stringOr(value.name, id),
    description: stringOr(value.description),
  }
}

function sanitizeTurnEvent(value: unknown): AcpTurnEvent | null {
  if (!isRecord(value)) return null
  const type = value.type
  if (type === 'assistant_text' || type === 'thought') {
    return {
      type,
      text: stringOr(value.text),
      toolCallId: isString(value.toolCallId) ? value.toolCallId : undefined,
      requestId: isString(value.requestId) ? value.requestId : undefined,
    }
  }

  if (type === 'tool_call') {
    const toolCallId = stringOr(value.toolCallId)
    if (!toolCallId) return null
    return {
      type,
      toolCallId,
      text: isString(value.text) ? value.text : undefined,
      requestId: isString(value.requestId) ? value.requestId : undefined,
    }
  }

  if (type === 'permission_request') {
    const requestId = stringOr(value.requestId)
    if (!requestId) return null
    return {
      type,
      requestId,
      toolCallId: isString(value.toolCallId) ? value.toolCallId : undefined,
      text: isString(value.text) ? value.text : undefined,
    }
  }

  return null
}

function sanitizePermissionOption(value: unknown): AcpPermissionOption | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id)
  if (!id) return null
  return {
    id,
    name: stringOr(value.name),
    kind: stringOr(value.kind),
  }
}

function sanitizePendingPermission(value: unknown): AcpPendingPermission | null {
  if (value == null) return null
  if (!isRecord(value)) return null
  const requestId = stringOr(value.requestId)
  if (!requestId) return null
  return {
    requestId,
    toolCallId: stringOr(value.toolCallId),
    title: stringOr(value.title),
    kind: stringOr(value.kind),
    status: stringOr(value.status),
    content: stringOr(value.content),
    options: Array.isArray(value.options)
      ? value.options.flatMap((option) => {
          const sanitized = sanitizePermissionOption(option)
          return sanitized ? [sanitized] : []
        })
      : [],
  }
}

function sanitizeAgentInfo(value: unknown): Partial<AcpAgentInfo> | undefined {
  if (!isRecord(value)) return undefined
  return {
    name: stringOr(value.name),
    title: stringOr(value.title),
    version: stringOr(value.version),
  }
}

function sanitizeCppAcpSession(value: unknown): CppAcpSession | undefined {
  if (!isRecord(value)) return undefined

  return {
    sessionId: isString(value.sessionId) ? value.sessionId : undefined,
    providerId: isString(value.providerId) ? value.providerId : undefined,
    protocolKind: isString(value.protocolKind) ? value.protocolKind : undefined,
    threadId: isString(value.threadId) ? value.threadId : undefined,
    running: typeof value.running === 'boolean' ? value.running : undefined,
    processing: typeof value.processing === 'boolean' ? value.processing : undefined,
    readySinceLastSelect: typeof value.readySinceLastSelect === 'boolean' ? value.readySinceLastSelect : undefined,
    lifecycleState: isString(value.lifecycleState) ? value.lifecycleState : undefined,
    lastError: isString(value.lastError) ? value.lastError : undefined,
    recentStderr: isString(value.recentStderr) ? value.recentStderr : undefined,
    lastExitCode: typeof value.lastExitCode === 'number' && Number.isFinite(value.lastExitCode) ? value.lastExitCode : null,
    diagnostics: Array.isArray(value.diagnostics)
      ? value.diagnostics.flatMap((entry) => {
          const sanitized = sanitizeDiagnostic(entry)
          return sanitized ? [sanitized] : []
        })
      : [],
    agentInfo: sanitizeAgentInfo(value.agentInfo),
    toolCalls: Array.isArray(value.toolCalls)
      ? value.toolCalls.flatMap((toolCall) => {
          const sanitized = sanitizeToolCall(toolCall)
          return sanitized ? [sanitized] : []
        })
      : [],
	    planEntries: Array.isArray(value.planEntries)
	      ? value.planEntries.flatMap((entry) => {
	          const sanitized = sanitizePlanEntry(entry)
	          return sanitized ? [sanitized] : []
	        })
	      : [],
	    availableModes: Array.isArray(value.availableModes)
	      ? value.availableModes.flatMap((mode) => {
	          const sanitized = sanitizeAcpMode(mode)
	          return sanitized ? [sanitized] : []
	        })
	      : [],
	    currentModeId: normalizeAcpApprovalMode(value.currentModeId),
	    availableModels: Array.isArray(value.availableModels)
	      ? value.availableModels.flatMap((model) => {
	          const sanitized = sanitizeAcpModel(model)
	          return sanitized ? [sanitized] : []
	        })
	      : [],
	    currentModelId: normalizeAcpModelId(value.currentModelId),
	    turnEvents: Array.isArray(value.turnEvents)
	      ? value.turnEvents.flatMap((event) => {
	          const sanitized = sanitizeTurnEvent(event)
          return sanitized ? [sanitized] : []
        })
      : [],
    turnUserMessageIndex: finiteNumberOr(value.turnUserMessageIndex, -1),
    turnAssistantMessageIndex: finiteNumberOr(value.turnAssistantMessageIndex, -1),
    turnSerial: finiteNumberOr(value.turnSerial, 0),
    pendingPermission: sanitizePendingPermission(value.pendingPermission),
  }
}

function sanitizeCppFolder(value: unknown): CppFolder | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id).trim()
  if (!id) return null
  return {
    id,
    title: stringOr(value.title, 'Untitled'),
    directory: stringOr(value.directory),
    collapsed: booleanOr(value.collapsed),
  }
}

function sanitizeCppChat(value: unknown): CppChat | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id).trim()
  if (!id) return null
  return {
    id,
	    title: stringOr(value.title, 'Untitled'),
	    folderId: stringOr(value.folderId),
	    providerId: stringOr(value.providerId, GEMINI_CLI_PROVIDER_ID),
	    modelId: normalizeAcpModelId(value.modelId),
	    approvalMode: normalizeAcpApprovalMode(value.approvalMode),
	    workspaceDirectory: isString(value.workspaceDirectory) ? value.workspaceDirectory : undefined,
    createdAt: stringOr(value.createdAt),
    updatedAt: stringOr(value.updatedAt),
    lastOpenedAt: isString(value.lastOpenedAt) ? value.lastOpenedAt : undefined,
    messages: Array.isArray(value.messages)
      ? value.messages.flatMap((message) => {
          const sanitized = sanitizeCppMessage(message)
          return sanitized ? [sanitized] : []
        })
      : [],
    cliTerminal: sanitizeCppCliTerminal(value.cliTerminal),
    acpSession: sanitizeCppAcpSession(value.acpSession),
  }
}

function sanitizeCppProvider(value: unknown): CppProvider | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id).trim()
  if (!id) return null
  return {
    id,
    name: stringOr(value.name, id),
    shortName: stringOr(value.shortName, stringOr(value.name, id)),
    outputMode: isString(value.outputMode) ? value.outputMode : undefined,
    supportsCli: typeof value.supportsCli === 'boolean' ? value.supportsCli : undefined,
    supportsStructured: typeof value.supportsStructured === 'boolean' ? value.supportsStructured : undefined,
    structuredProtocol: isString(value.structuredProtocol) ? value.structuredProtocol : undefined,
  }
}

function sanitizeCliDebugTerminal(value: unknown): CppCliDebugTerminal | null {
  if (!isRecord(value)) return null
  const terminalId = stringOr(value.terminalId)
  if (!terminalId) return null
  return {
    terminalId,
    frontendChatId: stringOr(value.frontendChatId),
    sourceChatId: stringOr(value.sourceChatId),
    attachedSessionId: stringOr(value.attachedSessionId),
    providerId: stringOr(value.providerId),
    nativeSessionId: stringOr(value.nativeSessionId),
    processId: stringOr(value.processId),
    running: booleanOr(value.running),
    uiAttached: booleanOr(value.uiAttached),
    lifecycleState: isString(value.lifecycleState) ? value.lifecycleState : undefined,
    turnState: isString(value.turnState) ? value.turnState : 'idle',
    inputReady: booleanOr(value.inputReady),
    generationInProgress: booleanOr(value.generationInProgress),
    lastUserInputAt: finiteNumberOr(value.lastUserInputAt, 0),
    lastAiOutputAt: finiteNumberOr(value.lastAiOutputAt, 0),
    lastPolledAt: finiteNumberOr(value.lastPolledAt, 0),
    lastError: stringOr(value.lastError),
  }
}

function sanitizeCliDebugState(value: unknown): CppCliDebugState | undefined {
  if (!isRecord(value)) return undefined
  const terminals = Array.isArray(value.terminals)
    ? value.terminals.flatMap((terminal) => {
        const sanitized = sanitizeCliDebugTerminal(terminal)
        return sanitized ? [sanitized] : []
      })
    : []

  return {
    selectedChatId: isString(value.selectedChatId) ? value.selectedChatId : null,
    terminalCount: finiteNumberOr(value.terminalCount, terminals.length),
    runningTerminalCount: finiteNumberOr(value.runningTerminalCount, terminals.filter((terminal) => terminal.running).length),
    busyTerminalCount: finiteNumberOr(value.busyTerminalCount, terminals.filter((terminal) => terminal.turnState === 'busy').length),
    terminals,
  }
}

function sanitizeCppSettings(value: unknown): CppSettings {
  if (!isRecord(value)) {
    return { activeProviderId: GEMINI_CLI_PROVIDER_ID, theme: 'dark' }
  }

  const theme = value.theme === 'light' || value.theme === 'dark' ? value.theme : 'dark'
  return {
    activeProviderId: stringOr(value.activeProviderId, GEMINI_CLI_PROVIDER_ID),
    theme,
  }
}

function sanitizeCppAppState(value: unknown): CppAppState | null {
  if (!isRecord(value)) return null

  const folders = Array.isArray(value.folders)
    ? value.folders.flatMap((folder) => {
        const sanitized = sanitizeCppFolder(folder)
        return sanitized ? [sanitized] : []
      })
    : []
  const chats = Array.isArray(value.chats)
    ? value.chats.flatMap((chat) => {
        const sanitized = sanitizeCppChat(chat)
        return sanitized ? [sanitized] : []
      })
    : []
  const providers = Array.isArray(value.providers)
    ? value.providers.flatMap((provider) => {
        const sanitized = sanitizeCppProvider(provider)
        return sanitized ? [sanitized] : []
      })
    : []

  const selectedChatId =
    isString(value.selectedChatId)
      ? value.selectedChatId
      : typeof value.selectedChatIndex === 'number' &&
          Number.isInteger(value.selectedChatIndex) &&
          value.selectedChatIndex >= 0 &&
          value.selectedChatIndex < chats.length
        ? chats[value.selectedChatIndex].id
        : null

  return {
    stateRevision: finiteNumberOr(value.stateRevision, 0),
    folders,
    chats,
    cliDebug: sanitizeCliDebugState(value.cliDebug),
    selectedChatId,
    selectedChatIndex: finiteNumberOr(value.selectedChatIndex, -1),
    providers,
    settings: sanitizeCppSettings(value.settings),
  }
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

function normalizeCliLifecycleState(
  value: unknown,
  running: boolean,
  turnState?: string,
  processing?: boolean
): CliLifecycleState {
  if (
    value === 'disabled' ||
    value === 'stopped' ||
    value === 'idle' ||
    value === 'busy' ||
    value === 'shuttingDown'
  ) {
    return value
  }

  if (!running) {
    return 'stopped'
  }

  if (turnState === 'busy' || processing) {
    return 'busy'
  }

  return 'idle'
}

function cliLifecycleIsProcessing(lifecycleState: CliLifecycleState): boolean {
  return lifecycleState === 'busy' || lifecycleState === 'shuttingDown'
}

function normalizeAcpLifecycleState(value: unknown, running: boolean, processing: boolean): AcpLifecycleState {
  if (
    value === 'stopped' ||
    value === 'starting' ||
    value === 'ready' ||
    value === 'processing' ||
    value === 'waitingPermission' ||
    value === 'error'
  ) {
    return value
  }

  if (processing) return 'processing'
  if (running) return 'ready'
  return 'stopped'
}

function acpBindingSignature(binding: AcpBinding | undefined) {
  if (!binding) return ''
  return JSON.stringify({
    sessionId: binding.sessionId,
    providerId: binding.providerId,
    protocolKind: binding.protocolKind,
    threadId: binding.threadId,
    running: binding.running,
    lifecycleState: binding.lifecycleState,
    processing: binding.processing,
    readySinceLastSelect: binding.readySinceLastSelect,
    processingStartedAtMs: binding.processingStartedAtMs,
    lastError: binding.lastError,
    recentStderr: binding.recentStderr,
    lastExitCode: binding.lastExitCode,
	    diagnostics: binding.diagnostics,
	    toolCalls: binding.toolCalls,
	    planEntries: binding.planEntries,
	    availableModes: binding.availableModes,
	    currentModeId: binding.currentModeId,
	    availableModels: binding.availableModels,
	    currentModelId: binding.currentModelId,
	    turnEvents: binding.turnEvents,
    turnUserMessageIndex: binding.turnUserMessageIndex,
    turnAssistantMessageIndex: binding.turnAssistantMessageIndex,
    turnSerial: binding.turnSerial,
    pendingPermission: binding.pendingPermission,
    agentInfo: binding.agentInfo,
  })
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

  const nextTranscript: CliTranscript = {
    terminalId: terminalId || existing.terminalId || '',
    content: clampCliTranscript(existing.content),
  }

  if (
    existing.terminalId === nextTranscript.terminalId &&
    existing.content === nextTranscript.content
  ) {
    return existing
  }

  return nextTranscript
}

const pendingRequestIdsByKey = new Map<string, string>()

function rememberPendingRequest(key: string, requestId: string) {
  pendingRequestIdsByKey.set(key, requestId)
}

function isLatestPendingRequest(key: string, requestId?: string) {
  return typeof requestId === 'string' && pendingRequestIdsByKey.get(key) === requestId
}

function sameRecordEntries<T>(existing: Record<string, T>, next: Record<string, T>) {
  const existingKeys = Object.keys(existing)
  const nextKeys = Object.keys(next)

  if (existingKeys.length !== nextKeys.length) {
    return false
  }

  for (const key of nextKeys) {
    if (!Object.prototype.hasOwnProperty.call(existing, key)) {
      return false
    }

    if (!Object.is(existing[key], next[key])) {
      return false
    }
  }

  return true
}

function sameArrayEntries<T>(existing: T[], next: T[]) {
  if (existing.length !== next.length) {
    return false
  }

  for (let i = 0; i < next.length; i++) {
    if (!Object.is(existing[i], next[i])) {
      return false
    }
  }

  return true
}

function cppMessageCreatedAtMillis(message: CppMessage) {
  if (!message.createdAt) {
    return Date.now()
  }

  const timestamp = Date.parse(message.createdAt)
  return Number.isFinite(timestamp) ? timestamp : Date.now()
}

function cppMessagesEquivalent(existing: Message, next: CppMessage) {
  return (
    existing.role === next.role &&
    existing.content === next.content &&
    (existing.thoughts ?? '') === (next.thoughts ?? '') &&
    toolCallsEquivalent(existing.toolCalls ?? [], next.toolCalls ?? []) &&
    existing.createdAt.getTime() === cppMessageCreatedAtMillis(next)
  )
}

function toolCallsEquivalent(existing: AcpToolCall[], next: AcpToolCall[]) {
  if (existing.length !== next.length) return false
  return existing.every((tool, index) => {
    const other = next[index]
    return (
      tool.id === other.id &&
      tool.title === other.title &&
      tool.kind === other.kind &&
      tool.status === other.status &&
      tool.content === other.content
    )
  })
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
    const sanitized = sanitizeCppAppState(raw.data)
    if (!sanitized) {
      return { ok: false, status: 'invalid-message', error: 'stateUpdate.data does not match CppAppState shape.' }
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
    providers: Provider[]
    activeSessionId: string | null
    cliTranscriptBySessionId: Record<string, CliTranscript>
    cliBindingBySessionId: Record<string, CliBinding>
    acpBindingBySessionId: Record<string, AcpBinding>
    cliDebugState: CppCliDebugState | null
  }
) {
  const buildMessage = (chatId: string, message: CppMessage, index: number): Message => {
    const createdAtMillis = cppMessageCreatedAtMillis(message)
    return {
      // Stable across refreshes (unlike an incrementing counter).
      id: `cef-m-${chatId}-${createdAtMillis}-${index}-${message.role}`,
      sessionId: chatId,
      role: message.role,
      content: message.content,
      thoughts: message.thoughts ?? '',
      toolCalls: message.toolCalls ?? [],
      createdAt: new Date(createdAtMillis),
    } satisfies Message
  }

  // Build lookup maps for reference-identity preservation
  const existingSessionsById = Object.fromEntries(existing.sessions.map((s) => [s.id, s]))
  const existingFoldersById = Object.fromEntries(existing.folders.map((f) => [f.id, f]))
  const existingProvidersById = Object.fromEntries(existing.providers.map((p) => [p.id, p]))

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

  const visibleProviders = cpp.providers.length > 0
    ? cpp.providers
    : [{ id: GEMINI_CLI_PROVIDER_ID, name: 'Gemini CLI', shortName: 'Gemini', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' }]
  const newSessions: Session[] = cpp.chats.map((c) => {
    const prev = existingSessionsById[c.id]
    const name = c.title || 'Untitled'
    const folderId = c.folderId || null
    const workspaceDirectory = c.workspaceDirectory ?? ''
    const providerId = c.providerId || GEMINI_CLI_PROVIDER_ID
    const modelId = c.modelId ?? ''
    const approvalMode = normalizeAcpApprovalMode(c.approvalMode)
    const createdAt = new Date(c.createdAt || Date.now())
    const updatedAt = new Date(c.updatedAt || Date.now())
    const lastOpenedAt = new Date(c.lastOpenedAt || c.updatedAt || c.createdAt || Date.now())
	    // Reuse reference if nothing changed — keeps memoized children stable
	    if (
	      prev &&
	      prev.name === name &&
	      prev.folderId === folderId &&
	      (prev.providerId ?? GEMINI_CLI_PROVIDER_ID) === providerId &&
	      (prev.modelId ?? '') === modelId &&
	      (prev.approvalMode ?? 'default') === approvalMode &&
	      prev.workspaceDirectory === workspaceDirectory &&
	      prev.viewMode === 'chat' &&
	      prev.createdAt.getTime() === createdAt.getTime() &&
	      prev.updatedAt.getTime() === updatedAt.getTime() &&
	      (prev.lastOpenedAt ?? prev.updatedAt).getTime() === lastOpenedAt.getTime()
	    ) {
      return prev
    }
    return {
      id: c.id,
      name,
      viewMode: 'chat',
	      folderId,
	      providerId,
	      modelId,
	      approvalMode,
	      workspaceDirectory,
      createdAt,
      updatedAt,
      lastOpenedAt,
    }
  })
  // Reuse array reference if all elements are identical
  const sessions: Session[] = newSessions.length === existing.sessions.length && newSessions.every((s, i) => s === existing.sessions[i])
    ? existing.sessions
    : newSessions

  const nextMessages: Record<string, Message[]> = {}
  for (const c of cpp.chats) {
    const existingMsgs = existing.messages[c.id] ?? []
    const existingRealMsgs = existingMsgs.filter((m) => !m.isStreaming)
    const hasStreamingPlaceholder = existingMsgs.some((m) => m.isStreaming)

    // While C++ is processing a request, the React store holds an optimistic
    // streaming placeholder.  Preserve it until C++ delivers the response
    // (detected by the C++ message count exceeding our real message count).
    if (hasStreamingPlaceholder && c.messages.length <= existingRealMsgs.length) {
      nextMessages[c.id] = existingMsgs
      continue
    }

    let prefixLength = 0
    while (
      prefixLength < existingRealMsgs.length &&
      prefixLength < c.messages.length &&
      cppMessagesEquivalent(existingRealMsgs[prefixLength], c.messages[prefixLength])
    ) {
      prefixLength++
    }

    if (!hasStreamingPlaceholder && prefixLength === existingRealMsgs.length && prefixLength === c.messages.length) {
      nextMessages[c.id] = existingMsgs
      continue
    }

    const reconciledMessages: Message[] = existingRealMsgs.slice(0, prefixLength)
    for (let i = prefixLength; i < c.messages.length; i++) {
      reconciledMessages.push(buildMessage(c.id, c.messages[i], i))
    }
    nextMessages[c.id] = reconciledMessages
  }
  const messages = sameRecordEntries(existing.messages, nextMessages) ? existing.messages : nextMessages

  const nextProviders: Provider[] = visibleProviders.map((p) => {
    const prev = existingProvidersById[p.id]
    const nextProvider: Provider = {
      id: p.id,
      name: p.name,
      shortName: p.shortName,
      // Preserve any UI-only provider metadata if it already exists.
      color: prev?.color ?? '#f97316', // default accent; could be persisted later
      description: prev?.description ?? '',
      outputMode: p.outputMode,
      supportsCli: p.supportsCli,
      supportsStructured: p.supportsStructured,
      structuredProtocol: p.structuredProtocol,
    }

    if (
      prev &&
      prev.id === nextProvider.id &&
      prev.name === nextProvider.name &&
      prev.shortName === nextProvider.shortName &&
      prev.color === nextProvider.color &&
      prev.description === nextProvider.description &&
      prev.outputMode === nextProvider.outputMode &&
      prev.supportsCli === nextProvider.supportsCli &&
      prev.supportsStructured === nextProvider.supportsStructured &&
      prev.structuredProtocol === nextProvider.structuredProtocol
    ) {
      return prev
    }

    return nextProvider
  })
  const providers = sameArrayEntries(existing.providers, nextProviders) ? existing.providers : nextProviders

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
  const nextCliBindingBySessionId = Object.fromEntries(
    cpp.chats
      .filter((c) => c.cliTerminal)
      .map((c) => {
        const running = Boolean(c.cliTerminal?.running)
        const lifecycleState = normalizeCliLifecycleState(
          c.cliTerminal?.lifecycleState,
          running,
          c.cliTerminal?.turnState,
          c.cliTerminal?.processing
        )
        const processing = Boolean(c.cliTerminal?.processing) || cliLifecycleIsProcessing(lifecycleState)
        const next: CliBinding = {
          terminalId: c.cliTerminal?.terminalId ?? '',
          boundChatId: c.cliTerminal?.sourceChatId ?? c.id,
          running,
          lifecycleState,
          turnState: processing ? 'busy' : 'idle',
          processing,
          readySinceLastSelect: Boolean(c.cliTerminal?.readySinceLastSelect),
          active: lifecycleState === 'idle' && running,
          lastError: c.cliTerminal?.lastError ?? '',
        }
        const prev = existingBindings[c.id]
        // Reuse reference if all fields match — prevents SessionItem re-renders
        if (
          prev &&
          prev.terminalId === next.terminalId &&
          prev.boundChatId === next.boundChatId &&
          prev.running === next.running &&
          prev.lifecycleState === next.lifecycleState &&
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
  const cliBindingBySessionId = sameRecordEntries(existingBindings, nextCliBindingBySessionId)
    ? existingBindings
    : nextCliBindingBySessionId

  const existingAcpBindings = existing.acpBindingBySessionId
  const nextAcpBindingBySessionId = Object.fromEntries(
    cpp.chats.map((c) => {
      const acp = c.acpSession
      const running = Boolean(acp?.running)
      const processing = Boolean(acp?.processing)
      const lifecycleState = normalizeAcpLifecycleState(acp?.lifecycleState, running, processing)
      const effectiveProcessing =
        lifecycleState === 'error' ? false : processing || lifecycleState === 'processing' || lifecycleState === 'waitingPermission'
      const prev = existingAcpBindings[c.id]
      const next: AcpBinding = {
        sessionId: acp?.sessionId ?? '',
        providerId: acp?.providerId ?? c.providerId ?? GEMINI_CLI_PROVIDER_ID,
        protocolKind: acp?.protocolKind ?? '',
        threadId: acp?.threadId ?? '',
        running,
        lifecycleState,
        processing: effectiveProcessing,
        readySinceLastSelect: Boolean(acp?.readySinceLastSelect),
        processingStartedAtMs: effectiveProcessing
          ? prev?.processing
            ? prev.processingStartedAtMs ?? Date.now()
            : Date.now()
          : null,
        lastError: acp?.lastError ?? '',
        recentStderr: acp?.recentStderr ?? '',
        lastExitCode: typeof acp?.lastExitCode === 'number' ? acp.lastExitCode : null,
	        diagnostics: Array.isArray(acp?.diagnostics) ? acp!.diagnostics : [],
	        toolCalls: Array.isArray(acp?.toolCalls) ? acp!.toolCalls : [],
	        planEntries: Array.isArray(acp?.planEntries) ? acp!.planEntries : [],
	        availableModes: Array.isArray(acp?.availableModes) ? acp!.availableModes : [],
	        currentModeId: normalizeAcpApprovalMode(acp?.currentModeId ?? c.approvalMode),
	        availableModels: Array.isArray(acp?.availableModels) ? acp!.availableModels : [],
	        currentModelId: normalizeAcpModelId(acp?.currentModelId ?? c.modelId),
	        turnEvents: Array.isArray(acp?.turnEvents) ? acp!.turnEvents : [],
        turnUserMessageIndex: typeof acp?.turnUserMessageIndex === 'number' ? acp.turnUserMessageIndex : -1,
        turnAssistantMessageIndex: typeof acp?.turnAssistantMessageIndex === 'number' ? acp.turnAssistantMessageIndex : -1,
        turnSerial: typeof acp?.turnSerial === 'number' ? acp.turnSerial : 0,
        pendingPermission: acp?.pendingPermission ?? null,
        agentInfo: acp?.agentInfo
          ? {
              name: acp.agentInfo.name ?? '',
              title: acp.agentInfo.title ?? '',
              version: acp.agentInfo.version ?? '',
            }
          : null,
      }
      if (acpBindingSignature(prev) === acpBindingSignature(next)) {
        return [c.id, prev]
      }
      return [c.id, next]
    })
  ) as Record<string, AcpBinding>
  const acpBindingBySessionId = sameRecordEntries(existingAcpBindings, nextAcpBindingBySessionId)
    ? existingAcpBindings
    : nextAcpBindingBySessionId

  const nextCliTranscriptBySessionId = Object.fromEntries(
    sessions.flatMap((session) => {
      const transcript = normalizeCliTranscript(
        existing.cliTranscriptBySessionId[session.id],
        cliBindingBySessionId[session.id]?.terminalId ?? ''
      )

      return transcript ? [[session.id, transcript]] : []
    })
  ) as Record<string, CliTranscript>
  const cliTranscriptBySessionId = sameRecordEntries(
    existing.cliTranscriptBySessionId,
    nextCliTranscriptBySessionId
  )
    ? existing.cliTranscriptBySessionId
    : nextCliTranscriptBySessionId

  const nextCliDebugState = cpp.cliDebug ?? null
  const cliDebugState =
    cliDebugSignature(existing.cliDebugState) === cliDebugSignature(nextCliDebugState)
      ? existing.cliDebugState
      : nextCliDebugState

  return {
    folders,
    sessions,
    messages,
    providers,
    activeSessionId: effectiveActiveSessionId,
    lastAppliedStateRevision: cppStateRevision(cpp),
    theme: (cpp.settings.theme as 'dark' | 'light') || 'dark',
    cliBindingBySessionId,
    acpBindingBySessionId,
    cliTranscriptBySessionId,
    cliDebugState,
  }
}

function cliDebugSignature(debug: CppCliDebugState | null | undefined) {
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

function isNewerStateRevision(nextRevision: number, currentRevision: number) {
  return nextRevision > currentRevision
}

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

  // Providers
  providers: Provider[]
  cliBindingBySessionId: Record<string, CliBinding>
  acpBindingBySessionId: Record<string, AcpBinding>
  cliTranscriptBySessionId: Record<string, CliTranscript>
  cliDebugState: CppCliDebugState | null

  // UI
  theme: 'dark' | 'light'
  isNewChatModalOpen: boolean
  newChatFolderId: string | null
  isSettingsOpen: boolean
  streamingMessageId: string | null
  pushChannelStatus: PushChannelStatus
  pushChannelError: string
  lastPushAtMs: number | null
  uiBuildId: string

  // Session actions
	  setActiveSession: (id: string) => void
	  addSession: (name: string, folderId: string | null, providerId?: string) => void
	  renameSession: (id: string, name: string) => void
	  setSessionProvider: (id: string, providerId: string) => Promise<boolean>
	  setSessionModel: (id: string, modelId: string) => Promise<boolean>
	  setSessionApprovalMode: (id: string, modeId: string) => Promise<boolean>
	  deleteSession: (id: string) => void

  // Folder actions
  addFolder: (name: string, parentId: string | null, directory: string) => Promise<boolean>
  toggleFolder: (id: string) => void
  renameFolder: (id: string, name: string, directory: string) => void
  deleteFolder: (id: string) => void
  browseFolderDirectory: (currentValue: string) => Promise<string | null>

  // CLI actions
  setCliBinding: (sessionId: string, binding: Partial<CliBinding>) => void

  // ACP actions
  sendAcpPrompt: (sessionId: string, text: string) => Promise<boolean>
  cancelAcpTurn: (sessionId: string) => Promise<boolean>
  resolveAcpPermission: (sessionId: string, requestId: string, optionId: string | 'cancelled') => Promise<boolean>
  stopAcpSession: (sessionId: string) => Promise<boolean>

  // UI actions
  setTheme: (theme: 'dark' | 'light') => void
  setNewChatModalOpen: (open: boolean, folderId?: string | null) => void
  setSettingsOpen: (open: boolean) => void

  // CEF bootstrap
  loadFromCef: (state: CppAppState) => void
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

function readDocumentTheme(): 'dark' | 'light' {
  if (typeof document === 'undefined') return 'dark'
  const value = document.documentElement.getAttribute('data-theme')
  return value === 'light' ? 'light' : 'dark'
}

function persistTheme(theme: 'dark' | 'light'): void {
  applyDocumentTheme(theme)
  writeStoredTheme(theme)
}

export const useAppStore = create<AppState>((set, get) => {
  // Bootstrap from CEF if available (non-blocking — state arrives via uamPush later too)
	  if (isCefContext()) {
	    sendToCEF<CppAppState>({ action: 'getInitialState' }).then((resp) => {
	      // resp.data is the raw CppAppState object. Sanitize before deserializing.
	      const sanitized = resp.ok ? sanitizeCppAppState(resp.data) : null
	      if (sanitized) {
	        const current = get()
	        const nextRevision = cppStateRevision(sanitized)
	        if (isNewerStateRevision(nextRevision, current.lastAppliedStateRevision)) {
	          const deserialized = deserializeState(sanitized, {
	            sessions: current.sessions,
            folders: current.folders,
            messages: current.messages,
            providers: current.providers,
            activeSessionId: current.activeSessionId,
            cliTranscriptBySessionId: current.cliTranscriptBySessionId,
            cliBindingBySessionId: current.cliBindingBySessionId,
            acpBindingBySessionId: current.acpBindingBySessionId,
            cliDebugState: current.cliDebugState,
          })
          set(deserialized)
          // Sync theme to DOM
          if (deserialized.theme) {
            persistTheme(deserialized.theme)
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
        case 'cliOutput':
          {
            const decodedData = decodeCliChunk(msg.data)
            const sessionId = msg.sessionId ?? msg.sourceChatId ?? ''

            if (sessionId) {
              set((state) => {
                const currentBinding = state.cliBindingBySessionId[sessionId]
                const terminalId = msg.terminalId ?? currentBinding?.terminalId ?? ''
                const boundChatId = msg.sourceChatId ?? currentBinding?.boundChatId ?? sessionId
                const bindingChanged =
                  currentBinding &&
                  (currentBinding.terminalId !== terminalId ||
                    currentBinding.boundChatId !== boundChatId)

                return {
                  cliTranscriptBySessionId: {
                    ...state.cliTranscriptBySessionId,
                    [sessionId]: appendCliTranscriptChunk(
                      state.cliTranscriptBySessionId[sessionId],
                      terminalId,
                      decodedData
                    ),
                  },
                  ...(bindingChanged ? {
                    cliBindingBySessionId: {
                      ...state.cliBindingBySessionId,
                      [sessionId]: {
                        ...(currentBinding as CliBinding),
                        terminalId,
                        boundChatId,
                      },
                    },
                  } : {}),
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
    messages: {},

    providers: inCef ? [] : initialProviders,
    cliBindingBySessionId: {},
    acpBindingBySessionId: {},
    cliTranscriptBySessionId: {},
    cliDebugState: null,

    theme: readDocumentTheme(),
    isNewChatModalOpen: false,
    newChatFolderId: null,
    isSettingsOpen: false,
    streamingMessageId: null,
    pushChannelStatus: inCef ? 'no-push-yet' : 'connected',
    pushChannelError: '',
    lastPushAtMs: null,
    uiBuildId: UI_RUNTIME_BUILD_MARKER,

    // ---- CEF bootstrap ----

	    loadFromCef: (cppState) => {
	      const sanitized = sanitizeCppAppState(cppState)
	      if (!sanitized) return
	      const current = get()
	      const nextRevision = cppStateRevision(sanitized)
	      if (!isNewerStateRevision(nextRevision, current.lastAppliedStateRevision)) return
	      const deserialized = deserializeState(sanitized, {
	        sessions: current.sessions,
        folders: current.folders,
        messages: current.messages,
        providers: current.providers,
        activeSessionId: current.activeSessionId,
        cliTranscriptBySessionId: current.cliTranscriptBySessionId,
        cliBindingBySessionId: current.cliBindingBySessionId,
        acpBindingBySessionId: current.acpBindingBySessionId,
        cliDebugState: current.cliDebugState,
      })
      set(deserialized)
      if (deserialized.theme) {
        persistTheme(deserialized.theme)
      }
    },

    // ---- Session actions ----

    setActiveSession: (id) => {
      if (isCefContext()) {
        const previousActiveSessionId = get().activeSessionId
        const requestKey = 'selectSession'
        const requestId = createRequestId('selectSession')
        rememberPendingRequest(requestKey, requestId)
        const openedAt = new Date()
        const previousSession = get().sessions.find((s) => s.id === id)
        set((state) => ({
          activeSessionId: id,
          sessions: state.sessions.map((s) =>
            s.id === id ? { ...s, lastOpenedAt: openedAt } : s
          ),
        }))
        sendToCEF({ action: 'selectSession', payload: { chatId: id }, requestId }).then((resp) => {
          if (resp.ok) {
            clearPendingRequest(requestKey, resp.requestId)
            return
          }

          if (!isLatestPendingRequest(requestKey, resp.requestId)) {
            return
          }

          set((state) => ({
            activeSessionId: previousActiveSessionId,
            sessions: previousSession
              ? state.sessions.map((s) =>
                  s.id === id ? { ...s, lastOpenedAt: previousSession.lastOpenedAt } : s
                )
              : state.sessions,
          }))
          pendingRequestIdsByKey.delete(requestKey)
        })
        return
      }

      set((state) => {
        const openedAt = new Date()
        return {
          activeSessionId: id,
          sessions: state.sessions.map((s) =>
            s.id === id ? { ...s, lastOpenedAt: openedAt } : s
          ),
        }
      })
    },

    addSession: (name, folderId, providerId = GEMINI_CLI_PROVIDER_ID) => {
      if (isCefContext()) {
        sendToCEF({
          action: 'createSession',
          payload: { title: name, folderId: folderId ?? '', providerId },
        }).then((resp) => {
          if (!resp.ok) {
            console.error('[CEF] createSession failed:', resp.error)
            return
          }

          set({ isNewChatModalOpen: false, newChatFolderId: null })
        })
        return
      }

      // Dev/mock path
      sessionCounter++
      const id = makeId('s', sessionCounter)
      const now = new Date()
      const session: Session = { id, name, viewMode: 'chat', folderId, providerId, createdAt: now, updatedAt: now, lastOpenedAt: now }
      set((state) => ({
        sessions: [...state.sessions, session],
        messages: { ...state.messages, [id]: [] },
        activeSessionId: id,
        isNewChatModalOpen: false,
        newChatFolderId: null,
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

	    setSessionProvider: async (id, providerId) => {
	      const requestedProviderId = providerId.trim()
	      const current = get()
	      if (!requestedProviderId || !current.providers.some((provider) => provider.id === requestedProviderId)) {
	        return false
	      }

	      const previousSession = current.sessions.find((s) => s.id === id)
	      if (!previousSession) {
	        return false
	      }

	      if ((previousSession.providerId ?? GEMINI_CLI_PROVIDER_ID) === requestedProviderId) {
	        return true
	      }

	      const acp = current.acpBindingBySessionId[id]
	      const messages = current.messages[id] ?? []
	      if (messages.length > 0 || acp?.running || acp?.processing) {
	        return false
	      }

	      const applyProvider = () => {
	        set((state) => ({
	          sessions: state.sessions.map((s) =>
	            s.id === id ? { ...s, providerId: requestedProviderId, modelId: '', approvalMode: 'default', updatedAt: new Date() } : s
	          ),
	        }))
	      }

	      if (isCefContext()) {
	        const requestKey = `setSessionProvider:${id}`
	        const requestId = createRequestId('setSessionProvider')
	        rememberPendingRequest(requestKey, requestId)
	        applyProvider()
	        const response = await sendToCEF({
	          action: 'setChatProvider',
	          payload: { chatId: id, providerId: requestedProviderId },
	          requestId,
	        })

	        if (response.ok) {
	          clearPendingRequest(requestKey, response.requestId)
	          return true
	        }

	        if (isLatestPendingRequest(requestKey, response.requestId)) {
	          set((state) => ({
	            sessions: state.sessions.map((s) => (s.id === id ? previousSession : s)),
	          }))
	          pendingRequestIdsByKey.delete(requestKey)
	        }

	        return false
	      }

	      applyProvider()
	      return true
	    },

	    setSessionModel: async (id, modelId) => {
	      const requestedModelId = modelId.trim()
	      if (!isAllowedAcpModelId(requestedModelId)) {
	        return false
	      }

      const previousSession = get().sessions.find((s) => s.id === id)
      if (!previousSession) {
        return false
      }

      if ((previousSession.modelId ?? '') === requestedModelId) {
        return true
      }

      const applyModel = () => {
        set((state) => ({
          sessions: state.sessions.map((s) =>
            s.id === id ? { ...s, modelId: requestedModelId, updatedAt: new Date() } : s
          ),
        }))
      }

      if (isCefContext()) {
        const requestKey = `setSessionModel:${id}`
        const requestId = createRequestId('setSessionModel')
        rememberPendingRequest(requestKey, requestId)
        applyModel()
        const response = await sendToCEF({
          action: 'setChatModel',
          payload: { chatId: id, modelId: requestedModelId },
          requestId,
        })

        if (response.ok) {
          clearPendingRequest(requestKey, response.requestId)
          return true
        }

        if (isLatestPendingRequest(requestKey, response.requestId)) {
          set((state) => ({
            sessions: state.sessions.map((s) => (s.id === id ? previousSession : s)),
          }))
          pendingRequestIdsByKey.delete(requestKey)
        }

        return false
      }

	      applyModel()
	      return true
	    },

	    setSessionApprovalMode: async (id, modeId) => {
	      const requestedModeId = modeId.trim() || 'default'
	      if (!(ACP_APPROVAL_MODE_IDS as readonly string[]).includes(requestedModeId)) {
	        return false
	      }

	      const previousSession = get().sessions.find((s) => s.id === id)
	      if (!previousSession) {
	        return false
	      }

	      if ((previousSession.approvalMode ?? 'default') === requestedModeId) {
	        return true
	      }

	      const applyMode = () => {
	        set((state) => ({
	          sessions: state.sessions.map((s) =>
	            s.id === id ? { ...s, approvalMode: requestedModeId, updatedAt: new Date() } : s
	          ),
	        }))
	      }

	      if (isCefContext()) {
	        const requestKey = `setSessionApprovalMode:${id}`
	        const requestId = createRequestId('setSessionApprovalMode')
	        rememberPendingRequest(requestKey, requestId)
	        applyMode()
	        const response = await sendToCEF({
	          action: 'setChatApprovalMode',
	          payload: { chatId: id, modeId: requestedModeId },
	          requestId,
	        })

	        if (response.ok) {
	          clearPendingRequest(requestKey, response.requestId)
	          return true
	        }

	        if (isLatestPendingRequest(requestKey, response.requestId)) {
	          set((state) => ({
	            sessions: state.sessions.map((s) => (s.id === id ? previousSession : s)),
	          }))
	          pendingRequestIdsByKey.delete(requestKey)
	        }

	        return false
	      }

	      applyMode()
	      return true
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
        const deletedAcpBinding = current.acpBindingBySessionId[id]
        const deletedTranscript = current.cliTranscriptBySessionId[id]
        const previousActiveSessionId = current.activeSessionId
        const requestKey = `deleteSession:${id}`
        const requestId = createRequestId('deleteSession')
        rememberPendingRequest(requestKey, requestId)
        set((state) => {
          const remaining = state.sessions.filter((s) => s.id !== id)
          const { [id]: _, ...msgs } = state.messages
          const { [id]: __, ...bindings } = state.cliBindingBySessionId
          const { [id]: ___, ...acpBindings } = state.acpBindingBySessionId
          const { [id]: ____, ...transcripts } = state.cliTranscriptBySessionId
          return {
            sessions: remaining,
            messages: msgs,
            cliBindingBySessionId: bindings,
            acpBindingBySessionId: acpBindings,
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
              acpBindingBySessionId: deletedAcpBinding
                ? { ...state.acpBindingBySessionId, [id]: deletedAcpBinding }
                : state.acpBindingBySessionId,
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
        const { [id]: ___, ...acpBindings } = state.acpBindingBySessionId
        const { [id]: ____, ...transcripts } = state.cliTranscriptBySessionId
        return {
          sessions: remaining,
          messages: msgs,
          cliBindingBySessionId: bindings,
          acpBindingBySessionId: acpBindings,
          cliTranscriptBySessionId: transcripts,
          activeSessionId:
            state.activeSessionId === id ? (remaining[0]?.id ?? null) : state.activeSessionId,
        }
      })
    },

    // ---- Folder actions ----

    addFolder: (name, _parentId, directory) => {
      if (isCefContext()) {
        return sendToCEF<CppFolder>({ action: 'createFolder', payload: { title: name, directory } }).then((resp) => {
          if (!resp.ok || !resp.data?.id) {
            if (!resp.ok) console.error('[CEF] createFolder failed:', resp.error)
            return false
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
          return true
        })
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
      return Promise.resolve(true)
    },

	    toggleFolder: (id) => {
	      if (isCefContext()) {
	        const currentFolder = get().folders.find((folder) => folder.id === id)
	        if (!currentFolder) {
	          return
	        }

	        const requestKey = `toggleFolder:${id}`
	        const requestId = createRequestId('toggleFolder')
	        rememberPendingRequest(requestKey, requestId)
	        const previousExpanded = currentFolder.isExpanded
	        set((state) => ({
	          folders: state.folders.map((f) =>
	            f.id === id ? { ...f, isExpanded: !f.isExpanded } : f
	          ),
	        }))
	        sendToCEF({ action: 'toggleFolder', payload: { folderId: id }, requestId }).then((resp) => {
	          if (resp.ok) {
	            clearPendingRequest(requestKey, resp.requestId)
	            return
	          }

	          if (!isLatestPendingRequest(requestKey, resp.requestId)) {
	            return
	          }

	          set((state) => ({
	            folders: state.folders.map((f) =>
	              f.id === id ? { ...f, isExpanded: previousExpanded } : f
	            ),
	          }))
	          pendingRequestIdsByKey.delete(requestKey)
	        })
	        return
	      }

	      set((state) => ({
	        folders: state.folders.map((f) =>
	          f.id === id ? { ...f, isExpanded: !f.isExpanded } : f
	        ),
	      }))
	    },

	    renameFolder: (id, name, directory) => {
	      if (isCefContext()) {
	        const previousFolder = get().folders.find((folder) => folder.id === id)
	        if (!previousFolder) {
	          return
	        }

	        const requestKey = `renameFolder:${id}`
	        const requestId = createRequestId('renameFolder')
	        rememberPendingRequest(requestKey, requestId)
	        set((state) => ({
	          folders: state.folders.map((folder) =>
	            folder.id === id ? { ...folder, name, directory } : folder
	          ),
	        }))
	        sendToCEF({ action: 'renameFolder', payload: { folderId: id, title: name, directory }, requestId }).then(
	          (resp) => {
	            if (resp.ok) {
	              clearPendingRequest(requestKey, resp.requestId)
	              return
	            }

	            if (!isLatestPendingRequest(requestKey, resp.requestId)) {
	              return
	            }

	            set((state) => ({
	              folders: state.folders.map((folder) => (folder.id === id ? previousFolder : folder)),
	            }))
	            pendingRequestIdsByKey.delete(requestKey)
	          }
	        )
	        return
	      }

      set((state) => ({
        folders: state.folders.map((f) => (f.id === id ? { ...f, name, directory } : f)),
      }))
    },

	    deleteFolder: (id) => {
	      if (isCefContext()) {
	        const deletedFolder = get().folders.find((folder) => folder.id === id)
	        if (!deletedFolder) {
	          return
	        }

	        const requestId = createRequestId('deleteFolder')
	        sendToCEF({ action: 'deleteFolder', payload: { folderId: id }, requestId }).then((resp) => {
	          if (!resp.ok) {
	            console.error('[CEF] deleteFolder failed:', resp.error)
	          }
	        })
	        return
	      }

      set((state) => {
        const deletedSessionIds = new Set(
          state.sessions.filter((session) => session.folderId === id).map((session) => session.id)
        )
        const remainingFolders = state.folders.filter((f) => f.id !== id)
        const sessions = state.sessions.filter((session) => !deletedSessionIds.has(session.id))
        const messages = { ...state.messages }
        const cliBindingBySessionId = { ...state.cliBindingBySessionId }
        const acpBindingBySessionId = { ...state.acpBindingBySessionId }
        const cliTranscriptBySessionId = { ...state.cliTranscriptBySessionId }

        deletedSessionIds.forEach((sessionId) => {
          delete messages[sessionId]
          delete cliBindingBySessionId[sessionId]
          delete acpBindingBySessionId[sessionId]
          delete cliTranscriptBySessionId[sessionId]
        })

        return {
          folders: remainingFolders,
          sessions,
          messages,
          cliBindingBySessionId,
          acpBindingBySessionId,
          cliTranscriptBySessionId,
          activeSessionId:
            state.activeSessionId !== null && deletedSessionIds.has(state.activeSessionId)
              ? (sessions[0]?.id ?? null)
              : state.activeSessionId,
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

    setCliBinding: (sessionId, binding) =>
      set((state) => {
        const existingBinding = state.cliBindingBySessionId[sessionId]
        const resolvedTerminalId = binding.terminalId ?? existingBinding?.terminalId ?? ''
        const running = binding.running ?? existingBinding?.running ?? false
        const turnState = binding.turnState ?? existingBinding?.turnState ?? 'idle'
        const processing = binding.processing ?? existingBinding?.processing ?? false
        const lifecycleState =
          binding.lifecycleState ??
          existingBinding?.lifecycleState ??
          normalizeCliLifecycleState(undefined, running, turnState, processing)
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
              running,
              lifecycleState,
              turnState: cliLifecycleIsProcessing(lifecycleState) ? 'busy' : turnState,
              processing: processing || cliLifecycleIsProcessing(lifecycleState),
              readySinceLastSelect: binding.readySinceLastSelect ?? existingBinding?.readySinceLastSelect ?? false,
              active: binding.active ?? existingBinding?.active ?? false,
              lastError: binding.lastError ?? existingBinding?.lastError ?? '',
            },
          },
          cliTranscriptBySessionId: nextTranscripts,
        }
      }),

    sendAcpPrompt: async (sessionId, text) => {
      const prompt = text.trim()
      if (!prompt) {
        return false
      }

      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'sendAcpPrompt',
          payload: { chatId: sessionId, text: prompt },
        })
        if (!response.ok) {
          set((state) => ({
            acpBindingBySessionId: {
              ...state.acpBindingBySessionId,
              [sessionId]: {
                ...(state.acpBindingBySessionId[sessionId] ?? {
                  sessionId: '',
                  providerId: state.sessions.find((session) => session.id === sessionId)?.providerId ?? GEMINI_CLI_PROVIDER_ID,
                  protocolKind: 'gemini-acp',
                  threadId: '',
                  running: false,
                  lifecycleState: 'error' as AcpLifecycleState,
                  processing: false,
                  readySinceLastSelect: false,
	                  processingStartedAtMs: null,
	                  lastError: '',
	                  recentStderr: '',
	                  lastExitCode: null,
	                  diagnostics: [],
	                  toolCalls: [],
                  planEntries: [],
                  availableModes: [],
                  currentModeId: 'default',
                  availableModels: [],
                  currentModelId: '',
                  turnEvents: [],
                  turnUserMessageIndex: -1,
                  turnAssistantMessageIndex: -1,
                  turnSerial: 0,
                  pendingPermission: null,
                  agentInfo: null,
                }),
                lifecycleState: 'error',
                processing: false,
                processingStartedAtMs: null,
                lastError: response.error ?? 'Failed to send ACP prompt.',
              },
            },
          }))
          return false
        }
        return true
      }

      const now = new Date()
      set((state) => ({
        messages: {
          ...state.messages,
          [sessionId]: [
            ...(state.messages[sessionId] ?? []),
            {
              id: `dev-user-${Date.now()}`,
              sessionId,
              role: 'user',
              content: prompt,
              createdAt: now,
            },
            {
              id: `dev-assistant-${Date.now()}`,
              sessionId,
              role: 'assistant',
              content: 'ACP dev mode response placeholder.',
              createdAt: now,
            },
          ],
        },
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: {
            sessionId: 'dev-acp-session',
            providerId: state.sessions.find((session) => session.id === sessionId)?.providerId ?? GEMINI_CLI_PROVIDER_ID,
            protocolKind: 'gemini-acp',
            threadId: '',
            running: true,
            lifecycleState: 'ready',
            processing: false,
            readySinceLastSelect: false,
	            processingStartedAtMs: null,
	            lastError: '',
	            recentStderr: '',
	            lastExitCode: null,
	            diagnostics: [],
	            toolCalls: [],
            planEntries: [],
            availableModes: [],
            currentModeId: state.sessions.find((session) => session.id === sessionId)?.approvalMode ?? 'default',
            availableModels: [],
            currentModelId: state.sessions.find((session) => session.id === sessionId)?.modelId ?? '',
            turnEvents: [],
            turnUserMessageIndex: -1,
            turnAssistantMessageIndex: -1,
            turnSerial: (state.acpBindingBySessionId[sessionId]?.turnSerial ?? 0) + 1,
            pendingPermission: null,
            agentInfo: { name: 'dev', title: 'Dev ACP', version: 'local' },
          },
        },
      }))
      return true
    },

    cancelAcpTurn: async (sessionId) => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'cancelAcpTurn',
          payload: { chatId: sessionId },
        })
        return response.ok
      }

      set((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: {
            ...(state.acpBindingBySessionId[sessionId] ?? {
              sessionId: 'dev-acp-session',
              running: true,
              lifecycleState: 'ready' as AcpLifecycleState,
              processing: false,
              readySinceLastSelect: false,
	              processingStartedAtMs: null,
	              lastError: '',
	              recentStderr: '',
	              lastExitCode: null,
	              diagnostics: [],
	              toolCalls: [],
              planEntries: [],
              availableModes: [],
              currentModeId: state.sessions.find((session) => session.id === sessionId)?.approvalMode ?? 'default',
              availableModels: [],
              currentModelId: state.sessions.find((session) => session.id === sessionId)?.modelId ?? '',
              turnEvents: [],
              turnUserMessageIndex: -1,
              turnAssistantMessageIndex: -1,
              turnSerial: 0,
              pendingPermission: null,
              agentInfo: null,
            }),
            lifecycleState: 'ready',
            processing: false,
            processingStartedAtMs: null,
            pendingPermission: null,
          },
        },
      }))
      return true
    },

    resolveAcpPermission: async (sessionId, requestId, optionId) => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'resolveAcpPermission',
          payload: {
            chatId: sessionId,
            requestId,
            optionId,
            cancelled: optionId === 'cancelled',
          },
        })
        return response.ok
      }

      set((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: {
            ...(state.acpBindingBySessionId[sessionId] ?? {
              sessionId: 'dev-acp-session',
              running: true,
              lifecycleState: 'ready' as AcpLifecycleState,
              processing: false,
              readySinceLastSelect: false,
	              processingStartedAtMs: null,
	              lastError: '',
	              recentStderr: '',
	              lastExitCode: null,
	              diagnostics: [],
	              toolCalls: [],
              planEntries: [],
              availableModes: [],
              currentModeId: state.sessions.find((session) => session.id === sessionId)?.approvalMode ?? 'default',
              availableModels: [],
              currentModelId: state.sessions.find((session) => session.id === sessionId)?.modelId ?? '',
              turnEvents: [],
              turnUserMessageIndex: -1,
              turnAssistantMessageIndex: -1,
              turnSerial: 0,
              pendingPermission: null,
              agentInfo: null,
            }),
            lifecycleState: 'processing',
            pendingPermission: null,
          },
        },
      }))
      return true
    },

    stopAcpSession: async (sessionId) => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'stopAcpSession',
          payload: { chatId: sessionId },
        })
        return response.ok
      }

      set((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: {
            ...(state.acpBindingBySessionId[sessionId] ?? {
              sessionId: '',
              running: false,
              lifecycleState: 'stopped' as AcpLifecycleState,
              processing: false,
              readySinceLastSelect: false,
	              processingStartedAtMs: null,
	              lastError: '',
	              recentStderr: '',
	              lastExitCode: null,
	              diagnostics: [],
	              toolCalls: [],
              planEntries: [],
              availableModes: [],
              currentModeId: state.sessions.find((session) => session.id === sessionId)?.approvalMode ?? 'default',
              availableModels: [],
              currentModelId: state.sessions.find((session) => session.id === sessionId)?.modelId ?? '',
              turnEvents: [],
              turnUserMessageIndex: -1,
              turnAssistantMessageIndex: -1,
              turnSerial: 0,
              pendingPermission: null,
              agentInfo: null,
            }),
            running: false,
            lifecycleState: 'stopped',
            processing: false,
            readySinceLastSelect: false,
            processingStartedAtMs: null,
            pendingPermission: null,
          },
        },
      }))
      return true
    },

    // ---- UI actions ----

    setTheme: (theme) => {
      const previousTheme = get().theme
      persistTheme(theme)
      set({ theme })
      if (isCefContext()) {
        const requestKey = 'setTheme'
        const requestId = createRequestId('setTheme')
        rememberPendingRequest(requestKey, requestId)
        sendToCEF({ action: 'setTheme', payload: { theme }, requestId }).then((resp) => {
          if (resp.ok) {
            clearPendingRequest(requestKey, resp.requestId)
            return
          }

          if (!isLatestPendingRequest(requestKey, resp.requestId)) {
            return
          }

          persistTheme(previousTheme)
          set({ theme: previousTheme })
          pendingRequestIdsByKey.delete(requestKey)
        })
      }
    },

    setNewChatModalOpen: (open, folderId) => set({
      isNewChatModalOpen: open,
      newChatFolderId: open ? (folderId ?? null) : null,
    }),
    setSettingsOpen: (open) => set({ isSettingsOpen: open }),
  }
})
