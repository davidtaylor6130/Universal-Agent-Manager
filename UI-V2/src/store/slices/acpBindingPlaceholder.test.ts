import { beforeEach, describe, expect, it, vi } from 'vitest'
import { createSessionsSlice } from './sessionsSlice'
import { isCefContext, sendToCEF } from '../../ipc/cefBridge'
import { DEFAULT_PROVIDER_ID, providerMetadataForId } from '../../utils/providerMetadata'
import type { AppState } from '../storeTypes'

vi.mock('../../ipc/cefBridge', async (original) => ({ ...await original<typeof import('../../ipc/cefBridge')>(), isCefContext: vi.fn(() => false), sendToCEF: vi.fn() }))

// Pins the binding each local action writes for a chat that has no binding yet, key for key.
function harness(bindings: Record<string, unknown> = {}) {
  const state = {
    sessions: [{ id: 'chat', providerId: 'codex-cli', approvalMode: 'plan', modelId: 'model-1', createdAt: new Date(0) }],
    acpBindingBySessionId: bindings,
    markdownStoreAttachedBySessionId: {},
    activeGoalIdByChatId: {},
    messages: {},
    historyStartIndexBySessionId: {},
  } as unknown as AppState
  const set = (update: Partial<AppState> | ((current: AppState) => Partial<AppState>)) => {
    Object.assign(state, typeof update === 'function' ? update(state) : update)
  }
  const actions = createSessionsSlice(set as never, () => state, false)
  return { actions, binding: () => state.acpBindingBySessionId.chat as unknown as Record<string, unknown> }
}

const idle = {
  processing: false,
  readySinceLastSelect: false,
  processingStartedAtMs: null,
  lastError: '',
  recentStderr: '',
  lastExitCode: null,
  diagnostics: [],
  toolCalls: [],
  planSummary: '',
  planEntries: [],
  availableModes: [],
  currentModeId: 'plan',
  availableModels: [],
  currentModelId: 'model-1',
  turnEvents: [],
  turnUserMessageIndex: -1,
  turnAssistantMessageIndex: -1,
  turnSerial: 0,
  waitIsStale: false,
  waitStaleReason: '',
  waitSeconds: 0,
  pendingPermission: null,
  pendingUserInput: null,
  agentInfo: null,
}
const structuredProtocol = providerMetadataForId(DEFAULT_PROVIDER_ID).structuredProtocol

describe('ACP binding placeholders', () => {
  beforeEach(() => {
    vi.mocked(isCefContext).mockReturnValue(false)
    vi.mocked(sendToCEF).mockReset()
  })

  it('cancelling a turn writes a ready development binding without provider fields', async () => {
    const { actions, binding } = harness()
    await actions.cancelAcpTurn('chat')
    expect(binding()).toStrictEqual({ ...idle, sessionId: 'dev-acp-session', running: true, lifecycleState: 'ready', attentionKind: null })
  })

  it('resolving a permission writes a processing development binding without provider fields', async () => {
    const { actions, binding } = harness()
    await actions.resolveAcpPermission('chat', 'request', 'allow_once')
    expect(binding()).toStrictEqual({ ...idle, sessionId: 'dev-acp-session', running: true, lifecycleState: 'processing', attentionKind: null })
  })

  it('resolving user input writes a processing development binding with provider fields', async () => {
    const { actions, binding } = harness()
    await actions.resolveAcpUserInput('chat', 'request', {})
    expect(binding()).toStrictEqual({
      ...idle, sessionId: 'dev-acp-session', providerId: 'codex-cli', protocolKind: structuredProtocol, threadId: '',
      running: true, lifecycleState: 'processing', processing: true, attentionKind: null,
    })
  })

  it('stopping a session writes a stopped binding without provider fields', async () => {
    const { actions, binding } = harness()
    await actions.stopAcpSession('chat')
    expect(binding()).toStrictEqual({ ...idle, sessionId: '', running: false, lifecycleState: 'stopped', attentionKind: null })
  })

  it('a browser-only send overwrites the binding but keeps its identity, serial and agent', async () => {
    const agentInfo = { name: 'agent' }
    const { actions, binding } = harness({ chat: { sessionId: 'native', threadId: 'thread', turnSerial: 7, agentInfo, queueHeld: true, lastError: 'old' } })
    await expect(actions.sendAcpPrompt('chat', 'hello')).resolves.toBe(false)
    expect(binding()).toStrictEqual({
      ...idle, sessionId: 'native', providerId: 'codex-cli', protocolKind: structuredProtocol, threadId: 'thread',
      running: false, lifecycleState: 'error', turnSerial: 7, agentInfo, queueHeld: true,
      promptActionError: { id: expect.any(String), message: 'Structured chat requires the desktop app.' },
    })
  })

  it.each([
    [false, 'error', structuredProtocol],
    [true, 'stopped', ''],
  ])('a desktop send failure (input recorded: %s) writes a neutral %s binding', async (inputRecorded, lifecycleState, protocolKind) => {
    vi.mocked(isCefContext).mockReturnValue(true)
    const parentMessage = { role: 'user', content: 'hello', createdAt: '2026-10-17T13:00:00.000Z' }
    vi.mocked(sendToCEF).mockResolvedValueOnce(inputRecorded
      ? { ok: true, data: { managedMention: { inputRecorded: true, delegationState: 'uncertain', parentMessageIndex: 0, parentMessage } }, requestId: 'r' }
      : { ok: false, error: 'Refused.', requestId: 'r' })
    const { actions, binding } = harness()
    await expect(actions.sendAcpPrompt('chat', 'hello')).resolves.toBe(inputRecorded)
    expect(binding()).toStrictEqual({
      ...idle, sessionId: '', providerId: 'codex-cli', protocolKind, threadId: '', running: false, lifecycleState,
      currentModeId: 'default', currentModelId: '',
      promptActionError: { id: expect.any(String), message: expect.any(String) },
    })
  })
})
