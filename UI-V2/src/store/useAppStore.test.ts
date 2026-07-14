import { beforeEach, describe, expect, it, vi } from 'vitest'
import { CppAppState, useAppStore } from './useAppStore'

type TestWindow = Window & typeof globalThis & {
  cefQuery?: Window['cefQuery']
}

function ensureTestWindow(): TestWindow {
  if (typeof window !== 'undefined') {
    return window as TestWindow
  }

  const testWindow = {} as TestWindow
  Object.defineProperty(globalThis, 'window', {
    value: testWindow,
    configurable: true,
  })
  return testWindow
}

function makeCppState(
  revision: number,
  selectedChatId = 'chat-1',
  terminal: Partial<NonNullable<CppAppState['chats'][number]['cliTerminal']>> = {}
): CppAppState {
  return {
    stateRevision: revision,
    folders: [
      {
        id: 'default',
        title: 'General',
        directory: '/tmp/project',
        collapsed: false,
      },
    ],
    resourceCollections: [],
    chats: [
      {
        id: 'chat-1',
        title: 'Gemini Session',
        folderId: 'default',
        providerId: 'gemini-cli',
        createdAt: '2026-01-01T00:00:00.000Z',
        updatedAt: '2026-01-01T00:00:01.000Z',
        messages: [],
        cliTerminal: {
          terminalId: 'term-chat-1',
          sourceChatId: 'chat-1',
          running: true,
          lifecycleState: 'idle',
          turnState: 'idle',
          processing: false,
          readySinceLastSelect: false,
          active: false,
          lastError: '',
          ...terminal,
        },
      },
    ],
    selectedChatId,
    providers: [
      { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', outputMode: 'cli' },
      { id: 'codex-cli', name: 'Codex', shortName: 'Codex', outputMode: 'cli' },
    ],
    settings: {
      activeProviderId: 'gemini-cli',
      theme: 'dark',
      memoryEnabledDefault: true,
      memoryIdleDelaySeconds: 60,
      memoryRecallBudgetBytes: 2048,
      goalMaxLoopIterations: 200,
      updateChecksEnabled: true,
      updateLastCheckedAt: '',
      dismissedUpdateVersions: {},
      memoryLastStatus: '',
      memoryWorkerBindings: {},
      defaultEditorPresetId: 'vscode',
      editorFileAssociations: [
        {
          id: 'cpp',
          name: 'C++',
          extensions: ['.cpp', '.h'],
          editorPresetId: 'clion',
        },
      ],
    },
    memoryActivity: {
      entryCount: 0,
      lastCreatedAt: '',
      lastCreatedCount: 0,
      runningCount: 0,
      lastStatus: '',
    },
  }
}

function resetStore() {
  useAppStore.setState({
    folders: [],
    sessions: [],
    activeSessionId: null,
    lastAppliedStateRevision: -1,
    messages: {},
    providers: [],
    cliBindingBySessionId: {},
    acpBindingBySessionId: {},
    cliTranscriptBySessionId: {},
    cliDebugState: null,
    memoryEnabledDefault: true,
    memoryIdleDelaySeconds: 60,
      memoryRecallBudgetBytes: 2048,
      goalMaxLoopIterations: 200,
      updateChecksEnabled: true,
      updateLastCheckedAt: '',
      dismissedUpdateVersions: {},
      memoryLastStatus: '',
    memoryWorkerBindings: {},
    defaultNewChatProviderId: 'gemini-cli',
    providerChatDefaults: {},
    defaultEditorPresetId: 'vscode',
    editorFileAssociations: [
      {
        id: 'cpp',
        name: 'C++',
        extensions: ['.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx'],
        editorPresetId: 'vscode',
      },
    ],
    shellActions: [],
    shellActionNotification: '',
    memoryActivity: {
      entryCount: 0,
      lastCreatedAt: '',
      lastCreatedCount: 0,
      runningCount: 0,
      lastStatus: '',
    },
    theme: 'dark',
    isNewChatModalOpen: false,
    isSettingsOpen: false,
    memoryLibraryScope: null,
    memoryLibraryEntries: [],
    memoryLibraryLoading: false,
    memoryLibraryError: '',
    isMemoryScanModalOpen: false,
    memoryScanCandidates: [],
    selectedMemoryScanChatIds: [],
    memoryScanLoading: false,
    memoryScanRunning: false,
    memoryScanError: '',
    streamingMessageId: null,
    sidebarCollapsed: false,
    commitPanelOpen: false,
    sidebarWidthPx: 320,
    commitPanelWidthPx: 420,
    cliVersionManager: { providers: [] },
    pushChannelStatus: 'connected',
    pushChannelError: '',
    lastPushAtMs: null,
  })
}

describe('useAppStore Gemini CLI slice', () => {
  beforeEach(() => {
    const testWindow = ensureTestWindow()
    resetStore()
    delete testWindow.cefQuery
    vi.restoreAllMocks()
  })

  it('simulates provider CLI install commands with provider-specific packages outside CEF', async () => {
    const store = useAppStore.getState()

    await expect(store.applyCliProviderVersion(' Claude-Code ', 'latest')).resolves.toBe(true)

    const claudeState = useAppStore.getState().cliVersionManager.providers.find((provider) => provider.providerId === 'claude-cli')
    expect(claudeState?.lastCommand).toBe('npm install -g @anthropic-ai/claude-code@latest')
    expect(claudeState?.lastOutput).toBe('Dev mode install simulated.')
  })

  it('deserializes backend state as ACP-first sessions and providers', () => {
    const cppState = makeCppState(1)
    cppState.chats[0].modelId = 'flash'
    cppState.chats[0].approvalMode = 'plan'
    cppState.chats[0].messages = [
      {
        role: 'assistant',
        content: 'Final answer',
        providerId: 'codex-cli',
        thoughts: 'Persisted backend thought',
        planSummary: 'Persisted plan summary',
        planEntries: [{ content: 'Persisted plan step', priority: '1', status: 'completed' }],
        toolCalls: [
          {
            id: 'persisted-tool-1',
            title: 'Saved read',
            kind: 'read',
            status: 'completed',
            content: 'Saved result',
          },
        ],
        createdAt: '2026-01-01T00:00:01.000Z',
      },
    ]
    cppState.chats[0].acpSession = {
      sessionId: 'native-1',
      running: true,
      lifecycleState: 'processing',
	      processing: true,
	      readySinceLastSelect: true,
	      attentionKind: 'memory',
	      lastError: '',
	      recentStderr: 'stderr tail',
	      lastExitCode: 137,
	      diagnostics: [
	        {
	          time: '2026-01-01T00:00:00.000Z',
	          event: 'response',
	          reason: 'jsonrpc_error',
	          method: 'session/prompt',
	          requestId: '42',
	          code: -32603,
	          message: 'Internal error',
	          detail: 'error.data={"cause":"boom"}',
	          lifecycleState: 'processing',
	        },
	      ],
	      toolCalls: [{ id: 'tool-1', title: 'Read file', kind: 'read', status: 'in_progress', content: '' }],
      planSummary: 'Live plan summary',
      planEntries: [{ content: 'Inspect project', priority: 'high', status: 'pending' }],
      availableModes: [
        { id: 'default', name: 'Default', description: 'Run normally' },
        { id: 'plan', name: 'Plan', description: 'Plan before editing' },
      ],
      currentModeId: 'plan',
      availableModels: [
        { id: 'auto-gemini-3', name: 'Auto 3', description: 'Gemini 3 routing' },
        { id: 'gemini-3-flash-preview', name: 'Gemini 3 Flash', description: 'Preview model' },
      ],
      currentModelId: 'gemini-3-flash-preview',
      turnEvents: [
        { type: 'assistant_text', text: 'Before tool.' },
        { type: 'thought', text: 'Live backend thought' },
        { type: 'tool_call', toolCallId: 'tool-1' },
      ],
      turnSerial: 3,
      pendingPermission: null,
    }
    useAppStore.getState().loadFromCef(cppState)

    const state = useAppStore.getState()
    expect(state.sessions).toHaveLength(1)
    expect(state.sessions[0]).toMatchObject({
      id: 'chat-1',
      name: 'Gemini Session',
      viewMode: 'chat',
      folderId: 'default',
      modelId: 'flash',
      approvalMode: 'plan',
    })
    expect(state.activeSessionId).toBe('chat-1')
    expect(state.providers.map((provider) => provider.id)).toEqual(['gemini-cli', 'codex-cli'])
    expect(state.defaultEditorPresetId).toBe('vscode')
    expect(state.editorFileAssociations[0]).toMatchObject({
      id: 'cpp',
      name: 'C++',
      extensions: ['.cpp', '.h'],
      editorPresetId: 'clion',
    })
    expect(state.cliBindingBySessionId['chat-1']).toMatchObject({
      terminalId: 'term-chat-1',
      running: true,
      lifecycleState: 'idle',
      turnState: 'idle',
    })
    expect(state.acpBindingBySessionId['chat-1']).toMatchObject({
      sessionId: 'native-1',
      running: true,
      lifecycleState: 'processing',
	      processing: true,
	      readySinceLastSelect: true,
	      attentionKind: 'memory',
	      turnSerial: 3,
	      recentStderr: 'stderr tail',
	      lastExitCode: 137,
	    })
	    expect(typeof state.acpBindingBySessionId['chat-1'].processingStartedAtMs).toBe('number')
	    expect(state.acpBindingBySessionId['chat-1'].toolCalls[0]).toMatchObject({ title: 'Read file' })
    expect(state.acpBindingBySessionId['chat-1'].planSummary).toBe('Live plan summary')
    expect(state.acpBindingBySessionId['chat-1'].planEntries[0]).toMatchObject({
      content: 'Inspect project',
      status: 'pending',
    })
	    expect(state.acpBindingBySessionId['chat-1'].availableModes.map((mode) => mode.id)).toEqual(['default', 'plan'])
	    expect(state.acpBindingBySessionId['chat-1'].currentModeId).toBe('plan')
	    expect(state.acpBindingBySessionId['chat-1'].availableModels.map((model) => model.id)).toEqual([
	      'auto-gemini-3',
	      'gemini-3-flash-preview',
	    ])
	    expect(state.acpBindingBySessionId['chat-1'].currentModelId).toBe('gemini-3-flash-preview')
	    expect(state.acpBindingBySessionId['chat-1'].diagnostics[0]).toMatchObject({
	      reason: 'jsonrpc_error',
	      method: 'session/prompt',
	      code: -32603,
	    })
    expect(state.messages['chat-1'][0]).toMatchObject({
      role: 'assistant',
      content: 'Final answer',
      providerId: 'codex-cli',
      thoughts: 'Persisted backend thought',
      planSummary: 'Persisted plan summary',
    })
    expect(state.messages['chat-1'][0].planEntries?.[0]).toMatchObject({
      content: 'Persisted plan step',
      status: 'completed',
    })
    expect(state.messages['chat-1'][0].toolCalls?.[0]).toMatchObject({
      id: 'persisted-tool-1',
      title: 'Saved read',
      status: 'completed',
    })
    expect(state.acpBindingBySessionId['chat-1'].turnEvents).toEqual([
      { type: 'assistant_text', text: 'Before tool.', toolCallId: undefined, requestId: undefined },
      { type: 'thought', text: 'Live backend thought', toolCallId: undefined, requestId: undefined },
      { type: 'tool_call', toolCallId: 'tool-1', text: undefined, requestId: undefined },
	    ])
	  })

  it('uses common editor groups when backend state has no editor associations', () => {
    const cppState = makeCppState(1)
    delete cppState.settings.editorFileAssociations

    useAppStore.getState().loadFromCef(cppState)

    const groupsById = new Map(useAppStore.getState().editorFileAssociations.map((group) => [group.id, group]))
    expect(groupsById.get('cpp')).toMatchObject({ name: 'C++', editorPresetId: 'clion' })
    expect(groupsById.get('csharp')).toMatchObject({ name: 'C#', editorPresetId: 'rider' })
    expect(groupsById.get('python')).toMatchObject({ name: 'Python', editorPresetId: 'pycharm' })
    expect(groupsById.get('javascript')).toMatchObject({ name: 'JavaScript', editorPresetId: 'webstorm' })
    expect(groupsById.get('react-typescript')).toMatchObject({ name: 'React / TypeScript', editorPresetId: 'webstorm' })
    expect(groupsById.get('rust')).toMatchObject({ name: 'Rust', editorPresetId: 'rustrover' })
    expect(groupsById.get('shell')?.extensions).toContain('.zsh')
  })

  it('updates ACP bindings when only turn serial changes and keeps the timer stable', () => {
    const firstState = makeCppState(1)
    firstState.chats[0].acpSession = {
      sessionId: 'native-1',
      running: true,
      lifecycleState: 'processing',
      processing: true,
      readySinceLastSelect: false,
      lastError: '',
      turnEvents: [{ type: 'assistant_text', text: 'First answer' }],
      turnUserMessageIndex: 0,
      turnAssistantMessageIndex: 1,
      turnSerial: 1,
      pendingPermission: null,
    }
    useAppStore.getState().loadFromCef(firstState)
    const firstBinding = useAppStore.getState().acpBindingBySessionId['chat-1']
    const firstStartedAt = firstBinding.processingStartedAtMs

    const secondState = makeCppState(2)
    secondState.chats[0].acpSession = {
      ...(firstState.chats[0].acpSession ?? {}),
      turnSerial: 2,
    }
    useAppStore.getState().loadFromCef(secondState)

    const secondBinding = useAppStore.getState().acpBindingBySessionId['chat-1']
    expect(secondBinding.turnSerial).toBe(2)
    expect(secondBinding.processingStartedAtMs).toBe(firstStartedAt)
  })

  it('preserves session identity when the backend model is unchanged', () => {
    const firstState = makeCppState(1)
    firstState.chats[0].modelId = 'pro'
    useAppStore.getState().loadFromCef(firstState)
    const firstSession = useAppStore.getState().sessions[0]

    const secondState = makeCppState(2)
    secondState.chats[0].modelId = 'pro'
    secondState.chats[0].acpSession = {
      sessionId: 'native-1',
      running: true,
      lifecycleState: 'ready',
      processing: false,
      readySinceLastSelect: true,
      pendingPermission: null,
    }
    useAppStore.getState().loadFromCef(secondState)

    expect(useAppStore.getState().sessions[0]).toBe(firstSession)
  })

  it('deserializes pinned backend sessions', () => {
    const state = makeCppState(1)
    state.chats[0].pinned = true

    useAppStore.getState().loadFromCef(state)

    expect(useAppStore.getState().sessions[0].isPinned).toBe(true)
  })

  it('refreshes persisted messages when only tool calls change', () => {
    const firstState = makeCppState(1)
    firstState.chats[0].messages = [
      {
        role: 'assistant',
        content: 'Final answer',
        thoughts: '',
        createdAt: '2026-01-01T00:00:01.000Z',
      },
    ]
    useAppStore.getState().loadFromCef(firstState)
    const firstMessage = useAppStore.getState().messages['chat-1'][0]

    const secondState = makeCppState(2)
    secondState.chats[0].messages = [
      {
        role: 'assistant',
        content: 'Final answer',
        thoughts: '',
        toolCalls: [
          {
            id: 'tool-1',
            title: 'Read file',
            kind: 'read',
            status: 'completed',
            content: 'file contents',
          },
        ],
        createdAt: '2026-01-01T00:00:01.000Z',
      },
    ]
    useAppStore.getState().loadFromCef(secondState)

    const updatedMessage = useAppStore.getState().messages['chat-1'][0]
    expect(updatedMessage).not.toBe(firstMessage)
    expect(updatedMessage.toolCalls?.[0]).toMatchObject({
      id: 'tool-1',
      title: 'Read file',
      content: 'file contents',
    })
  })

  it('sanitizes ACP attention kinds with safe fallbacks', () => {
    const cppState = makeCppState(1)
    cppState.chats[0].acpSession = {
      sessionId: 'native-1',
      running: true,
      lifecycleState: 'waitingUserInput',
      processing: true,
      readySinceLastSelect: false,
      attentionKind: 'unsupported' as never,
      pendingPermission: null,
      pendingUserInput: {
        requestId: 'input-1',
        itemId: 'item-1',
        status: 'pending',
        attentionKind: 'unsupported' as never,
        questions: [],
      },
    }

    useAppStore.getState().loadFromCef(cppState)

    const binding = useAppStore.getState().acpBindingBySessionId['chat-1']
    expect(binding.attentionKind).toBeNull()
    expect(binding.pendingUserInput?.attentionKind).toBe('question')
  })

  it('refreshes persisted messages when only plan fields change', () => {
    const firstState = makeCppState(1)
    firstState.chats[0].messages = [
      {
        role: 'assistant',
        content: '',
        thoughts: '',
        createdAt: '2026-01-01T00:00:01.000Z',
      },
    ]
    useAppStore.getState().loadFromCef(firstState)
    const firstMessage = useAppStore.getState().messages['chat-1'][0]

    const secondState = makeCppState(2)
    secondState.chats[0].messages = [
      {
        role: 'assistant',
        content: '',
        thoughts: '',
        planSummary: 'Plan summary',
        planEntries: [{ content: 'Patch Codex plan rendering', priority: '1', status: 'inProgress' }],
        createdAt: '2026-01-01T00:00:01.000Z',
      },
    ]
    useAppStore.getState().loadFromCef(secondState)

    const updatedMessage = useAppStore.getState().messages['chat-1'][0]
    expect(updatedMessage).not.toBe(firstMessage)
    expect(updatedMessage.planSummary).toBe('Plan summary')
    expect(updatedMessage.planEntries?.[0]).toMatchObject({
      content: 'Patch Codex plan rendering',
      status: 'inProgress',
    })
  })

  it('keeps backend providers when the backend omits Gemini CLI', () => {
    useAppStore.getState().loadFromCef({
      ...makeCppState(1),
      providers: [
        { id: 'codex-cli', name: 'Codex', shortName: 'Codex', outputMode: 'cli' },
        { id: 'claude-cli', name: 'Claude', shortName: 'Claude', outputMode: 'cli' },
      ],
    })

    expect(useAppStore.getState().providers.map((provider) => provider.id)).toEqual(['codex-cli', 'claude-cli'])
  })

  it('maps backend lifecycle states to CLI binding status', () => {
    useAppStore.getState().loadFromCef(makeCppState(1, 'chat-1', {
      lifecycleState: 'busy',
      turnState: 'busy',
      processing: true,
    }))

    expect(useAppStore.getState().cliBindingBySessionId['chat-1']).toMatchObject({
      lifecycleState: 'busy',
      turnState: 'busy',
      processing: true,
    })

    useAppStore.getState().loadFromCef(makeCppState(2, 'chat-1', {
      running: false,
      lifecycleState: 'disabled',
      turnState: 'idle',
      processing: false,
    }))

    expect(useAppStore.getState().cliBindingBySessionId['chat-1']).toMatchObject({
      running: false,
      lifecycleState: 'disabled',
      turnState: 'idle',
      processing: false,
    })
  })

  it('appends CLI output without forcing the session busy', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    testWindow.cefQuery = ({ onSuccess }) => {
      onSuccess(JSON.stringify(makeCppState(1)))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))

    cefStore.getState().loadFromCef(makeCppState(2))
    expect(cefStore.getState().cliBindingBySessionId['chat-1']).toMatchObject({
      lifecycleState: 'idle',
      turnState: 'idle',
      processing: false,
    })

    testWindow.uamPush?.({
      type: 'cliOutput',
      sessionId: 'chat-1',
      sourceChatId: 'chat-1',
      terminalId: 'term-chat-1',
      data: btoa('hello'),
    })
    await new Promise((resolve) => setTimeout(resolve, 120))

    const state = cefStore.getState()
    expect(state.cliTranscriptBySessionId['chat-1']?.content).toBe('hello')
    expect(state.cliBindingBySessionId['chat-1']).toMatchObject({
      lifecycleState: 'idle',
      turnState: 'idle',
      processing: false,
    })
  })

  it('merges statePatch updates without dropping existing messages', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    testWindow.cefQuery = ({ onSuccess }) => {
      const initialState = makeCppState(1)
      initialState.chats[0].messages = [
        { role: 'user', content: 'keep me', createdAt: '2026-01-01T00:00:00.000Z' },
      ]
      onSuccess(JSON.stringify(initialState))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))

    testWindow.uamPush?.({
      type: 'statePatch',
      data: {
        stateRevision: 2,
        chats: [
          {
            id: 'chat-1',
            title: 'Patched Session',
            folderId: 'default',
            providerId: 'gemini-cli',
            createdAt: '2026-01-01T00:00:00.000Z',
            updatedAt: '2026-01-01T00:00:02.000Z',
            messageCount: 1,
            messagesDigest: 'digest-1',
            cliTerminal: {
              terminalId: 'term-chat-1',
              sourceChatId: 'chat-1',
              running: true,
              lifecycleState: 'busy',
              turnState: 'busy',
              processing: true,
              readySinceLastSelect: false,
              active: false,
              lastError: '',
            },
          },
        ],
      },
    })

    let state = cefStore.getState()
    expect(state.lastAppliedStateRevision).toBe(2)
    expect(state.sessions[0].name).toBe('Patched Session')
    expect(state.messages['chat-1'].map((message) => message.content)).toEqual(['keep me'])
    expect(state.cliBindingBySessionId['chat-1']).toMatchObject({ lifecycleState: 'busy', processing: true })

    testWindow.uamPush?.({
      type: 'statePatch',
      data: {
        stateRevision: 3,
        messagesByChatId: {
          'chat-1': [
            { role: 'assistant', content: 'replacement', createdAt: '2026-01-01T00:00:03.000Z' },
          ],
        },
      },
    })

    state = cefStore.getState()
    expect(state.messages['chat-1'].map((message) => message.content)).toEqual(['replacement'])

    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: '? duplicated hint' })
    await new Promise((resolve) => setTimeout(resolve, 80))
    expect(cefStore.getState().messages['chat-1'].map((message) => message.content)).toEqual(['replacement'])

    testWindow.uamPush?.({
      type: 'statePatch',
      data: {
        stateRevision: 4,
        removedChatIds: ['chat-1'],
      },
    })

    state = cefStore.getState()
    expect(state.sessions).toEqual([])
    expect(state.messages['chat-1']).toBeUndefined()
    expect(state.cliBindingBySessionId['chat-1']).toBeUndefined()
  })

  it('applies selected chat patches with hydrated messages and chat order', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    testWindow.cefQuery = ({ onSuccess }) => {
      const initialState = makeCppState(1)
      initialState.chats[0].messages = [
        { role: 'user', content: 'first chat message', createdAt: '2026-01-01T00:00:00.000Z' },
      ]
      initialState.chats.push({
        id: 'chat-2',
        title: 'Second Chat',
        folderId: 'default',
        providerId: 'gemini-cli',
        createdAt: '2026-01-01T00:00:00.000Z',
        updatedAt: '2026-01-01T00:00:01.000Z',
        messageCount: 1,
        messagesDigest: 'chat-2-digest',
      })
      onSuccess(JSON.stringify(initialState))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))

    testWindow.uamPush?.({
      type: 'statePatch',
      data: {
        stateRevision: 2,
        selectedChatId: 'chat-2',
        chatOrder: ['chat-2', 'chat-1'],
        chats: [
          {
            id: 'chat-2',
            title: 'Second Chat',
            folderId: 'default',
            providerId: 'gemini-cli',
            createdAt: '2026-01-01T00:00:00.000Z',
            updatedAt: '2026-01-01T00:00:02.000Z',
            lastOpenedAt: '2026-01-01T00:00:03.000Z',
            messageCount: 1,
            messagesDigest: 'chat-2-digest',
          },
        ],
        messagesByChatId: {
          'chat-2': [
            { role: 'assistant', content: 'hydrated second chat', createdAt: '2026-01-01T00:00:04.000Z' },
          ],
        },
      },
    })

    const state = cefStore.getState()
    expect(state.activeSessionId).toBe('chat-2')
    expect(state.sessions.map((session) => session.id)).toEqual(['chat-2', 'chat-1'])
    expect(state.messages['chat-1'].map((message) => message.content)).toEqual(['first chat message'])
    expect(state.messages['chat-2'].map((message) => message.content)).toEqual(['hydrated second chat'])
  })

  it('applies pin patches without changing the active chat', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    testWindow.cefQuery = ({ onSuccess }) => {
      const initialState = makeCppState(1, 'chat-2')
      initialState.chats.push({
        id: 'chat-2',
        title: 'Active Chat',
        folderId: 'default',
        providerId: 'gemini-cli',
        createdAt: '2026-01-01T00:00:00.000Z',
        updatedAt: '2026-01-01T00:00:01.000Z',
        messages: [],
      })
      onSuccess(JSON.stringify(initialState))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))

    testWindow.uamPush?.({
      type: 'statePatch',
      data: {
        stateRevision: 2,
        chats: [
          {
            id: 'chat-1',
            title: 'Gemini Session',
            folderId: 'default',
            pinned: true,
            providerId: 'gemini-cli',
            createdAt: '2026-01-01T00:00:00.000Z',
            updatedAt: '2026-01-01T00:00:01.000Z',
            messageCount: 0,
            messagesDigest: 'chat-1-digest',
          },
        ],
        chatOrder: ['chat-1', 'chat-2'],
      },
    })

    const state = cefStore.getState()
    expect(state.activeSessionId).toBe('chat-2')
    expect(state.sessions.find((session) => session.id === 'chat-1')?.isPinned).toBe(true)
    expect(state.sessions.map((session) => session.id)).toEqual(['chat-1', 'chat-2'])
  })

  it('ignores stale no-op pin patches without replacing session state', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    testWindow.cefQuery = ({ onSuccess }) => {
      const initialState = makeCppState(2)
      initialState.chats[0].pinned = true
      onSuccess(JSON.stringify(initialState))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const previousSessions = cefStore.getState().sessions

    testWindow.uamPush?.({
      type: 'statePatch',
      data: {
        stateRevision: 2,
        chats: [
          {
            id: 'chat-1',
            title: 'Gemini Session',
            folderId: 'default',
            pinned: true,
            providerId: 'gemini-cli',
            createdAt: '2026-01-01T00:00:00.000Z',
            updatedAt: '2026-01-01T00:00:01.000Z',
            messageCount: 0,
            messagesDigest: 'chat-1-digest',
          },
        ],
      },
    })

    expect(cefStore.getState().sessions).toBe(previousSessions)
    expect(cefStore.getState().activeSessionId).toBe('chat-1')
    expect(cefStore.getState().lastAppliedStateRevision).toBe(2)
  })

  it('applies permission resolution patches without requiring a full state update', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    testWindow.cefQuery = ({ onSuccess }) => {
      const initialState = makeCppState(1)
      initialState.chats[0].messages = [
        { role: 'assistant', content: 'approval needed', createdAt: '2026-01-01T00:00:00.000Z' },
      ]
      initialState.chats[0].acpSession = {
        sessionId: 'acp-chat-1',
        running: true,
        processing: true,
        lifecycleState: 'waitingPermission',
        attentionKind: 'command',
        pendingPermission: {
          requestId: 'req-1',
          toolCallId: 'tool-1',
          title: 'Command approval',
          kind: 'command',
          status: 'pending',
          content: 'Run command?',
          options: [
            { id: 'accept', name: 'Allow', kind: 'decision' },
            { id: 'decline', name: 'Deny', kind: 'decision' },
          ],
        },
      }
      onSuccess(JSON.stringify(initialState))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    expect(cefStore.getState().acpBindingBySessionId['chat-1'].pendingPermission?.requestId).toBe('req-1')

    testWindow.uamPush?.({
      type: 'statePatch',
      data: {
        stateRevision: 2,
        chats: [
          {
            id: 'chat-1',
            title: 'Gemini Session',
            folderId: 'default',
            providerId: 'gemini-cli',
            createdAt: '2026-01-01T00:00:00.000Z',
            updatedAt: '2026-01-01T00:00:01.000Z',
            messageCount: 1,
            messagesDigest: 'chat-1-digest',
            acpSession: {
              sessionId: 'acp-chat-1',
              running: true,
              processing: true,
              lifecycleState: 'processing',
              attentionKind: null,
              pendingPermission: null,
            },
          },
        ],
      },
    })

    const state = cefStore.getState()
    expect(state.activeSessionId).toBe('chat-1')
    expect(state.messages['chat-1'].map((message) => message.content)).toEqual(['approval needed'])
    expect(state.acpBindingBySessionId['chat-1']).toMatchObject({
      lifecycleState: 'processing',
      attentionKind: null,
      pendingPermission: null,
    })
  })

  it('ignores stale backend revisions', () => {
    useAppStore.getState().loadFromCef(makeCppState(2))
    useAppStore.getState().loadFromCef({
      ...makeCppState(1),
      chats: [{ ...makeCppState(1).chats[0], title: 'Stale' }],
    })

    expect(useAppStore.getState().sessions[0].name).toBe('Gemini Session')
    expect(useAppStore.getState().lastAppliedStateRevision).toBe(2)
  })

  it('creates CEF sessions with the Gemini CLI provider', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }

    useAppStore.setState({
      folders: [{ id: 'default', name: 'General', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: new Date() }],
      providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#f97316', description: '' }],
    })

    useAppStore.getState().addSession('New Chat', 'default')
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(requests).toHaveLength(1)
    expect(requests[0].action).toBe('createSession')
    expect(requests[0].payload).toEqual({
      title: 'New Chat',
      folderId: 'default',
      providerId: 'gemini-cli',
      defaults: {
        modelId: '',
        approvalMode: 'default',
        autoApproveCommands: false,
        memoryEnabled: true,
        reasoningEffort: '',
        serviceTier: '',
      },
    })
  })

  it('creates edited and reverted message branches through CEF', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess(JSON.stringify({ chatId: 'branch-1' }))
    }

    await expect(useAppStore.getState().branchFromMessage('chat-1', 2, 'Edited prompt')).resolves.toBe('branch-1')
    await expect(useAppStore.getState().branchFromMessage('chat-1', 0)).resolves.toBe('branch-1')
    expect(requests.map(({ action, payload }) => ({ action, payload }))).toEqual([
      { action: 'branchFromMessage', payload: { chatId: 'chat-1', messageIndex: 2, content: 'Edited prompt' } },
      { action: 'branchFromMessage', payload: { chatId: 'chat-1', messageIndex: 0 } },
    ])
  })

  it('keeps message branch metadata from native state', () => {
    const state = makeCppState(3)
    state.chats[0] = {
      ...state.chats[0],
      parentChatId: 'chat-root',
      branchFromMessageIndex: 2,
      branchMessageEdited: true,
    }
    useAppStore.getState().loadFromCef(state)
    expect(useAppStore.getState().sessions[0]).toMatchObject({
      parentChatId: 'chat-root',
      branchFromMessageIndex: 2,
      branchMessageEdited: true,
    })
  })

  it('creates CEF sessions with the selected Codex provider', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }

    useAppStore.setState({
      folders: [{ id: 'default', name: 'General', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: new Date() }],
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#f97316', description: '' },
        { id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', color: '#22c55e', description: '' },
      ],
    })

    useAppStore.getState().addSession('Codex Chat', 'default', ' CoDeX ', 'gpt-5.4')
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(requests).toHaveLength(1)
    expect(requests[0].action).toBe('createSession')
    expect(requests[0].payload).toEqual({
      title: 'Codex Chat',
      folderId: 'default',
      providerId: 'codex-cli',
      defaults: {
        modelId: 'gpt-5.4',
        approvalMode: 'default',
        autoApproveCommands: false,
        memoryEnabled: true,
        reasoningEffort: '',
        serviceTier: '',
      },
    })
  })

  it('applies provider chat defaults when creating CEF and local sessions', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }

    useAppStore.setState({
      folders: [{ id: 'default', name: 'General', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: new Date() }],
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#f97316', description: '' },
        { id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', color: '#22c55e', description: '' },
      ],
      providerChatDefaults: {
        'codex-cli': {
          modelId: 'gpt-5.4',
          approvalMode: 'plan',
          autoApproveCommands: true,
          memoryEnabled: false,
          reasoningEffort: 'high',
          serviceTier: 'fast',
        },
      },
    })

    useAppStore.getState().addSession('Codex Defaults', 'default', 'codex-cli')
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(requests[0].payload).toMatchObject({
      title: 'Codex Defaults',
      folderId: 'default',
      providerId: 'codex-cli',
      defaults: {
        modelId: 'gpt-5.4',
        approvalMode: 'plan',
        autoApproveCommands: true,
        memoryEnabled: false,
        reasoningEffort: 'high',
        serviceTier: 'fast',
      },
    })

    delete window.cefQuery
    useAppStore.setState({
      sessions: [],
      activeSessionId: null,
    })

    useAppStore.getState().addSession('Local Codex Defaults', 'default', 'codex-cli')

    const created = useAppStore.getState().sessions[0]
    expect(created).toMatchObject({
      name: 'Local Codex Defaults',
      providerId: 'codex-cli',
      modelId: 'gpt-5.4',
      approvalMode: 'plan',
      autoApproveCommands: true,
      memoryEnabled: false,
      reasoningEffort: 'high',
      serviceTier: 'fast',
    })
  })

  it('opens a session workspace through CEF', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }

    await expect(useAppStore.getState().openSessionWorkspace('chat-1')).resolves.toBe(true)

    expect(requests).toHaveLength(1)
    expect(requests[0].action).toBe('openWorkspaceDirectory')
    expect(requests[0].payload).toEqual({ chatId: 'chat-1' })
  })

  it('dispatches worktree actions without changing the active chat locally', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess(JSON.stringify({
        message: 'Created isolated Git worktree.',
        patchPath: '',
        status: {
          isGitRepository: true,
          isSvnWorkspace: false,
          isolated: true,
          sourceDirty: false,
          worktreeDirty: false,
          worktreeMissing: false,
          sourceDirectory: '/tmp/project',
          worktreeDirectory: '/tmp/uam-worktree',
          branchName: 'uam/chat-1',
          baseRef: 'abc123',
          warning: '',
          error: '',
        },
      }))
    }
    useAppStore.setState({ activeSessionId: 'chat-1' })

    await expect(useAppStore.getState().createChatWorktree('chat-1')).resolves.toMatchObject({
      ok: true,
      message: 'Created isolated Git worktree.',
    })

    expect(requests).toHaveLength(1)
    expect(requests[0].action).toBe('createChatWorktree')
    expect(requests[0].payload).toEqual({ chatId: 'chat-1' })
    expect(useAppStore.getState().activeSessionId).toBe('chat-1')
  })

  it('returns false when CEF fails to open a session workspace', async () => {
    const consoleSpy = vi.spyOn(console, 'error').mockImplementation(() => {})
    window.cefQuery = ({ onFailure }) => {
      onFailure(404, 'Workspace directory does not exist.')
    }

    await expect(useAppStore.getState().openSessionWorkspace('missing')).resolves.toBe(false)

    consoleSpy.mockRestore()
  })

  it('does not create sessions without a valid folder', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    const consoleSpy = vi.spyOn(console, 'error').mockImplementation(() => {})
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }

    useAppStore.setState({
      folders: [{ id: 'project', name: 'Project', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: new Date() }],
      providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#f97316', description: '' }],
    })

    useAppStore.getState().addSession('New Chat', null)
    useAppStore.getState().addSession('New Chat', 'missing')
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(requests).toEqual([])
    consoleSpy.mockRestore()
  })

  it('pins CEF sessions optimistically and rolls back when rejected', async () => {
    const now = new Date()
    const requests: Array<{ action: string; payload?: unknown }> = []
    let rejectPin: () => void = () => {
      throw new Error('CEF pin request was not sent')
    }
    window.cefQuery = ({ request, onFailure }) => {
      requests.push(JSON.parse(request))
      rejectPin = () => onFailure(500, 'save failed')
    }

    useAppStore.setState({
      sessions: [
        { id: 'chat-1', name: 'Chat 1', viewMode: 'chat', folderId: 'project', isPinned: false, createdAt: now, updatedAt: now },
      ],
    })

    const resultPromise = useAppStore.getState().setSessionPinned('chat-1', true)
    expect(useAppStore.getState().sessions[0].isPinned).toBe(true)
    rejectPin()

    await expect(resultPromise).resolves.toBe(false)
    expect(requests).toHaveLength(1)
    expect(requests[0].action).toBe('setChatPinned')
    expect(requests[0].payload).toEqual({ chatId: 'chat-1', pinned: true })
    expect(useAppStore.getState().sessions[0].isPinned).toBe(false)
  })

  it('changes providers while preserving existing message history', async () => {
    const now = new Date()
    useAppStore.setState({
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#f97316', description: '' },
        { id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', color: '#22c55e', description: '' },
      ],
      sessions: [
        {
          id: 'chat-1',
          name: 'Empty Session',
          viewMode: 'chat',
          folderId: null,
          providerId: 'gemini-cli',
          createdAt: now,
          updatedAt: now,
        },
      ],
      messages: { 'chat-1': [] },
      acpBindingBySessionId: {},
    })

    await expect(useAppStore.getState().setSessionProvider('chat-1', ' CoDeX ')).resolves.toBe(true)
    expect(useAppStore.getState().sessions[0].providerId).toBe('codex-cli')

    useAppStore.setState({
      messages: {
        'chat-1': [
          { id: 'm-1', sessionId: 'chat-1', role: 'user', content: 'hello', createdAt: now },
        ],
      },
    })
    await expect(useAppStore.getState().setSessionProvider('chat-1', 'gemini-cli')).resolves.toBe(true)
    expect(useAppStore.getState().sessions[0].providerId).toBe('gemini-cli')
    expect(useAppStore.getState().messages['chat-1']).toHaveLength(1)
  })

  it('allows an idle runtime provider switch but rejects active turns and waits', async () => {
    const cppState = makeCppState(1)
    cppState.chats[0].acpSession = {
      sessionId: 'old-native-session',
      running: true,
      lifecycleState: 'ready',
      processing: false,
      readySinceLastSelect: true,
      lastError: '',
      pendingPermission: null,
    }
    useAppStore.getState().loadFromCef(cppState)

    await expect(useAppStore.getState().setSessionProvider('chat-1', 'codex-cli')).resolves.toBe(true)
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => ({ ...session, providerId: 'gemini-cli' })),
      acpBindingBySessionId: {
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: true },
      },
    }))
    await expect(useAppStore.getState().setSessionProvider('chat-1', 'codex-cli')).resolves.toBe(false)

    useAppStore.setState({
      acpBindingBySessionId: {
        'chat-1': { ...useAppStore.getState().acpBindingBySessionId['chat-1'], processing: false, pendingUserInput: { requestId: 'wait-1', itemId: 'item-1', status: 'pending', questions: [] } },
      },
    })
    await expect(useAppStore.getState().setSessionProvider('chat-1', 'codex-cli')).resolves.toBe(false)
  })

  it('rejects switching a session to an unavailable provider in a Gemini-only provider list', async () => {
    const now = new Date()
    useAppStore.setState({
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#f97316', description: '' },
      ],
      sessions: [
        {
          id: 'chat-1',
          name: 'Gemini Session',
          viewMode: 'chat',
          folderId: null,
          providerId: 'gemini-cli',
          createdAt: now,
          updatedAt: now,
        },
      ],
      messages: { 'chat-1': [] },
      acpBindingBySessionId: {},
    })

    await expect(useAppStore.getState().setSessionProvider('chat-1', 'codex-cli')).resolves.toBe(false)
    expect(useAppStore.getState().sessions[0].providerId).toBe('gemini-cli')
  })

  it('updates local session model state in dev mode', async () => {
    const now = new Date()
    useAppStore.setState({
      sessions: [
        {
          id: 'chat-1',
          name: 'Gemini Session',
          viewMode: 'chat',
          folderId: 'default',
          modelId: '',
          createdAt: now,
          updatedAt: now,
        },
      ],
    })

    await expect(useAppStore.getState().setSessionModel('chat-1', 'flash')).resolves.toBe(true)
    expect(useAppStore.getState().sessions[0].modelId).toBe('flash')
    await expect(useAppStore.getState().setSessionModel('chat-1', 'models/gemini-3-pro-preview')).resolves.toBe(true)
    expect(useAppStore.getState().sessions[0].modelId).toBe('models/gemini-3-pro-preview')
    await expect(useAppStore.getState().setSessionModel('chat-1', 'bad model')).resolves.toBe(false)
    await expect(useAppStore.getState().setSessionModel('chat-1', '-bad')).resolves.toBe(false)
    expect(useAppStore.getState().sessions[0].modelId).toBe('models/gemini-3-pro-preview')
  })

  it('sends selected model changes through CEF', async () => {
    const now = new Date()
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }
    useAppStore.setState({
      sessions: [
        {
          id: 'chat-1',
          name: 'Gemini Session',
          viewMode: 'chat',
          folderId: 'default',
          modelId: '',
          createdAt: now,
          updatedAt: now,
        },
      ],
    })

    await expect(useAppStore.getState().setSessionModel('chat-1', 'auto-gemini-3')).resolves.toBe(true)

    expect(requests).toHaveLength(1)
    expect(requests[0].action).toBe('setChatModel')
    expect(requests[0].payload).toEqual({ chatId: 'chat-1', modelId: 'auto-gemini-3' })
    expect(useAppStore.getState().sessions[0].modelId).toBe('auto-gemini-3')
  })

  it('sends Codex reasoning and speed changes through CEF and rolls back on failure', async () => {
    const now = new Date()
    const requests: Array<{ action: string; payload?: unknown }> = []
    let rejectNext = false
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      requests.push(JSON.parse(request))
      if (rejectNext) {
        onFailure(409, 'Codex is busy')
        return
      }
      onSuccess('{}')
    }
    useAppStore.setState({
      sessions: [
        {
          id: 'chat-1',
          name: 'Codex Session',
          viewMode: 'chat',
          folderId: 'default',
          providerId: 'codex-cli',
          reasoningEffort: 'medium',
          serviceTier: 'flex',
          createdAt: now,
          updatedAt: now,
        },
      ],
    })

    await expect(useAppStore.getState().setSessionCodexOptions('chat-1', {
      reasoningEffort: 'high',
      serviceTier: 'fast',
    })).resolves.toBe(true)

    expect(requests[0].action).toBe('setChatCodexOptions')
    expect(requests[0].payload).toEqual({ chatId: 'chat-1', reasoningEffort: 'high', serviceTier: 'fast' })
    expect(useAppStore.getState().sessions[0].reasoningEffort).toBe('high')
    expect(useAppStore.getState().sessions[0].serviceTier).toBe('fast')

    rejectNext = true
    await expect(useAppStore.getState().setSessionCodexOptions('chat-1', {
      reasoningEffort: 'low',
      serviceTier: 'flex',
    })).resolves.toBe(false)

    expect(requests[1].payload).toEqual({ chatId: 'chat-1', reasoningEffort: 'low', serviceTier: 'flex' })
    expect(useAppStore.getState().sessions[0].reasoningEffort).toBe('high')
    expect(useAppStore.getState().sessions[0].serviceTier).toBe('fast')
  })

  it('keeps pending Codex reasoning and speed when stale backend state arrives before CEF success', async () => {
    const now = new Date()
    const cefSuccess: { current: ((response: string) => void) | null } = { current: null }
    window.cefQuery = ({ onSuccess }) => {
      cefSuccess.current = onSuccess
    }
    useAppStore.setState({
      sessions: [
        {
          id: 'chat-1',
          name: 'Codex Session',
          viewMode: 'chat',
          folderId: 'default',
          providerId: 'codex-cli',
          reasoningEffort: 'medium',
          serviceTier: 'flex',
          createdAt: now,
          updatedAt: now,
        },
      ],
      lastAppliedStateRevision: -1,
    })

    const change = useAppStore.getState().setSessionCodexOptions('chat-1', {
      reasoningEffort: 'high',
      serviceTier: 'fast',
    })

    expect(useAppStore.getState().sessions[0].reasoningEffort).toBe('high')
    expect(useAppStore.getState().sessions[0].serviceTier).toBe('fast')

    const staleState = makeCppState(1)
    staleState.chats[0].providerId = 'codex-cli'
    staleState.chats[0].reasoningEffort = 'medium'
    staleState.chats[0].serviceTier = 'flex'
    useAppStore.getState().loadFromCef(staleState)

    expect(useAppStore.getState().sessions[0].reasoningEffort).toBe('high')
    expect(useAppStore.getState().sessions[0].serviceTier).toBe('fast')

    expect(cefSuccess.current).toBeTruthy()
    cefSuccess.current?.('{}')
    await expect(change).resolves.toBe(true)
  })

  it('sends provider chat defaults through CEF and rolls back on failure', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    let rejectNext = false
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      requests.push(JSON.parse(request))
      if (rejectNext) {
        onFailure(500, 'Failed to save settings')
        return
      }
      onSuccess('{}')
    }
    useAppStore.setState({
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#f97316', description: '' },
        { id: 'codex-cli', name: 'Codex', shortName: 'Codex', color: '#0ea5e9', description: '' },
      ],
      defaultNewChatProviderId: 'gemini-cli',
      providerChatDefaults: {},
    })

    await expect(useAppStore.getState().setProviderChatDefaults({
      defaultNewChatProviderId: ' codex ',
      providerChatDefaults: {
        ' CoDeX ': {
          modelId: 'gpt-5.2',
          approvalMode: 'acceptEdits',
          autoApproveCommands: true,
          memoryEnabled: false,
          reasoningEffort: 'xhigh',
          serviceTier: 'fast',
        },
        'gemini-cli': {
          modelId: 'gemini-3-pro-preview',
          approvalMode: 'plan',
          autoApproveCommands: false,
          memoryEnabled: true,
          reasoningEffort: 'high',
          serviceTier: 'fast',
        },
      },
    })).resolves.toBe(true)

    const expectedDefaults = {
      'codex-cli': {
        modelId: 'gpt-5.2',
        approvalMode: 'acceptEdits',
        autoApproveCommands: true,
        memoryEnabled: false,
        reasoningEffort: 'xhigh',
        serviceTier: 'fast',
      },
      'gemini-cli': {
        modelId: 'gemini-3-pro-preview',
        approvalMode: 'plan',
        autoApproveCommands: false,
        memoryEnabled: true,
        reasoningEffort: '',
        serviceTier: '',
      },
    }

    expect(requests[0].action).toBe('setProviderChatDefaults')
    expect(requests[0].payload).toEqual({
      defaultProviderId: 'codex-cli',
      defaults: expectedDefaults,
    })
    expect(useAppStore.getState().defaultNewChatProviderId).toBe('codex-cli')
    expect(useAppStore.getState().providerChatDefaults).toEqual(expectedDefaults)

    rejectNext = true
    await expect(useAppStore.getState().setProviderChatDefaults({
      defaultNewChatProviderId: 'gemini-cli',
      providerChatDefaults: {
        'codex-cli': {
          modelId: 'gpt-5.3',
          approvalMode: 'default',
          autoApproveCommands: false,
          memoryEnabled: true,
          reasoningEffort: 'low',
          serviceTier: 'flex',
        },
      },
    })).resolves.toBe(false)

    expect(requests[1].payload).toMatchObject({ defaultProviderId: 'gemini-cli' })
    expect(useAppStore.getState().defaultNewChatProviderId).toBe('codex-cli')
    expect(useAppStore.getState().providerChatDefaults).toEqual(expectedDefaults)

    await expect(useAppStore.getState().setProviderChatDefaults({
      defaultNewChatProviderId: 'missing-provider',
    })).resolves.toBe(false)
    expect(requests).toHaveLength(2)
  })

  it('keeps pending provider chat defaults when stale backend state arrives before CEF success', async () => {
    const cefSuccess: { current: ((response: string) => void) | null } = { current: null }
    window.cefQuery = ({ onSuccess }) => {
      cefSuccess.current = onSuccess
    }
    useAppStore.setState({
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#f97316', description: '' },
        { id: 'codex-cli', name: 'Codex', shortName: 'Codex', color: '#0ea5e9', description: '' },
      ],
      defaultNewChatProviderId: 'gemini-cli',
      providerChatDefaults: {
        'codex-cli': {
          modelId: '',
          approvalMode: 'default',
          autoApproveCommands: false,
          memoryEnabled: true,
          reasoningEffort: 'medium',
          serviceTier: 'flex',
        },
      },
      lastAppliedStateRevision: -1,
    })

    const nextDefaults = {
      'codex-cli': {
        modelId: 'gpt-5.4',
        approvalMode: 'plan',
        autoApproveCommands: true,
        memoryEnabled: false,
        reasoningEffort: 'high',
        serviceTier: 'fast',
      },
    }

    const change = useAppStore.getState().setProviderChatDefaults({
      defaultNewChatProviderId: 'codex-cli',
      providerChatDefaults: nextDefaults,
    })

    expect(useAppStore.getState().defaultNewChatProviderId).toBe('codex-cli')
    expect(useAppStore.getState().providerChatDefaults['codex-cli']).toEqual(nextDefaults['codex-cli'])

    const staleState = makeCppState(1)
    staleState.settings.defaultNewChatProviderId = 'gemini-cli'
    staleState.settings.providerChatDefaults = {
      'codex-cli': {
        modelId: '',
        approvalMode: 'default',
        autoApproveCommands: false,
        memoryEnabled: true,
        reasoningEffort: 'medium',
        serviceTier: 'flex',
      },
    }
    useAppStore.getState().loadFromCef(staleState)

    expect(useAppStore.getState().defaultNewChatProviderId).toBe('codex-cli')
    expect(useAppStore.getState().providerChatDefaults['codex-cli']).toEqual(nextDefaults['codex-cli'])

    expect(cefSuccess.current).toBeTruthy()
    cefSuccess.current?.('{}')
    await expect(change).resolves.toBe(true)
  })

  it('sends approval mode changes through CEF and rolls back on failure', async () => {
    const now = new Date()
    const requests: Array<{ action: string; payload?: unknown }> = []
    let rejectNext = false
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      requests.push(JSON.parse(request))
      if (rejectNext) {
        onFailure(409, 'ACP is busy')
        return
      }
      onSuccess('{}')
    }
    useAppStore.setState({
      sessions: [
        {
          id: 'chat-1',
          name: 'Gemini Session',
          viewMode: 'chat',
          folderId: 'default',
          approvalMode: 'default',
          createdAt: now,
          updatedAt: now,
        },
      ],
    })

    await expect(useAppStore.getState().setSessionApprovalMode('chat-1', 'plan')).resolves.toBe(true)
    expect(requests[0].action).toBe('setChatApprovalMode')
    expect(requests[0].payload).toEqual({ chatId: 'chat-1', modeId: 'plan' })
    expect(useAppStore.getState().sessions[0].approvalMode).toBe('plan')

    rejectNext = true
    await expect(useAppStore.getState().setSessionApprovalMode('chat-1', 'default')).resolves.toBe(false)
    expect(requests[1].payload).toEqual({ chatId: 'chat-1', modeId: 'default' })
    expect(useAppStore.getState().sessions[0].approvalMode).toBe('plan')

    await expect(useAppStore.getState().setSessionApprovalMode('chat-1', 'yolo')).resolves.toBe(false)
    expect(requests).toHaveLength(2)
    expect(useAppStore.getState().sessions[0].approvalMode).toBe('plan')
  })

  it('sends UAM auto-approve command changes through CEF and rolls back on failure', async () => {
    const now = new Date()
    const requests: Array<{ action: string; payload?: unknown }> = []
    let rejectNext = false
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      requests.push(JSON.parse(request))
      if (rejectNext) {
        onFailure(409, 'Auto approve failed')
        return
      }
      onSuccess('{}')
    }
    useAppStore.setState({
      sessions: [
        {
          id: 'chat-1',
          name: 'Gemini Session',
          viewMode: 'chat',
          folderId: 'default',
          approvalMode: 'plan',
          autoApproveCommands: false,
          createdAt: now,
          updatedAt: now,
        },
      ],
    })

    await expect(useAppStore.getState().setSessionAutoApproveCommands('chat-1', true)).resolves.toBe(true)
    expect(requests[0].action).toBe('setChatAutoApproveCommands')
    expect(requests[0].payload).toEqual({ chatId: 'chat-1', enabled: true })
    expect(useAppStore.getState().sessions[0].approvalMode).toBe('plan')
    expect(useAppStore.getState().sessions[0].autoApproveCommands).toBe(true)

    rejectNext = true
    await expect(useAppStore.getState().setSessionAutoApproveCommands('chat-1', false)).resolves.toBe(false)
    expect(requests[1].payload).toEqual({ chatId: 'chat-1', enabled: false })
    expect(useAppStore.getState().sessions[0].autoApproveCommands).toBe(true)
  })

  it('persists command safety tier changes through CEF and rolls back on failure', async () => {
    const now = new Date()
    const requests: Array<{ action: string; payload?: unknown }> = []
    let rejectNext = false
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      requests.push(JSON.parse(request))
      if (rejectNext) onFailure(500, 'Safety tier failed')
      else onSuccess('{}')
    }
    useAppStore.setState({
      sessions: [{
        id: 'chat-1', name: 'Chat', viewMode: 'chat', folderId: 'default', commandSafetyTier: 'medium', createdAt: now, updatedAt: now,
      }],
    })

    await expect(useAppStore.getState().setSessionCommandSafetyTier('chat-1', 'low')).resolves.toBe(true)
    expect(requests[0]).toMatchObject({ action: 'setChatCommandSafetyTier', payload: { chatId: 'chat-1', commandSafetyTier: 'low' } })
    expect(useAppStore.getState().sessions[0].commandSafetyTier).toBe('low')

    rejectNext = true
    await expect(useAppStore.getState().setSessionCommandSafetyTier('chat-1', 'high')).resolves.toBe(false)
    expect(useAppStore.getState().sessions[0].commandSafetyTier).toBe('low')
  })

  it('sends planning mode changes when the live runtime mode differs from the saved chat mode', async () => {
    const now = new Date()
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }
    useAppStore.setState({
      sessions: [
        {
          id: 'chat-1',
          name: 'Codex Session',
          viewMode: 'chat',
          folderId: 'default',
          approvalMode: 'default',
          createdAt: now,
          updatedAt: now,
        },
      ],
      acpBindingBySessionId: {
        'chat-1': {
          sessionId: 'native-1',
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
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
          planSummary: '',
          planEntries: [],
          availableModes: [
            { id: 'default', name: 'Default', description: 'Run normally' },
            { id: 'plan', name: 'Plan', description: 'Plan before editing' },
          ],
          currentModeId: 'plan',
          availableModels: [],
          currentModelId: '',
          turnEvents: [],
          turnUserMessageIndex: -1,
          turnAssistantMessageIndex: -1,
          turnSerial: 0,
          pendingPermission: null,
          pendingUserInput: null,
          agentInfo: null,
        },
      },
    })

    await expect(useAppStore.getState().setSessionApprovalMode('chat-1', 'default')).resolves.toBe(true)

    expect(requests).toHaveLength(1)
    expect(requests[0].action).toBe('setChatApprovalMode')
    expect(requests[0].payload).toEqual({ chatId: 'chat-1', modeId: 'default' })
    expect(useAppStore.getState().sessions[0].approvalMode).toBe('default')
    expect(useAppStore.getState().acpBindingBySessionId['chat-1'].currentModeId).toBe('default')
  })

  it('deletes a folder and its sessions from local UI state', () => {
    const now = new Date()
    useAppStore.setState({
      folders: [
        { id: 'default', name: 'General', parentId: null, directory: '/tmp/general', isExpanded: true, createdAt: now },
        { id: 'project', name: 'Project', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: now },
      ],
      sessions: [
        { id: 'chat-folder', name: 'Folder chat', viewMode: 'cli', folderId: 'project', createdAt: now, updatedAt: now },
        { id: 'chat-general', name: 'General chat', viewMode: 'cli', folderId: 'default', createdAt: now, updatedAt: now },
      ],
      activeSessionId: 'chat-folder',
      messages: {
        'chat-folder': [{ id: 'm-folder', sessionId: 'chat-folder', role: 'user', content: 'delete me', createdAt: now }],
        'chat-general': [{ id: 'm-general', sessionId: 'chat-general', role: 'user', content: 'keep me', createdAt: now }],
      },
      cliBindingBySessionId: {
        'chat-folder': {
          terminalId: 'term-folder',
          boundChatId: 'chat-folder',
          running: true,
          lifecycleState: 'idle',
          turnState: 'idle',
          processing: false,
          readySinceLastSelect: false,
          active: false,
          lastError: '',
        },
      },
      cliTranscriptBySessionId: {
        'chat-folder': { terminalId: 'term-folder', content: 'transcript' },
      },
    })

    useAppStore.getState().deleteFolder('project')

    const state = useAppStore.getState()
    expect(state.folders.map((folder) => folder.id)).toEqual(['default'])
    expect(state.sessions.map((session) => session.id)).toEqual(['chat-general'])
    expect(state.sessions[0].folderId).toBe('default')
    expect(state.activeSessionId).toBe('chat-general')
    expect(state.messages['chat-folder']).toBeUndefined()
    expect(state.messages['chat-general']).toHaveLength(1)
    expect(state.cliBindingBySessionId['chat-folder']).toBeUndefined()
    expect(state.cliTranscriptBySessionId['chat-folder']).toBeUndefined()
  })

  it('keeps folder state unchanged when CEF rejects folder delete', async () => {
    const now = new Date()
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onFailure }) => {
      requests.push(JSON.parse(request))
      onFailure(409, 'Cannot delete while Gemini is running')
    }

    useAppStore.setState({
      folders: [
        { id: 'default', name: 'General', parentId: null, directory: '/tmp/general', isExpanded: true, createdAt: now },
        { id: 'project', name: 'Project', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: now },
      ],
      sessions: [
        { id: 'chat-folder', name: 'Folder chat', viewMode: 'cli', folderId: 'project', createdAt: now, updatedAt: now },
        { id: 'chat-general', name: 'General chat', viewMode: 'cli', folderId: 'default', createdAt: now, updatedAt: now },
      ],
      activeSessionId: 'chat-folder',
      messages: {
        'chat-folder': [{ id: 'm-folder', sessionId: 'chat-folder', role: 'user', content: 'delete me', createdAt: now }],
      },
      cliBindingBySessionId: {
        'chat-folder': {
          terminalId: 'term-folder',
          boundChatId: 'chat-folder',
          running: true,
          lifecycleState: 'idle',
          turnState: 'idle',
          processing: false,
          readySinceLastSelect: false,
          active: false,
          lastError: '',
        },
      },
      cliTranscriptBySessionId: {
        'chat-folder': { terminalId: 'term-folder', content: 'transcript' },
      },
    })

    useAppStore.getState().deleteFolder('project')
    await new Promise((resolve) => setTimeout(resolve, 0))

    const state = useAppStore.getState()
    expect(requests).toHaveLength(1)
    expect(requests[0].action).toBe('deleteFolder')
    expect(requests[0].payload).toEqual({ folderId: 'project' })
    expect(state.folders.map((folder) => folder.id)).toEqual(['default', 'project'])
    expect(state.sessions.map((session) => session.id)).toEqual(['chat-folder', 'chat-general'])
    expect(state.activeSessionId).toBe('chat-folder')
    expect(state.messages['chat-folder']).toHaveLength(1)
    expect(state.cliBindingBySessionId['chat-folder']).toMatchObject({ terminalId: 'term-folder' })
    expect(state.cliTranscriptBySessionId['chat-folder']).toMatchObject({ content: 'transcript' })
  })

  it('does not clobber newer backend state when folder delete fails later', async () => {
    const now = new Date()
    const requests: Array<{ action: string; payload?: unknown }> = []
    let rejectDelete: () => void = () => {
      throw new Error('CEF delete request was not sent')
    }
    window.cefQuery = ({ request, onFailure }) => {
      requests.push(JSON.parse(request))
      rejectDelete = () => onFailure(409, 'Cannot delete while Gemini is running')
    }

    useAppStore.setState({
      folders: [
        { id: 'default', name: 'General', parentId: null, directory: '/tmp/general', isExpanded: true, createdAt: now },
        { id: 'project', name: 'Project', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: now },
      ],
      sessions: [
        { id: 'chat-folder', name: 'Folder chat', viewMode: 'cli', folderId: 'project', createdAt: now, updatedAt: now },
        { id: 'chat-general', name: 'General chat', viewMode: 'cli', folderId: 'default', createdAt: now, updatedAt: now },
      ],
      activeSessionId: 'chat-folder',
      lastAppliedStateRevision: 1,
    })

    useAppStore.getState().deleteFolder('project')
    useAppStore.getState().loadFromCef({
      ...makeCppState(2, 'chat-general'),
      folders: [
        { id: 'default', title: 'General', directory: '/tmp/general', collapsed: false },
        { id: 'project', title: 'Project', directory: '/tmp/project', collapsed: false },
      ],
      chats: [
        {
          id: 'chat-folder',
          title: 'Folder chat from backend',
          folderId: 'project',
          providerId: 'gemini-cli',
          createdAt: '2026-01-01T00:00:00.000Z',
          updatedAt: '2026-01-01T00:00:02.000Z',
          messages: [],
        },
        {
          id: 'chat-general',
          title: 'General chat from backend',
          folderId: 'default',
          providerId: 'gemini-cli',
          createdAt: '2026-01-01T00:00:00.000Z',
          updatedAt: '2026-01-01T00:00:02.000Z',
          messages: [],
        },
      ],
    })
    rejectDelete()
    await new Promise((resolve) => setTimeout(resolve, 0))

    const state = useAppStore.getState()
    expect(requests).toHaveLength(1)
    expect(state.lastAppliedStateRevision).toBe(2)
    expect(state.sessions.map((session) => session.name)).toEqual([
      'Folder chat from backend',
      'General chat from backend',
    ])
    expect(state.activeSessionId).toBe('chat-general')
  })

  it('toggles folder expansion locally without changing the active chat', async () => {
    const now = new Date()
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }

    useAppStore.setState({
      folders: [
        { id: 'default', name: 'General', parentId: null, directory: '/tmp/general', isExpanded: true, createdAt: now },
        { id: 'project', name: 'Project', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: now },
      ],
      sessions: [
        { id: 'chat-folder', name: 'Folder chat', viewMode: 'cli', folderId: 'project', createdAt: now, updatedAt: now },
        { id: 'chat-general', name: 'General chat', viewMode: 'cli', folderId: 'default', createdAt: now, updatedAt: now },
      ],
      activeSessionId: 'chat-folder',
      messages: {
        'chat-folder': [{ id: 'm-folder', sessionId: 'chat-folder', role: 'user', content: 'keep selected', createdAt: now }],
      },
      lastAppliedStateRevision: 7,
    })

    useAppStore.getState().toggleFolder('project')
    await new Promise((resolve) => setTimeout(resolve, 0))

    const state = useAppStore.getState()
    expect(requests).toHaveLength(1)
    expect(requests[0].action).toBe('toggleFolder')
    expect(requests[0].payload).toEqual({ folderId: 'project' })
    expect(state.folders.find((folder) => folder.id === 'project')?.isExpanded).toBe(false)
    expect(state.activeSessionId).toBe('chat-folder')
    expect(state.messages['chat-folder']).toEqual([
      { id: 'm-folder', sessionId: 'chat-folder', role: 'user', content: 'keep selected', createdAt: now },
    ])
    expect(state.lastAppliedStateRevision).toBe(7)
  })

  it('reorders workspace folders and keeps omitted folders stable', async () => {
    const createdAt = new Date()
    useAppStore.setState({
      folders: ['one', 'two', 'three'].map((id) => ({
        id,
        name: id,
        parentId: null,
        directory: `/tmp/${id}`,
        isExpanded: true,
        createdAt,
      })),
    })

    await expect(useAppStore.getState().reorderFolders(['three', 'one'])).resolves.toBe(true)
    expect(useAppStore.getState().folders.map((folder) => folder.id)).toEqual(['three', 'one', 'two'])
  })

  it('rolls folder expansion back on CEF failure without changing the active chat', async () => {
    const now = new Date()
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onFailure }) => {
      requests.push(JSON.parse(request))
      onFailure(500, 'Failed to save folders')
    }

    useAppStore.setState({
      folders: [
        { id: 'default', name: 'General', parentId: null, directory: '/tmp/general', isExpanded: true, createdAt: now },
        { id: 'project', name: 'Project', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: now },
      ],
      sessions: [
        { id: 'chat-folder', name: 'Folder chat', viewMode: 'cli', folderId: 'project', createdAt: now, updatedAt: now },
        { id: 'chat-general', name: 'General chat', viewMode: 'cli', folderId: 'default', createdAt: now, updatedAt: now },
      ],
      activeSessionId: 'chat-folder',
      messages: {
        'chat-folder': [{ id: 'm-folder', sessionId: 'chat-folder', role: 'assistant', content: 'still selected', createdAt: now }],
      },
    })

    useAppStore.getState().toggleFolder('project')
    await new Promise((resolve) => setTimeout(resolve, 0))

    const state = useAppStore.getState()
    expect(requests).toHaveLength(1)
    expect(requests[0].action).toBe('toggleFolder')
    expect(requests[0].payload).toEqual({ folderId: 'project' })
    expect(state.folders.find((folder) => folder.id === 'project')?.isExpanded).toBe(true)
    expect(state.activeSessionId).toBe('chat-folder')
    expect(state.messages['chat-folder']).toEqual([
      { id: 'm-folder', sessionId: 'chat-folder', role: 'assistant', content: 'still selected', createdAt: now },
    ])
  })

  it('sanitizes malformed initial state and pushed stateUpdate payloads', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    vi.spyOn(console, 'error').mockImplementation(() => {})

    const malformedChat = {
          id: 'chat-1',
          title: 'Sanitized Session',
          folderId: 'default',
          providerId: 'gemini-cli',
          workspaceDirectory: '/tmp/project',
          approvalMode: 'yolo',
          modelId: 'bad model',
          createdAt: '2026-01-01T00:00:00.000Z',
          updatedAt: '2026-01-01T00:00:01.000Z',
          messages: [
            {
              role: 'user',
              content: 'hello',
              toolCalls: [{ id: 'tool-message-1', title: 'Saved tool' }, { title: 'missing id' }],
              createdAt: '2026-01-01T00:00:00.000Z',
            },
            { role: 'assistant', content: 42, createdAt: '2026-01-01T00:00:01.000Z' },
            { role: 'bot', content: 'bad role', createdAt: '2026-01-01T00:00:02.000Z' },
          ],
          cliTerminal: {
            terminalId: 99,
            running: 'yes',
            lifecycleState: 3,
            lastError: 7,
          },
          acpSession: {
            running: true,
            processing: 'yes',
            diagnostics: [{ reason: 'ok' }, 'bad-diagnostic'],
            toolCalls: [{ id: 'tool-1', title: 'Read' }, { title: 'missing id' }],
            planEntries: ['bad-plan', { content: 'Inspect', priority: 'high' }],
            availableModes: [
              { id: 'plan', name: 'Plan', description: 'Plan first' },
              { id: '', name: 'Missing id' },
              'bad-mode',
            ],
            currentModeId: 'auto_edit',
            availableModels: [
              { id: 'models/gemini-3-pro-preview', name: 'Gemini 3 Pro' },
              { id: '-bad' },
              'bad-model',
            ],
            currentModelId: 'models/gemini-3-pro-preview',
            turnEvents: [
              'bad-event',
              { type: 'assistant_text', text: 'streamed' },
              { type: 'tool_call' },
            ],
            pendingPermission: {
              requestId: 'req-1',
              options: [{ id: 'allow', name: 'Allow' }, { name: 'missing id' }],
            },
          },
        }

    const malformedState = {
      stateRevision: 1,
      folders: [
        { id: 'default', title: 'General', directory: '/tmp/project', collapsed: false },
        { id: '', title: 'Missing id', directory: 7, collapsed: 'no' },
        'bad-folder',
      ],
      chats: [
        malformedChat,
        { title: 'Missing id', messages: [] },
        'bad-chat',
      ],
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', outputMode: 'cli' },
        { id: '', name: 'Missing id' },
        'bad-provider',
      ],
      selectedChatId: 42,
      selectedChatIndex: 0,
      settings: {
        activeProviderId: 7,
        theme: 'system',
      },
      cliDebug: {
        terminalCount: 'bad',
        terminals: [
          { terminalId: 'term-1', running: true, turnState: 'busy' },
          { running: true },
        ],
      },
    }

    testWindow.cefQuery = ({ onSuccess }) => {
      onSuccess(JSON.stringify(malformedState))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))

    let state = cefStore.getState()
    expect(state.folders.map((folder) => folder.id)).toEqual(['default'])
    expect(state.sessions.map((session) => session.id)).toEqual(['chat-1'])
    expect(state.sessions[0].approvalMode).toBe('default')
    expect(state.sessions[0].autoApproveCommands).toBe(true)
    expect(state.sessions[0].modelId).toBe('')
    expect(state.activeSessionId).toBe('chat-1')
    expect(state.messages['chat-1'].map((message) => message.content)).toEqual(['hello'])
    expect(state.messages['chat-1'][0].toolCalls?.map((tool) => tool.id)).toEqual(['tool-message-1'])
    expect(state.providers.map((provider) => provider.id)).toEqual(['gemini-cli'])
    expect(state.theme).toBe('system')
    expect(state.cliDebugState?.terminals.map((terminal) => terminal.terminalId)).toEqual(['term-1'])
    expect(state.acpBindingBySessionId['chat-1'].toolCalls.map((tool) => tool.id)).toEqual(['tool-1'])
    expect(state.acpBindingBySessionId['chat-1'].availableModes.map((mode) => mode.id)).toEqual(['plan'])
    expect(state.acpBindingBySessionId['chat-1'].currentModeId).toBe('acceptEdits')
    expect(state.acpBindingBySessionId['chat-1'].availableModels.map((model) => model.id)).toEqual([
      'models/gemini-3-pro-preview',
    ])
    expect(state.acpBindingBySessionId['chat-1'].currentModelId).toBe('models/gemini-3-pro-preview')
    expect(state.acpBindingBySessionId['chat-1'].turnEvents).toEqual([
      { type: 'assistant_text', text: 'streamed', toolCallId: undefined, requestId: undefined },
    ])
    expect(state.acpBindingBySessionId['chat-1'].pendingPermission?.options.map((option) => option.id)).toEqual(['allow'])

    const pushedState = {
      ...malformedState,
      stateRevision: 2,
      folders: [
        { id: 'default', title: 'General', directory: '/tmp/project', collapsed: false },
        null,
      ],
      chats: [
        {
          ...malformedChat,
          title: 'Updated Session',
          messages: [
            { role: 'assistant', content: 'safe update', createdAt: '2026-01-01T00:00:03.000Z' },
            null,
          ],
        },
        null,
      ],
      providers: ['bad-provider'],
      selectedChatId: 'chat-1',
      selectedChatIndex: 'bad',
      settings: null,
    }

    expect(() => testWindow.uamPush?.({ type: 'stateUpdate', data: pushedState })).not.toThrow()
    state = cefStore.getState()
    expect(state.lastAppliedStateRevision).toBe(2)
    expect(state.sessions[0].name).toBe('Updated Session')
    expect(state.messages['chat-1'].map((message) => message.content)).toEqual(['safe update'])
    expect(state.providers.map((provider) => provider.id)).toEqual(['gemini-cli', 'codex-cli', 'claude-cli', 'opencode-cli', 'copilot-cli'])
    expect(state.theme).toBe('dark')
  })

  it('updates pinning state and sends setChatPinned through CEF', async () => {
    const now = new Date()
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }
    useAppStore.setState({
      sessions: [
        {
          id: 'chat-1',
          name: 'Gemini Session',
          viewMode: 'chat',
          folderId: 'default',
          isPinned: false,
          createdAt: now,
          updatedAt: now,
        },
      ],
    })

    await expect(useAppStore.getState().setSessionPinned('chat-1', true)).resolves.toBe(true)

    expect(requests).toHaveLength(1)
    expect(requests[0].action).toBe('setChatPinned')
    expect(requests[0].payload).toEqual({ chatId: 'chat-1', pinned: true })
    expect(useAppStore.getState().sessions[0].isPinned).toBe(true)
  })

  it('does not send setChatPinned when the requested pin state is already applied', async () => {
    const now = new Date()
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }
    useAppStore.setState({
      sessions: [
        {
          id: 'chat-1',
          name: 'Gemini Session',
          viewMode: 'chat',
          folderId: 'default',
          isPinned: true,
          createdAt: now,
          updatedAt: now,
        },
      ],
    })

    await expect(useAppStore.getState().setSessionPinned('chat-1', true)).resolves.toBe(true)

    expect(requests).toHaveLength(0)
    expect(useAppStore.getState().sessions[0].isPinned).toBe(true)
  })

  it('deserializes memory state and toggles chat memory through CEF', async () => {
    const cppState = makeCppState(1)
    cppState.chats[0].memoryEnabled = false
    cppState.chats[0].memoryLastProcessedMessageCount = 3
    cppState.chats[0].memoryLastProcessedAt = '2026-01-01T00:00:02.000Z'
    cppState.settings.memoryEnabledDefault = false
    cppState.settings.memoryIdleDelaySeconds = 90
    cppState.settings.memoryRecallBudgetBytes = 1536
    cppState.settings.memoryLastStatus = 'Memory updated.'
    cppState.settings.memoryWorkerBindings = {
      'gemini-cli': { workerProviderId: 'codex-cli', workerModelId: 'gpt-5.4-mini' },
    }
    cppState.memoryActivity = {
      entryCount: 4,
      lastCreatedAt: '2026-01-01T00:00:03.000Z',
      lastCreatedCount: 2,
      runningCount: 1,
      lastStatus: 'Memory updated.',
      lastWorkerChatId: 'chat-1',
      lastWorkerProviderId: 'codex-cli',
      lastWorkerUpdatedAt: '2026-01-01T00:00:04.000Z',
      lastWorkerStatus: 'Memory worker completed.',
      lastWorkerOutput: '{"memories":[]}',
      lastWorkerError: '',
      lastWorkerTimedOut: false,
      lastWorkerCanceled: false,
      lastWorkerHasExitCode: true,
      lastWorkerExitCode: 0,
    }

    useAppStore.getState().loadFromCef(cppState)
    expect(useAppStore.getState().sessions[0].memoryEnabled).toBe(false)
    expect(useAppStore.getState().sessions[0].memoryLastProcessedMessageCount).toBe(3)
    expect(useAppStore.getState().memoryEnabledDefault).toBe(false)
    expect(useAppStore.getState().memoryIdleDelaySeconds).toBe(90)
    expect(useAppStore.getState().memoryRecallBudgetBytes).toBe(1536)
    expect(useAppStore.getState().memoryWorkerBindings['gemini-cli'].workerProviderId).toBe('codex-cli')
    expect(useAppStore.getState().memoryActivity).toMatchObject({
      entryCount: 4,
      lastCreatedAt: '2026-01-01T00:00:03.000Z',
      lastCreatedCount: 2,
      runningCount: 1,
      lastStatus: 'Memory updated.',
      lastWorkerChatId: 'chat-1',
      lastWorkerProviderId: 'codex-cli',
      lastWorkerOutput: '{"memories":[]}',
      lastWorkerHasExitCode: true,
      lastWorkerExitCode: 0,
    })

    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }

    await expect(useAppStore.getState().setSessionMemoryEnabled('chat-1', true)).resolves.toBe(true)
    expect(requests[0].action).toBe('setChatMemoryEnabled')
    expect(requests[0].payload).toEqual({ chatId: 'chat-1', enabled: true })
    expect(useAppStore.getState().sessions[0].memoryEnabled).toBe(true)
  })

  it('persists editor settings through CEF', async () => {
    const cppState = makeCppState(1)
    useAppStore.getState().loadFromCef(cppState)

    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }

    await expect(useAppStore.getState().setEditorSettings({
      defaultEditorPresetId: 'webstorm',
      editorFileAssociations: [
        {
          id: 'cpp',
          name: 'C++',
          extensions: ['cpp', '.h'],
          editorPresetId: 'clion',
        },
        {
          id: 'javascript',
          name: 'JavaScript',
          extensions: ['js', '.mjs'],
          editorPresetId: 'webstorm',
        },
      ],
    })).resolves.toBe(true)

    expect(requests[0].action).toBe('setEditorSettings')
    expect(requests[0].payload).toEqual({
      defaultEditorPresetId: 'webstorm',
      fileAssociations: [
        {
          id: 'cpp',
          name: 'C++',
          extensions: ['.cpp', '.h'],
          editorPresetId: 'clion',
        },
        {
          id: 'javascript',
          name: 'JavaScript',
          extensions: ['.js', '.mjs'],
          editorPresetId: 'webstorm',
        },
      ],
    })
    expect(useAppStore.getState().defaultEditorPresetId).toBe('webstorm')
    expect(useAppStore.getState().editorFileAssociations[0].extensions).toEqual(['.cpp', '.h'])
    expect(useAppStore.getState().editorFileAssociations[1].editorPresetId).toBe('webstorm')
  })

  it('saves and applies shell actions through CEF', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }
    const actions = [{
      id: 'review',
      label: 'Review Selection',
      skillPath: '/tmp/Review ü.uam',
      providerId: 'codex-cli',
      modelId: 'gpt-5.4',
      acceptsFiles: true,
      acceptsFolders: false,
      enabled: true,
      openWorkspace: false,
    }]

    await expect(useAppStore.getState().setShellActions(actions)).resolves.toBe(true)
    await expect(useAppStore.getState().applyShellActions()).resolves.toBe(true)

    expect(requests).toHaveLength(2)
    expect(requests[0]).toMatchObject({ action: 'setShellActions', payload: { actions } })
    expect(requests[1]).toMatchObject({ action: 'applyShellActions' })
    expect(useAppStore.getState().shellActions).toEqual(actions)
    expect(useAppStore.getState().shellActionNotification).toBe('Shell actions applied successfully.')
  })

  it('clamps memory settings before optimistic updates and CEF persistence', async () => {
    const cppState = makeCppState(1)
    useAppStore.getState().loadFromCef(cppState)

    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }

    await expect(useAppStore.getState().setMemorySettings({
      memoryIdleDelaySeconds: -1,
      memoryRecallBudgetBytes: 999999,
      goalMaxLoopIterations: -1,
    })).resolves.toBe(true)

    expect(useAppStore.getState().memoryIdleDelaySeconds).toBe(30)
    expect(useAppStore.getState().memoryRecallBudgetBytes).toBe(8192)
    expect(useAppStore.getState().goalMaxLoopIterations).toBe(0)
    expect(requests[0].action).toBe('setMemorySettings')
    expect(requests[0].payload?.idleDelaySeconds).toBe(30)
    expect(requests[0].payload?.recallBudgetBytes).toBe(8192)
    expect(requests[0].payload?.goalMaxLoopIterations).toBe(0)
  })

  it('loads the global memory library through CEF', async () => {
    const testWindow = ensureTestWindow()
    testWindow.cefQuery = vi.fn(({ request, onSuccess }) => {
      const parsed = JSON.parse(request as string)
      if (parsed.action === 'listMemoryEntries') {
        onSuccess?.(JSON.stringify({
          scope: {
            scopeType: 'global',
            folderId: '',
            label: 'Global memory',
            rootPath: '/tmp/uam-memory',
          },
          entries: [
            {
              id: 'allman.md',
              title: 'Project uses Allman braces',
              category: 'Lessons/User_Lessons',
              scope: 'global',
              confidence: 'high',
              sourceChatId: 'chat-1',
              lastObserved: '2026-01-01T00:00:00.000Z',
              occurrenceCount: 2,
              preview: 'Prefer Allman braces.',
              filePath: '/tmp/uam-memory/Lessons/User_Lessons/allman.md',
            },
          ],
        }))
        return
      }
      onSuccess?.('{}')
    }) as TestWindow['cefQuery']

    await expect(useAppStore.getState().openGlobalMemoryLibrary()).resolves.toBe(true)

    const state = useAppStore.getState()
    expect(state.memoryLibraryScope?.scopeType).toBe('global')
    expect(state.memoryLibraryEntries).toHaveLength(1)
    expect(state.memoryLibraryEntries[0].title).toBe('Project uses Allman braces')
  })

  it('loads the all memory library through CEF', async () => {
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    const testWindow = ensureTestWindow()
    testWindow.cefQuery = vi.fn(({ request, onSuccess }) => {
      const parsed = JSON.parse(request as string)
      requests.push({ action: parsed.action, payload: parsed.payload })

      if (parsed.action === 'listMemoryEntries') {
        onSuccess?.(JSON.stringify({
          scope: {
            scopeType: 'all',
            folderId: '',
            label: 'All memory',
            rootPath: 'Global and project memory roots',
            rootCount: 2,
          },
          entries: [
            {
              id: 'all/726f6f74/Lessons/User_Lessons/local.md',
              title: 'Local lesson',
              category: 'Lessons/User_Lessons',
              scope: 'local',
              confidence: 'high',
              sourceChatId: 'chat-1',
              lastObserved: '2026-01-01T00:00:00.000Z',
              occurrenceCount: 1,
              preview: 'Keep this project-specific.',
              filePath: '/tmp/project/.UAM/Lessons/User_Lessons/local.md',
              scopeType: 'folder',
              folderId: 'default',
              scopeLabel: 'General',
              rootPath: '/tmp/project/.UAM',
            },
          ],
        }))
        return
      }
      onSuccess?.('{}')
    }) as TestWindow['cefQuery']

    await expect(useAppStore.getState().openAllMemoryLibrary()).resolves.toBe(true)

    const state = useAppStore.getState()
    expect(requests[0].payload?.scopeType).toBe('all')
    expect(state.memoryLibraryScope?.scopeType).toBe('all')
    expect(state.memoryLibraryScope?.rootCount).toBe(2)
    expect(state.memoryLibraryEntries[0].scopeLabel).toBe('General')
  })

  it('accepts raw successful VCS commit responses from CEF', async () => {
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    const testWindow = ensureTestWindow()
    testWindow.cefQuery = vi.fn(({ request, onSuccess }) => {
      const parsed = JSON.parse(request as string)
      requests.push({ action: parsed.action, payload: parsed.payload })

      if (parsed.action === 'commitVcsChanges') {
        onSuccess?.(JSON.stringify({
          ok: true,
          message: 'Git commit created locally.',
          error: '',
          status: {
            available: true,
            vcsTypes: ['git'],
            activeVcsType: 'git',
            workspaceDirectory: '/tmp/project',
            branchOrRevision: 'main',
            changedFiles: [],
            warning: '',
            error: '',
          },
        }))
        return
      }

      onSuccess?.('{}')
    }) as TestWindow['cefQuery']

    const result = await useAppStore.getState().commitVcsChanges('chat-1', 'git', 'Update app', ['app.txt'])

    expect(requests[0]).toEqual({
      action: 'commitVcsChanges',
      payload: { chatId: 'chat-1', vcsType: 'git', message: 'Update app', files: ['app.txt'] },
    })
    expect(result.ok).toBe(true)
    expect(result.message).toBe('Git commit created locally.')
    expect(result.error).toBe('')
    expect(result.status?.changedFiles).toEqual([])
  })

  it('accepts raw successful worktree action responses from CEF', async () => {
    const testWindow = ensureTestWindow()
    testWindow.cefQuery = vi.fn(({ request, onSuccess }) => {
      const parsed = JSON.parse(request as string)

      if (parsed.action === 'createChatWorktree') {
        onSuccess?.(JSON.stringify({
          ok: true,
          message: 'Created isolated worktree.',
          patchPath: '',
          status: {
            isGitRepository: true,
            isSvnWorkspace: false,
            isolated: true,
            sourceDirty: false,
            worktreeDirty: false,
            worktreeMissing: false,
            sourceDirectory: '/tmp/project',
            worktreeDirectory: '/tmp/uam-worktree',
            branchName: 'uam/chat-1',
            baseRef: 'HEAD',
            warning: '',
            error: '',
          },
        }))
        return
      }

      onSuccess?.('{}')
    }) as TestWindow['cefQuery']

    const result = await useAppStore.getState().createChatWorktree('chat-1')

    expect(result.ok).toBe(true)
    expect(result.message).toBe('Created isolated worktree.')
    expect(result.status?.isolated).toBe(true)
  })

  it('creates and deletes memory entries through the active scope', async () => {
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    const testWindow = ensureTestWindow()
    testWindow.cefQuery = vi.fn(({ request, onSuccess }) => {
      const parsed = JSON.parse(request as string)
      requests.push({ action: parsed.action, payload: parsed.payload })

      if (parsed.action === 'listMemoryEntries') {
        onSuccess?.(JSON.stringify({
          scope: {
            scopeType: 'folder',
            folderId: 'default',
            label: 'General',
            rootPath: '/tmp/project/.UAM',
          },
          entries: [],
        }))
        return
      }

      onSuccess?.('{}')
    }) as TestWindow['cefQuery']

    await expect(useAppStore.getState().openFolderMemoryLibrary('default')).resolves.toBe(true)

    await expect(useAppStore.getState().createMemoryEntry({
      category: 'Lessons/User_Lessons',
      title: 'Brace style',
      memory: 'Use Allman braces.',
      evidence: 'Repository convention.',
      confidence: 'high',
      sourceChatId: 'chat-1',
    })).resolves.toBe(true)

    await expect(useAppStore.getState().deleteMemoryEntry('Lessons/User_Lessons/brace-style.md')).resolves.toBe(true)

    expect(requests.some((request) => request.action === 'createMemoryEntry')).toBe(true)
    expect(requests.some((request) => request.action === 'deleteMemoryEntry')).toBe(true)
    expect(requests.find((request) => request.action === 'createMemoryEntry')?.payload?.scopeType).toBe('folder')
  })

  it('sends an explicit target when creating from the all memory scope', async () => {
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    const testWindow = ensureTestWindow()
    testWindow.cefQuery = vi.fn(({ request, onSuccess }) => {
      const parsed = JSON.parse(request as string)
      requests.push({ action: parsed.action, payload: parsed.payload })

      if (parsed.action === 'listMemoryEntries') {
        onSuccess?.(JSON.stringify({
          scope: {
            scopeType: 'all',
            folderId: '',
            label: 'All memory',
            rootPath: 'Global and project memory roots',
          },
          entries: [],
        }))
        return
      }

      onSuccess?.('{}')
    }) as TestWindow['cefQuery']

    await expect(useAppStore.getState().openAllMemoryLibrary()).resolves.toBe(true)
    await expect(useAppStore.getState().createMemoryEntry({
      category: 'Lessons/User_Lessons',
      title: 'Scoped add',
      memory: 'Save this to the project root.',
      evidence: 'The user selected a folder target.',
      confidence: 'medium',
      sourceChatId: 'chat-1',
      targetScopeType: 'folder',
      targetFolderId: 'default',
    })).resolves.toBe(true)

    const createRequest = requests.find((request) => request.action === 'createMemoryEntry')
    expect(createRequest?.payload?.scopeType).toBe('all')
    expect(createRequest?.payload?.targetScopeType).toBe('folder')
    expect(createRequest?.payload?.targetFolderId).toBe('default')
  })

  it('loads scan candidates and queues a manual memory scan through CEF', async () => {
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    const testWindow = ensureTestWindow()
    testWindow.cefQuery = vi.fn(({ request, onSuccess }) => {
      const parsed = JSON.parse(request as string)
      requests.push({ action: parsed.action, payload: parsed.payload })

      if (parsed.action === 'listMemoryScanCandidates') {
        onSuccess?.(JSON.stringify({
          candidates: [
            {
              chatId: 'chat-1',
              title: 'Gemini Session',
              folderId: 'default',
              folderTitle: 'General',
              providerId: 'gemini-cli',
              messageCount: 12,
              memoryEnabled: true,
              memoryLastProcessedAt: '',
              alreadyFullyProcessed: false,
            },
          ],
        }))
        return
      }

      if (parsed.action === 'scanCurrentChats') {
        onSuccess?.(JSON.stringify({ queuedCount: 1 }))
        return
      }

      onSuccess?.('{}')
    }) as TestWindow['cefQuery']

    await expect(useAppStore.getState().openMemoryScanModal()).resolves.toBe(true)
    expect(useAppStore.getState().isMemoryScanModalOpen).toBe(true)
    expect(useAppStore.getState().selectedMemoryScanChatIds).toEqual(['chat-1'])

    useAppStore.getState().selectNoMemoryScanChats()
    expect(useAppStore.getState().selectedMemoryScanChatIds).toEqual([])

    useAppStore.getState().selectAllMemoryScanChats()
    expect(useAppStore.getState().selectedMemoryScanChatIds).toEqual(['chat-1'])

    await expect(useAppStore.getState().startMemoryScan()).resolves.toBe(true)
    expect(useAppStore.getState().isMemoryScanModalOpen).toBe(false)
    expect(requests.some((request) => request.action === 'listMemoryScanCandidates')).toBe(true)
    expect(requests.find((request) => request.action === 'scanCurrentChats')?.payload?.chatIds).toEqual(['chat-1'])
  })

  it('toggles goal mode per chat without making CEF requests', () => {
    const requests: { action: string; payload?: unknown }[] = []
    ensureTestWindow().cefQuery = ((params: { request: string }) => {
      requests.push(JSON.parse(params.request) as { action: string; payload?: unknown })
    }) as unknown as TestWindow['cefQuery']

    expect(useAppStore.getState().goalModeByChatId['chat-1'] ?? false).toBe(false)

    useAppStore.getState().setGoalMode('chat-1', true)
    expect(useAppStore.getState().goalModeByChatId['chat-1']).toBe(true)

    useAppStore.getState().setGoalMode('chat-1', false)
    expect(useAppStore.getState().goalModeByChatId['chat-1']).toBe(false)

    useAppStore.getState().setGoalMode('chat-2', true)
    expect(useAppStore.getState().goalModeByChatId['chat-2']).toBe(true)
    expect(useAppStore.getState().goalModeByChatId['chat-1']).toBe(false)

    expect(requests).toEqual([])
  })

  it('sends ACP goal context from active goal id without requiring goal mode toggle', async () => {
    const requests: { action: string; payload?: any }[] = []
    ensureTestWindow().cefQuery = ((params: { request: string; onSuccess?: (response: string) => void }) => {
      const parsed = JSON.parse(params.request) as { action: string; payload?: any }
      requests.push(parsed)
      params.onSuccess?.('{}')
    }) as unknown as TestWindow['cefQuery']

    useAppStore.setState({
      sessions: [{ id: 'chat-1', name: 'Codex', viewMode: 'chat', providerId: 'codex-cli', folderId: 'default', createdAt: new Date(), updatedAt: new Date(), isPinned: false }],
      activeGoalIdByChatId: { 'chat-1': 'goal-1' },
      goalModeByChatId: { 'chat-1': false },
    })

    await expect(useAppStore.getState().sendAcpPrompt('chat-1', 'continue')).resolves.toBe(true)

    const promptRequest = requests.find((request) => request.action === 'sendAcpPrompt')
    expect(promptRequest?.payload?.goalId).toBe('goal-1')
    expect(promptRequest?.payload?.goalMode).toBe(true)
  })

  it('loads queued ACP prompt payloads and blocks provider switching until they clear', async () => {
    const state = makeCppState(1)
    state.chats[0].acpSession = {
      sessionId: 'acp-chat-1',
      running: true,
      processing: false,
      lifecycleState: 'error',
      queuedPrompts: [
        {
          text: 'Queued follow-up',
          markdownStoreFiles: ['/tmp/review.uam'],
          attachments: [
            { id: 'attachment-1', name: 'diagram.png', type: 'image', size: 42, path: '/tmp/diagram.png' },
          ],
          goalMode: true,
          goalId: 'goal-1',
        },
      ],
    }
    useAppStore.getState().loadFromCef(state)

    expect(useAppStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts).toEqual(state.chats[0].acpSession.queuedPrompts)
    const requests: unknown[] = []
    ensureTestWindow().cefQuery = ((params: { request: string }) => {
      requests.push(JSON.parse(params.request))
    }) as unknown as TestWindow['cefQuery']

    await expect(useAppStore.getState().setSessionProvider('chat-1', 'codex-cli')).resolves.toBe(false)
    expect(requests).toEqual([])
  })

  it('resumes a goal through the setActiveGoal backend action', async () => {
    const requests: { action: string; payload?: any }[] = []
    ensureTestWindow().cefQuery = ((params: { request: string; onSuccess?: (response: string) => void }) => {
      const parsed = JSON.parse(params.request) as { action: string; payload?: any }
      requests.push(parsed)
      params.onSuccess?.('{}')
    }) as unknown as TestWindow['cefQuery']

    await expect(useAppStore.getState().resumeGoal('chat-1', 'goal-1')).resolves.toBe(true)

    const resumeRequest = requests.find((request) => request.action === 'setActiveGoal')
    expect(resumeRequest?.payload).toEqual({ chatId: 'chat-1', goalId: 'goal-1' })
  })
})
