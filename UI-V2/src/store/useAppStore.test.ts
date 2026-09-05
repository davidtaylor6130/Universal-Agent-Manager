import { beforeEach, describe, expect, it, vi } from 'vitest'
import type { AcpBinding } from './cpp/types'
import { CppAppState, useAppStore } from './useAppStore'
import { createUiSlice } from './slices/uiSlice'

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
  selectedChatId: string | null = 'chat-1',
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
      showProviderIconsInSidebar: true,
      showWorktreePathInSidebar: true,
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
    statusLine: '',
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
    statusLine: '',
    memoryActivity: {
      entryCount: 0,
      lastCreatedAt: '',
      lastCreatedCount: 0,
      runningCount: 0,
      lastStatus: '',
    },
    theme: 'dark',
    workingDisplayMode: 'verbose',
    showProviderIconsInSidebar: true,
    showWorktreePathInSidebar: true,
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

  it('keeps loaded skills cached when the Skills window closes', () => {
    const entries = [{ id: 'github', title: 'GitHub', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', filePath: '/tmp/github.uam' }]
    useAppStore.setState({ isMarkdownStoreOpen: true, markdownStoreEntries: entries })

    useAppStore.getState().closeMarkdownStore()

    expect(useAppStore.getState().isMarkdownStoreOpen).toBe(false)
    expect(useAppStore.getState().markdownStoreEntries).toEqual(entries)
  })

  it('clears the selected chat locally and through CEF', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }
    useAppStore.setState({
      sessions: [{ id: 'chat-1', name: 'Chat 1', viewMode: 'chat', folderId: 'default', createdAt: new Date(), updatedAt: new Date() }],
      activeSessionId: 'chat-1',
    })

    useAppStore.getState().setActiveSession(null)
    expect(useAppStore.getState().activeSessionId).toBeNull()
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(requests).toHaveLength(1)
    expect(requests[0]).toMatchObject({ action: 'selectSession', payload: { chatId: '' } })
  })

  it('does not reselect and rewrite the active chat', async () => {
    const requests: Array<{ action: string }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }
    const sessions = [{ id: 'chat-1', name: 'Chat 1', viewMode: 'chat' as const, folderId: 'default', createdAt: new Date(), updatedAt: new Date() }]
    useAppStore.setState({ sessions, activeSessionId: 'chat-1' })

    useAppStore.getState().setActiveSession('chat-1')
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(useAppStore.getState().sessions).toBe(sessions)
    expect(requests).toEqual([])
  })

  it('ignores stale successful selection responses before hydrating messages', async () => {
    const now = new Date()
    const finishSelection = new Map<string, () => void>()
    const hydrated: string[] = []
    window.cefQuery = ({ request, onSuccess }) => {
      const envelope = JSON.parse(request) as { action: string; payload?: { chatId?: string } }
      const chatId = envelope.payload?.chatId ?? ''
      if (envelope.action === 'selectSession') finishSelection.set(chatId, () => onSuccess('{}'))
      else if (envelope.action === 'getChatMessages') {
        hydrated.push(chatId)
        onSuccess(JSON.stringify({ chatId, unchanged: true }))
      }
    }
    useAppStore.setState({
      sessions: [
        { id: 'chat-a', name: 'A', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
        { id: 'chat-b', name: 'B', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
      ],
      activeSessionId: null,
    })

    useAppStore.getState().setActiveSession('chat-a')
    useAppStore.getState().setActiveSession('chat-b')
    finishSelection.get('chat-b')?.()
    await new Promise((resolve) => setTimeout(resolve, 0))
    finishSelection.get('chat-a')?.()
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(useAppStore.getState().activeSessionId).toBe('chat-b')
    expect(hydrated).toEqual(['chat-b'])
  })

  it('preserves an explicit empty selection from native state and patches', () => {
    useAppStore.getState().loadFromCef(makeCppState(1, null))
    expect(useAppStore.getState().activeSessionId).toBeNull()

    const testWindow = ensureTestWindow()
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, selectedChatId: null } })
    expect(useAppStore.getState().activeSessionId).toBeNull()
  })

  it('distinguishes a failed Skills directory browse from cancellation', async () => {
    let response: 'failure' | 'cancel' | 'selected' = 'failure'
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      if (response === 'failure') onSuccess(JSON.stringify({ ok: false, error: 'Folder picker failed.' }))
      else onSuccess(JSON.stringify({ selectedPath: response === 'selected' ? ' /tmp/chosen-store ' : '' }))
    }
    useAppStore.setState({ markdownStoreError: 'Stale error.' })

    await expect(useAppStore.getState().browseMarkdownStoreDirectory('/tmp/typed-store')).resolves.toBeNull()
    expect(useAppStore.getState().markdownStoreError).toBe('Folder picker failed.')

    response = 'cancel'
    await expect(useAppStore.getState().browseMarkdownStoreDirectory('/tmp/typed-store')).resolves.toBeNull()
    expect(useAppStore.getState().markdownStoreError).toBe('')

    response = 'selected'
    useAppStore.setState({ markdownStoreError: 'Another stale error.' })
    await expect(useAppStore.getState().browseMarkdownStoreDirectory('/tmp/typed-store')).resolves.toBe('/tmp/chosen-store')
    expect(useAppStore.getState().markdownStoreError).toBe('')
    expect(requests).toEqual(Array(3).fill(expect.objectContaining({
      action: 'browseMarkdownStoreDirectory',
      payload: { currentValue: '/tmp/typed-store' },
    })))
  })

  it('persists the working display mode locally', () => {
    const stored = new Map<string, string>()
    Object.defineProperty(window, 'localStorage', {
      configurable: true,
      value: {
        clear: () => stored.clear(),
        getItem: (key: string) => stored.get(key) ?? null,
        setItem: (key: string, value: string) => stored.set(key, value),
      },
    })
    window.localStorage.clear()

    useAppStore.getState().setWorkingDisplayMode('compact')

    expect(useAppStore.getState().workingDisplayMode).toBe('compact')
    expect(JSON.parse(window.localStorage.getItem('uam-app-shell-layout-v2') ?? '{}')).toMatchObject({
      workingDisplayMode: 'compact',
    })
    expect(createUiSlice(vi.fn(), () => useAppStore.getState(), false).workingDisplayMode).toBe('compact')
  })

  it('does not claim provider CLI operations succeeded outside CEF', async () => {
    const store = useAppStore.getState()
    const before = useAppStore.getState().cliVersionManager

    await expect(store.refreshCliProviderVersion(' Claude-Code ')).resolves.toBe(false)
    await expect(store.applyCliProviderVersion(' Claude-Code ', 'latest')).resolves.toBe(false)

    expect(useAppStore.getState().cliVersionManager).toBe(before)
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
      configOptions: [{
        id: 'thought_level',
        name: 'Thinking Style',
        description: 'Provider-owned model variant.',
        category: 'custom/provider-category',
        currentValue: 'balanced.v2',
        options: [
          { value: 'balanced.v2', name: 'Balanced 2.0', description: 'Normal thinking.' },
          { value: 'deep/custom', name: 'Deep + Custom', description: 'Provider-specific value.' },
          { value: '', name: 'Invalid empty value', description: '' },
        ],
      }],
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
	    expect(state.acpBindingBySessionId['chat-1'].configOptions).toEqual([{
	      id: 'thought_level',
	      name: 'Thinking Style',
	      description: 'Provider-owned model variant.',
	      category: 'custom/provider-category',
	      currentValue: 'balanced.v2',
	      options: [
	        { value: 'balanced.v2', name: 'Balanced 2.0', description: 'Normal thinking.' },
	        { value: 'deep/custom', name: 'Deep + Custom', description: 'Provider-specific value.' },
	      ],
	    }])
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

  it('drops buffered output when a state patch replaces the terminal identity', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    testWindow.cefQuery = ({ onSuccess }) => {
      onSuccess(JSON.stringify(makeCppState(1, 'chat-1', { terminalId: 'term-old' })))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    cefStore.setState({
      cliTranscriptBySessionId: {
        'chat-1': { terminalId: 'term-old', content: 'old output' },
      },
    })

    testWindow.uamPush?.({
      type: 'cliOutput',
      sessionId: 'chat-1',
      sourceChatId: 'chat-1',
      terminalId: 'term-old',
      data: btoa('stale tail'),
    })
    const replacement = makeCppState(2, 'chat-1', { terminalId: 'term-new' })
    testWindow.uamPush?.({
      type: 'statePatch',
      data: {
        stateRevision: 2,
        chats: replacement.chats,
      },
    })
    await new Promise((resolve) => setTimeout(resolve, 120))

    expect(cefStore.getState().cliBindingBySessionId['chat-1']?.terminalId).toBe('term-new')
    expect(cefStore.getState().cliTranscriptBySessionId['chat-1']).toBeUndefined()
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
        statusLine: 'Failed to persist settings.',
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
    expect(state.statusLine).toBe('Failed to persist settings.')

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

    cefStore.setState({
      goalModeByChatId: { 'chat-1': true },
      defaultGoalTokenBudgetByChatId: { 'chat-1': 100 },
      markdownStoreAttachedBySessionId: { 'chat-1': [] },
      repositoryReviewBySessionId: { 'chat-1': {} as never },
      uamAgentsBySessionId: { 'chat-1': [] },
    })
    testWindow.uamPush?.({ type: 'cliOutput', sessionId: 'chat-1', terminalId: 'term-chat-1', data: btoa('late output') })
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: 'late token' })
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
    expect(state.goalModeByChatId['chat-1']).toBeUndefined()
    expect(state.defaultGoalTokenBudgetByChatId['chat-1']).toBeUndefined()
    expect(state.markdownStoreAttachedBySessionId['chat-1']).toBeUndefined()
    expect(state.repositoryReviewBySessionId['chat-1']).toBeUndefined()
    expect(state.uamAgentsBySessionId['chat-1']).toBeUndefined()
    await new Promise((resolve) => setTimeout(resolve, 80))
    expect(cefStore.getState().messages['chat-1']).toBeUndefined()
    expect(cefStore.getState().cliTranscriptBySessionId['chat-1']).toBeUndefined()
  })

  it('preserves provider-managed goal metadata across incremental patches', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    const providerGoal = {
      id: 'goal-provider',
      objective: 'Finish through the provider',
      status: 'active' as const,
      tokenBudget: 100,
      tokensUsed: 0,
      createdAt: '2026-01-01T00:00:00.000Z',
      updatedAt: '2026-01-01T00:00:00.000Z',
      executionOwner: 'provider' as const,
      providerCommand: '/goal',
    }
    initial.chats[0].activeGoalId = providerGoal.id
    initial.chats[0].goals = [providerGoal]
    testWindow.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(initial))

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    expect(cefStore.getState().goalsByChatId['chat-1'][0]).toMatchObject({
      executionOwner: 'provider',
      providerCommand: '/goal',
    })

    testWindow.uamPush?.({
      type: 'statePatch',
      data: {
        stateRevision: 2,
        chats: [{
          ...initial.chats[0],
          goals: [{
            ...providerGoal,
            tokensUsed: 12,
            updatedAt: '2026-01-01T00:00:02.000Z',
          }],
        }],
      },
    })

    expect(cefStore.getState().goalsByChatId['chat-1'][0]).toMatchObject({
      tokensUsed: 12,
      executionOwner: 'provider',
      providerCommand: '/goal',
    })
  })

  it('clears goals when an authoritative chat patch omits an empty goal list', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    const initial = makeCppState(1)
    initial.chats[0].goals = [{
      id: 'goal-complete',
      objective: 'Finished work',
      status: 'complete',
      tokenBudget: 0,
      tokensUsed: 0,
      createdAt: '2026-01-01T00:00:00.000Z',
      updatedAt: '2026-01-01T00:00:00.000Z',
    }]
    testWindow.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(initial))

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    expect(cefStore.getState().goalsByChatId['chat-1']).toHaveLength(1)

    const chatWithoutGoals = { ...initial.chats[0] }
    delete chatWithoutGoals.goals
    testWindow.uamPush?.({
      type: 'statePatch',
      data: { stateRevision: 2, chats: [chatWithoutGoals] },
    })

    expect(cefStore.getState().goalsByChatId['chat-1']).toEqual([])
  })

  it('continues streaming into an authoritative assistant message during an active ACP turn', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    testWindow.cefQuery = ({ onSuccess }) => {
      const initialState = makeCppState(1)
      initialState.chats.push({
        ...initialState.chats[0],
        id: 'chat-2',
        title: 'Background session',
        messages: [
          { role: 'assistant', content: 'First', createdAt: '2026-01-01T00:00:00.000Z' },
        ],
        acpSession: {
          sessionId: 'acp-chat-2',
          running: true,
          processing: true,
          lifecycleState: 'processing',
          turnAssistantMessageIndex: 0,
          turnSerial: 1,
        },
      })
      onSuccess(JSON.stringify(initialState))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))

    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-2', token: ' second' })
    await new Promise((resolve) => setTimeout(resolve, 80))

    const messages = cefStore.getState().messages['chat-2']
    expect(messages[messages.length - 1]).toMatchObject({
      content: 'First second',
      isStreaming: true,
    })
  })

	it('keeps buffered background tokens across a selected-chat full state update', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats.push({
      id: 'chat-2',
      title: 'Background chat',
      folderId: 'default',
      providerId: 'gemini-cli',
      createdAt: '2026-01-01T00:00:00.000Z',
      updatedAt: '2026-01-01T00:00:01.000Z',
      messageCount: 0,
      messagesDigest: '',
    })
    testWindow.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(initial))

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-2', token: 'Background answer' })
    testWindow.uamPush?.({
      type: 'stateUpdate',
      data: { ...initial, stateRevision: 2 },
    })
    await new Promise((resolve) => setTimeout(resolve, 80))

		expect(cefStore.getState().messages['chat-2']?.at(-1)?.content).toBe('Background answer')
	})

	it('accepts an authoritative empty selected transcript and drops buffered tokens', async () => {
		const testWindow = ensureTestWindow()
		vi.resetModules()
		testWindow.dispatchEvent = vi.fn(() => true)
		const initial = makeCppState(1)
		initial.chats[0].messages = [
			{ role: 'assistant', content: 'Stale answer', createdAt: '2026-01-01T00:00:00.000Z' },
		]
		testWindow.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(initial))

		const { useAppStore: cefStore } = await import('./useAppStore')
		await new Promise((resolve) => setTimeout(resolve, 0))
		testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: ' buffered' })
		testWindow.uamPush?.({
			type: 'stateUpdate',
			data: {
				...initial,
				stateRevision: 2,
				chats: [{ ...initial.chats[0], messages: [] }],
			},
		})
		await new Promise((resolve) => setTimeout(resolve, 80))

		expect(cefStore.getState().messages['chat-1']).toEqual([])
	})

	it('keeps buffered tokens when stale state payloads are rejected', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(2)
    initial.chats[0].messages = [
      { role: 'assistant', content: 'First', createdAt: '2026-01-01T00:00:00.000Z' },
    ]
    initial.chats[0].acpSession = {
      sessionId: 'acp-chat-1', running: true, processing: true, lifecycleState: 'processing',
      turnSerial: 1, turnUserMessageIndex: -1, turnAssistantMessageIndex: 0,
    }
    testWindow.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(initial))

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: ' second' })
    testWindow.uamPush?.({ type: 'stateUpdate', data: initial })
    await new Promise((resolve) => setTimeout(resolve, 80))
    expect(cefStore.getState().messages['chat-1'][0].content).toBe('First second')

    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: ' third' })
    testWindow.uamPush?.({
      type: 'statePatch',
      data: { stateRevision: 2, messagesByChatId: { 'chat-1': initial.chats[0].messages! } },
    })
    await new Promise((resolve) => setTimeout(resolve, 80))
    expect(cefStore.getState().messages['chat-1'][0].content).toBe('First second third')
  })

  it('removes a duplicate stream placeholder when its turn completes', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats[0].messages = [
      { role: 'user', content: 'Question', createdAt: '2026-01-01T00:00:00.000Z' },
      { role: 'assistant', content: 'Initial text.', createdAt: '2026-01-01T00:00:01.000Z' },
    ]
    initial.chats[0].acpSession = {
      sessionId: 'acp-chat-1',
      running: true,
      processing: true,
      lifecycleState: 'processing',
      turnSerial: 1,
      turnUserMessageIndex: 0,
      turnAssistantMessageIndex: 1,
      turnEvents: [
        { type: 'tool_call', toolCallId: 'tool-1' },
        { type: 'assistant_text', text: ' Final text.' },
      ],
    }
    testWindow.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(initial))

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    cefStore.setState((state) => ({
      messages: {
        ...state.messages,
        'chat-1': [
          ...state.messages['chat-1'],
          {
            id: 'stream-chat-1',
            sessionId: 'chat-1',
            role: 'assistant',
            content: ' Final text.',
            createdAt: new Date('2026-01-01T00:00:02.000Z'),
            isStreaming: true,
          },
        ],
      },
    }))

    testWindow.uamPush?.({ type: 'streamDone', chatId: 'chat-1' })

    expect(cefStore.getState().messages['chat-1']).toHaveLength(2)
    expect(cefStore.getState().messages['chat-1'][1]).toMatchObject({
      content: 'Initial text. Final text.',
    })
    expect(cefStore.getState().messages['chat-1'][1].isStreaming).toBe(false)
  })

  it('keeps a new turn from streaming into the previous assistant message', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats[0].messages = [
      { role: 'user', content: 'Old question', createdAt: '2026-01-01T00:00:00.000Z' },
      { role: 'assistant', content: 'First', createdAt: '2026-01-01T00:00:01.000Z' },
    ]
    initial.chats[0].messageCount = 2
    initial.chats[0].acpSession = {
      sessionId: 'acp-chat-1',
      running: true,
      processing: false,
      lifecycleState: 'ready',
      turnSerial: 1,
      turnUserMessageIndex: 0,
      turnAssistantMessageIndex: 1,
    }
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const action = (JSON.parse(request) as { action: string }).action
      if (action === 'getInitialState') {
        onSuccess(JSON.stringify(initial))
        return
      }
      testWindow.uamPush?.({
        type: 'statePatch',
        data: {
          stateRevision: 2,
          chats: [{
            ...initial.chats[0],
            messages: undefined,
            messageCount: 3,
            acpSession: {
              ...initial.chats[0].acpSession,
              processing: true,
              lifecycleState: 'processing',
              turnSerial: 2,
              turnUserMessageIndex: 2,
              turnAssistantMessageIndex: -1,
            },
          }],
        },
      })
      onSuccess('{}')
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    await expect(cefStore.getState().sendAcpPrompt('chat-1', 'New question')).resolves.toBe(true)
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: 'Second' })
    await new Promise((resolve) => setTimeout(resolve, 80))

    expect(cefStore.getState().messages['chat-1'].map((message) => message.content)).toEqual([
      'Old question',
      'First',
      'New question',
      'Second',
    ])
  })

  it('commits a continuous turn before streaming the next turn into a new bubble', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const requests: string[] = []
    const initial = makeCppState(1)
    initial.chats[0].messages = [
      { role: 'user', content: 'First question', createdAt: '2026-01-01T00:00:00.000Z' },
      { role: 'assistant', content: 'First', createdAt: '2026-01-01T00:00:01.000Z' },
    ]
    initial.chats[0].messageCount = 2
    initial.chats[0].messagesDigest = 'turn-1'
    initial.chats[0].acpSession = {
      sessionId: 'acp-chat-1',
      running: true,
      processing: true,
      lifecycleState: 'processing',
      turnSerial: 1,
      turnUserMessageIndex: 0,
      turnAssistantMessageIndex: 1,
    }
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request) as { action: string }
      requests.push(parsed.action)
      if (parsed.action === 'getInitialState') {
        onSuccess(JSON.stringify(initial))
        return
      }
      onSuccess(JSON.stringify({
        chatId: 'chat-1',
        messagesDigest: 'turn-1-committed',
        unchanged: false,
        messages: [
          { role: 'user', content: 'First question', createdAt: '2026-01-01T00:00:00.000Z' },
          { role: 'assistant', content: 'First tail', createdAt: '2026-01-01T00:00:01.000Z' },
        ],
      }))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: ' tail' })
    await new Promise((resolve) => setTimeout(resolve, 80))
    testWindow.uamPush?.({ type: 'streamDone', chatId: 'chat-1' })
    testWindow.uamPush?.({
      type: 'statePatch',
      data: {
        stateRevision: 2,
        chats: [{
          ...initial.chats[0],
          messages: undefined,
          messagesDigest: 'turn-2',
          acpSession: {
            ...initial.chats[0].acpSession,
            turnSerial: 2,
            turnUserMessageIndex: -1,
            turnAssistantMessageIndex: -1,
          },
        }],
      },
    })
    await new Promise((resolve) => setTimeout(resolve, 0))
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: 'Second' })
    await new Promise((resolve) => setTimeout(resolve, 80))

    expect(requests).toContain('getChatMessages')
    expect(cefStore.getState().messages['chat-1'].map((message) => message.content)).toEqual([
      'First question',
      'First tail',
      'Second',
    ])
  })

  it('keeps next-turn streamed text when continuous-turn hydration resolves late', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const hydrationSuccess: { current: ((response: string) => void) | null } = { current: null }
    const initial = makeCppState(1)
    initial.chats[0].messages = [
      { role: 'user', content: 'First question', createdAt: '2026-01-01T00:00:00.000Z' },
      { role: 'assistant', content: 'First answer', createdAt: '2026-01-01T00:00:01.000Z' },
    ]
    initial.chats[0].messageCount = 2
    initial.chats[0].messagesDigest = 'turn-1'
    initial.chats[0].acpSession = {
      sessionId: 'acp-chat-1',
      running: true,
      processing: true,
      lifecycleState: 'processing',
      turnSerial: 1,
      turnUserMessageIndex: 0,
      turnAssistantMessageIndex: 1,
    }
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const action = (JSON.parse(request) as { action: string }).action
      if (action === 'getInitialState') {
        onSuccess(JSON.stringify(initial))
        return
      }
      hydrationSuccess.current = onSuccess
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    testWindow.uamPush?.({
      type: 'statePatch',
      data: {
        stateRevision: 2,
        chats: [{
          ...initial.chats[0],
          messages: undefined,
          messageCount: 3,
          messagesDigest: 'turn-2',
          acpSession: {
            ...initial.chats[0].acpSession,
            turnSerial: 2,
            turnUserMessageIndex: 2,
            turnAssistantMessageIndex: -1,
          },
        }],
      },
    })
    await new Promise((resolve) => setTimeout(resolve, 0))

    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: 'Second answer' })
    await new Promise((resolve) => setTimeout(resolve, 80))
    expect(cefStore.getState().messages['chat-1'].at(-1)).toMatchObject({
      content: 'Second answer',
      isStreaming: true,
    })

    hydrationSuccess.current?.(JSON.stringify({
      chatId: 'chat-1',
      messagesDigest: 'turn-2-before-answer',
      unchanged: false,
      messages: [
        { role: 'user', content: 'First question', createdAt: '2026-01-01T00:00:00.000Z' },
        { role: 'assistant', content: 'First answer', createdAt: '2026-01-01T00:00:01.000Z' },
        { role: 'user', content: 'Queued question', createdAt: '2026-01-01T00:00:02.000Z' },
      ],
    }))
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(cefStore.getState().messages['chat-1'].at(-1)).toMatchObject({
      content: 'Second answer',
      isStreaming: true,
    })
  })

  it('hydrates an active queued turn after an empty assistant turn advances the serial', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const requests: string[] = []
    const initial = makeCppState(1)
    initial.chats[0].messages = [
      { role: 'user', content: 'First question', createdAt: '2026-01-01T00:00:00.000Z' },
    ]
    initial.chats[0].messageCount = 1
    initial.chats[0].messagesDigest = 'turn-1'
    initial.chats[0].acpSession = {
      sessionId: 'acp-chat-1',
      running: true,
      processing: true,
      lifecycleState: 'processing',
      turnSerial: 1,
      turnUserMessageIndex: 0,
      turnAssistantMessageIndex: -1,
    }
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const action = (JSON.parse(request) as { action: string }).action
      requests.push(action)
      if (action === 'getInitialState') {
        onSuccess(JSON.stringify(initial))
        return
      }
      onSuccess(JSON.stringify({
        chatId: 'chat-1',
        messagesDigest: 'turn-2',
        unchanged: false,
        messages: [
          { role: 'user', content: 'First question', createdAt: '2026-01-01T00:00:00.000Z' },
          { role: 'user', content: 'Queued question', createdAt: '2026-01-01T00:00:01.000Z' },
        ],
      }))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    testWindow.uamPush?.({
      type: 'statePatch',
      data: {
        stateRevision: 2,
        chats: [{
          ...initial.chats[0],
          messages: undefined,
          messageCount: 2,
          messagesDigest: 'turn-2',
          acpSession: {
            ...initial.chats[0].acpSession,
            turnSerial: 2,
            turnUserMessageIndex: 1,
            turnAssistantMessageIndex: -1,
          },
        }],
      },
    })
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(requests).toEqual(['getInitialState', 'getChatMessages'])
    expect(cefStore.getState().messages['chat-1'].map((message) => message.content)).toEqual([
      'First question',
      'Queued question',
    ])
  })

  it('refreshes the active transcript after a summary-only completion patch', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const requests: Array<{ action: string; payload?: { messagesDigest?: string } }> = []
    const initial = makeCppState(1)
    initial.chats[0].messages = [
      { role: 'user', content: 'Question', createdAt: '2026-01-01T00:00:00.000Z' },
    ]
    initial.chats[0].messagesDigest = 'before'
    initial.chats[0].acpSession = {
      sessionId: 'acp-chat-1',
      running: true,
      processing: true,
      lifecycleState: 'processing',
    }
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request) as { action: string; payload?: { messagesDigest?: string } }
      const { action } = parsed
      requests.push(parsed)
      if (action === 'getInitialState') {
        onSuccess(JSON.stringify(initial))
        return
      }
      if (parsed.payload?.messagesDigest === 'after') {
        onSuccess(JSON.stringify({ chatId: 'chat-1', messagesDigest: 'after', unchanged: true }))
        return
      }
      onSuccess(JSON.stringify({
        chatId: 'chat-1',
        messagesDigest: 'after',
        unchanged: false,
        messages: [
          { role: 'user', content: 'Question', createdAt: '2026-01-01T00:00:00.000Z' },
          { role: 'assistant', content: 'Final answer', createdAt: '2026-01-01T00:00:01.000Z' },
        ],
      }))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    testWindow.uamPush?.({
      type: 'statePatch',
      data: {
        stateRevision: 2,
        chats: [{
          ...initial.chats[0],
          messages: undefined,
          messagesDigest: 'after',
          messageCount: 2,
          acpSession: {
            ...initial.chats[0].acpSession,
            processing: false,
            lifecycleState: 'ready',
          },
        }],
      },
    })
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(requests.map((request) => request.action)).toEqual(['getInitialState', 'getChatMessages'])
    expect(requests[1].payload?.messagesDigest).toBe('')
    expect(cefStore.getState().messages['chat-1'].map((message) => message.content)).toEqual([
      'Question',
      'Final answer',
    ])
  })

  it('does not mistake a newer summary digest for a transcript already loaded in the browser', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const requestedDigests: string[] = []
    const initial = makeCppState(1)
    initial.chats[0].messages = [
      { role: 'user', content: 'First question', createdAt: '2026-01-01T00:00:00.000Z' },
      { role: 'assistant', content: 'First answer', createdAt: '2026-01-01T00:00:01.000Z' },
    ]
    initial.chats[0].messageCount = 2
    initial.chats[0].messagesDigest = 'before'
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request) as { action: string; payload?: { messagesDigest?: string } }
      if (parsed.action === 'getInitialState') {
        onSuccess(JSON.stringify(initial))
        return
      }

      const requestedDigest = parsed.payload?.messagesDigest ?? ''
      requestedDigests.push(requestedDigest)
      if (requestedDigest === 'after') {
        onSuccess(JSON.stringify({ chatId: 'chat-1', messagesDigest: 'after', unchanged: true }))
        return
      }
      onSuccess(JSON.stringify({
        chatId: 'chat-1',
        messagesDigest: 'after',
        unchanged: false,
        messages: [
          { role: 'user', content: 'First question', createdAt: '2026-01-01T00:00:00.000Z' },
          { role: 'assistant', content: 'First answer', createdAt: '2026-01-01T00:00:01.000Z' },
          { role: 'user', content: 'Middle question', createdAt: '2026-01-01T00:00:02.000Z' },
          { role: 'assistant', content: 'Latest answer', createdAt: '2026-01-01T00:00:03.000Z' },
        ],
      }))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    testWindow.uamPush?.({
      type: 'statePatch',
      data: {
        stateRevision: 2,
        chats: [{
          ...initial.chats[0],
          messages: undefined,
          messageCount: 4,
          messagesDigest: 'after',
        }],
      },
    })

    cefStore.getState().loadSessionMessages('chat-1')
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(requestedDigests).toEqual([''])
    expect(cefStore.getState().messages['chat-1'].map((message) => message.content)).toEqual([
      'First question',
      'First answer',
      'Middle question',
      'Latest answer',
    ])

    cefStore.getState().loadSessionMessages('chat-1')
    await new Promise((resolve) => setTimeout(resolve, 0))
    expect(requestedDigests).toEqual(['', 'after'])
  })

  it('lets a forced authoritative refresh replace a longer corrupt browser transcript', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats[0].messages = [
      { role: 'user', content: 'Start', createdAt: '2026-01-01T00:00:00.000Z' },
      { role: 'assistant', content: 'Ghost duplicate', createdAt: '2026-01-01T00:00:01.000Z' },
      { role: 'assistant', content: 'Another ghost', createdAt: '2026-01-01T00:00:02.000Z' },
      { role: 'assistant', content: 'End', createdAt: '2026-01-01T00:00:03.000Z' },
    ]
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const action = (JSON.parse(request) as { action: string }).action
      if (action === 'getInitialState') {
        onSuccess(JSON.stringify(initial))
        return
      }
      onSuccess(JSON.stringify({
        chatId: 'chat-1',
        messagesDigest: 'authoritative',
        unchanged: false,
        messages: [
          { role: 'user', content: 'Start', createdAt: '2026-01-01T00:00:00.000Z' },
          { role: 'assistant', content: 'Missing middle', createdAt: '2026-01-01T00:00:01.500Z' },
          { role: 'assistant', content: 'End', createdAt: '2026-01-01T00:00:03.000Z' },
        ],
      }))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    cefStore.getState().loadSessionMessages('chat-1', true)
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(cefStore.getState().messages['chat-1'].map((message) => message.content)).toEqual([
      'Start',
      'Missing middle',
      'End',
    ])
  })

  it('surfaces chat hydration failures instead of presenting an unexplained empty transcript', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    testWindow.cefQuery = ({ request, onSuccess, onFailure }) => {
      const action = (JSON.parse(request) as { action: string }).action
      if (action === 'getInitialState') {
        onSuccess(JSON.stringify(makeCppState(1)))
        return
      }
      if (action === 'getChatMessages') {
        onFailure(502, 'Remote transcript could not be read.')
      }
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    cefStore.getState().loadSessionMessages('chat-1')
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(cefStore.getState().statusLine).toBe(
      'Failed to load chat history for Gemini Session: Remote transcript could not be read.'
    )
  })

  it('ignores a chat hydration response after the transcript is unloaded', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    let finishHydration: ((payload: string) => void) | undefined
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const action = (JSON.parse(request) as { action: string }).action
      if (action === 'getInitialState') onSuccess(JSON.stringify(makeCppState(1)))
      else if (action === 'getChatMessages') finishHydration = onSuccess
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    cefStore.getState().loadSessionMessages('chat-1')
    cefStore.getState().unloadSessionMessages('chat-1')
    finishHydration?.(JSON.stringify({ chatId: 'chat-1', messagesDigest: 'late', unchanged: false, messages: [{ role: 'user', content: 'Late', createdAt: '2026-01-01T00:00:00.000Z' }] }))
    await new Promise((resolve) => setTimeout(resolve, 0))
    expect(cefStore.getState().messages['chat-1']).toBeUndefined()
  })

  it('discards buffered transcript pushes when an idle chat is unloaded', async () => {
    vi.useFakeTimers()
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    testWindow.cefQuery = ({ request, onSuccess }) => {
      if ((JSON.parse(request) as { action: string }).action === 'getInitialState') {
        const state = makeCppState(1)
        state.chats[0].messages = [{ role: 'assistant', content: 'Loaded' }]
        state.chats[0].cliTranscript = 'Loaded terminal'
        onSuccess(JSON.stringify(state))
      }
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await vi.runAllTimersAsync()
    testWindow.uamPush?.({ type: 'cliOutput', sessionId: 'chat-1', terminalId: 'term-1', data: btoa('late output') })
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: 'late token' })
    cefStore.getState().unloadSessionMessages('chat-1')
    await vi.runAllTimersAsync()
    expect(cefStore.getState().cliTranscriptBySessionId['chat-1']).toBeUndefined()
    expect(cefStore.getState().messages['chat-1']).toBeUndefined()
    vi.useRealTimers()
  })

  it('ignores a chat hydration response after the chat is deleted', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    let finishHydration: ((payload: string) => void) | undefined
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const action = (JSON.parse(request) as { action: string }).action
      if (action === 'getInitialState') onSuccess(JSON.stringify(makeCppState(1)))
      else if (action === 'getChatMessages') finishHydration = onSuccess
      else if (action === 'deleteSessions') onSuccess(JSON.stringify({ selectedChatId: null }))
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    cefStore.getState().loadSessionMessages('chat-1')
    await cefStore.getState().deleteSessions(['chat-1'])
    finishHydration?.(JSON.stringify({ chatId: 'chat-1', messagesDigest: 'late', unchanged: false, messages: [{ role: 'user', content: 'Late', createdAt: '2026-01-01T00:00:00.000Z' }] }))
    await new Promise((resolve) => setTimeout(resolve, 0))
    expect(cefStore.getState().sessions).toEqual([])
    expect(cefStore.getState().messages['chat-1']).toBeUndefined()
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

  it('preserves collection identity for a revision-only patch', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    testWindow.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(makeCppState(1)))

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const before = cefStore.getState()

    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2 } })

    expect(cefStore.getState().sessions).toBe(before.sessions)
    expect(cefStore.getState().goalsByChatId).toBe(before.goalsByChatId)
    expect(cefStore.getState().activeGoalIdByChatId).toBe(before.activeGoalIdByChatId)
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
        commandSafetyTier: 'off',
        memoryEnabled: true,
        memoryLevel: 'strict',
        smallModelMode: false,
        reasoningEffort: '',
        serviceTier: '',
      },
    })
  })

	it('requests a remote workspace when no matching workspace exists yet', async () => {
	  const requests: Array<{ action: string; payload?: unknown }> = []
	  window.cefQuery = ({ request, onSuccess }) => {
		requests.push(JSON.parse(request))
		onSuccess('{}')
	  }
	  useAppStore.setState({
		folders: [],
		providers: [{ id: 'opencode-cli', name: 'OpenCode', shortName: 'OpenCode', color: '#f97316', description: '' }],
	  })

	  await useAppStore.getState().addSession('Remote', null, 'opencode-cli', '', '', 'chat', 'lab', '/srv/project')

	  expect(requests).toHaveLength(1)
	  expect(requests[0]).toMatchObject({
		action: 'createSession',
		payload: { title: 'Remote', folderId: '', providerId: 'opencode-cli', executionHostId: 'lab', workspaceDirectory: '/srv/project' },
	  })
	})

	it('rejects a chat whose selected workspace belongs to another computer', async () => {
	  const requests: Array<{ action: string }> = []
	  window.cefQuery = ({ request, onSuccess }) => {
		requests.push(JSON.parse(request))
		onSuccess('{}')
	  }
	  useAppStore.setState({
		folders: [{ id: 'lab-workspace', name: 'Lab', parentId: null, directory: '/srv/project', executionHostId: 'lab', isExpanded: true, createdAt: new Date() }],
		providers: [{ id: 'opencode-cli', name: 'OpenCode', shortName: 'OpenCode', color: '#f97316', description: '' }],
	  })

	  await expect(useAppStore.getState().addSession('Wrong host', 'lab-workspace', 'opencode-cli', '', '', 'chat', 'local')).resolves.toBe(false)
	  expect(requests).toHaveLength(0)
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
      branchRootChatId: 'chat-family',
      branchFromMessageIndex: 2,
      branchMessageEdited: true,
    }
    useAppStore.getState().loadFromCef(state)
    expect(useAppStore.getState().sessions[0]).toMatchObject({
      parentChatId: 'chat-root',
      branchRootChatId: 'chat-family',
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
        commandSafetyTier: 'off',
        memoryEnabled: true,
        memoryLevel: 'strict',
        smallModelMode: false,
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
          commandSafetyTier: 'yolo',
          memoryEnabled: false,
          smallModelMode: true,
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
        commandSafetyTier: 'yolo',
        memoryEnabled: false,
        memoryLevel: 'off',
        smallModelMode: true,
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
      commandSafetyTier: 'yolo',
      memoryEnabled: false,
      smallModelMode: true,
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

  it('keeps newer session metadata when a pin change rolls back', async () => {
    const now = new Date('2026-01-01T00:00:00.000Z')
    const backendUpdatedAt = new Date('2026-01-01T00:00:01.000Z')
    let rejectPin: () => void = () => {
      throw new Error('CEF pin request was not sent')
    }
    window.cefQuery = ({ onFailure }) => {
      rejectPin = () => onFailure(500, 'save failed')
    }
    useAppStore.setState({
      sessions: [{
        id: 'chat-1',
        name: 'Original name',
        viewMode: 'chat',
        folderId: 'project',
        providerId: 'gemini-cli',
        isPinned: false,
        createdAt: now,
        updatedAt: now,
      }],
    })

    const resultPromise = useAppStore.getState().setSessionPinned('chat-1', true)
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1' ? {
        ...session,
        name: 'Renamed by backend',
        providerId: 'codex-cli',
        modelId: 'gpt-5',
        updatedAt: backendUpdatedAt,
      } : session),
    }))
    rejectPin()

    await expect(resultPromise).resolves.toBe(false)
    expect(useAppStore.getState().sessions[0]).toMatchObject({
      name: 'Renamed by backend',
      providerId: 'codex-cli',
      modelId: 'gpt-5',
      isPinned: false,
      updatedAt: backendUpdatedAt,
    })
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

  it('keeps newer workspace state when a provider switch rolls back', async () => {
    const now = new Date('2026-01-01T00:00:00.000Z')
    const workspaceUpdatedAt = new Date('2026-01-01T00:00:01.000Z')
    let rejectProvider: () => void = () => {
      throw new Error('provider request was not sent')
    }
    window.cefQuery = ({ onFailure }) => {
      rejectProvider = () => onFailure(500, 'Failed to save provider')
    }
    useAppStore.setState({
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#f97316', description: '' },
        { id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', color: '#22c55e', description: '' },
      ],
      sessions: [{
        id: 'chat-1',
        name: 'Chat',
        viewMode: 'chat',
        folderId: 'default',
        providerId: 'gemini-cli',
        modelId: 'gemini-old',
        smallModelMode: false,
        workspaceDirectory: '/tmp/source',
        createdAt: now,
        updatedAt: now,
      }],
      providerChatDefaults: {
        'codex-cli': {
          modelId: 'gpt-new',
          approvalMode: 'plan',
          commandSafetyTier: 'yolo',
          memoryEnabled: true,
          memoryLevel: 'strict',
          smallModelMode: true,
          reasoningEffort: 'high',
          serviceTier: 'fast',
        },
      },
      acpBindingBySessionId: {},
      cliBindingBySessionId: {},
    })

    const change = useAppStore.getState().setSessionProvider('chat-1', 'codex-cli')
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1'
        ? {
            ...session,
            workspaceDirectory: '/tmp/source/.uam-worktrees/chat-1',
            workspaceIsolationKind: 'gitWorktree',
            workspaceSourceDirectory: '/tmp/source',
            workspaceWorktreeDirectory: '/tmp/source/.uam-worktrees/chat-1',
            updatedAt: workspaceUpdatedAt,
          }
        : session),
    }))
    rejectProvider()

    await expect(change).resolves.toBe(false)
    expect(useAppStore.getState().sessions[0]).toMatchObject({
      providerId: 'gemini-cli',
      modelId: 'gemini-old',
      smallModelMode: false,
      workspaceDirectory: '/tmp/source/.uam-worktrees/chat-1',
      workspaceIsolationKind: 'gitWorktree',
      workspaceSourceDirectory: '/tmp/source',
      workspaceWorktreeDirectory: '/tmp/source/.uam-worktrees/chat-1',
      updatedAt: workspaceUpdatedAt,
    })
  })

  it('applies generic ACP reasoning and permission defaults', async () => {
    const now = new Date()
    window.cefQuery = ({ onSuccess }) => onSuccess('{}')
    useAppStore.setState({
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#f97316', description: '' },
        { id: 'opencode-cli', name: 'OpenCode', shortName: 'OpenCode', color: '#14b8a6', description: '' },
      ],
      sessions: [{
        id: 'chat-1',
        name: 'Chat',
        viewMode: 'chat',
        folderId: 'default',
        providerId: 'gemini-cli',
        commandSafetyTier: 'high',
        createdAt: now,
        updatedAt: now,
      }],
      providerChatDefaults: {
        'opencode-cli': {
          modelId: 'reasoner',
          approvalMode: 'plan',
          commandSafetyTier: 'low',
          memoryEnabled: true,
          memoryLevel: 'strict',
          reasoningEffort: 'high',
          serviceTier: 'fast',
        },
      },
      acpBindingBySessionId: {},
      cliBindingBySessionId: {},
    })

    await expect(useAppStore.getState().setSessionProvider('chat-1', 'opencode-cli')).resolves.toBe(true)
    expect(useAppStore.getState().sessions[0]).toMatchObject({
      providerId: 'opencode-cli',
      modelId: 'reasoner',
      reasoningEffort: 'high',
      serviceTier: '',
      approvalMode: 'plan',
      commandSafetyTier: 'aiReview',
    })
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
          reasoningEffort: 'high',
          serviceTier: 'flex',
          approvalMode: 'plan',
          commandSafetyTier: 'low',
          memoryLevel: 'balanced',
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

  it('reconciles reasoning and speed atomically when the selected model changes', async () => {
    const now = new Date()
    useAppStore.setState({
      sessions: [{
        id: 'chat-1',
        name: 'Codex Session',
        viewMode: 'chat',
        folderId: 'default',
        providerId: 'codex-cli',
        modelId: 'model-flex',
        reasoningEffort: 'ultra',
        serviceTier: 'flex',
        createdAt: now,
        updatedAt: now,
      }],
      acpBindingBySessionId: {
        'chat-1': {
          ...useAppStore.getState().acpBindingBySessionId['chat-1'],
          availableModels: [
            { id: 'model-flex', name: 'Flex', description: '', defaultReasoningEffort: 'high', supportedReasoningEfforts: ['high', 'ultra'], additionalSpeedTiers: ['fast', 'flex'] },
            { id: 'model-fast', name: 'Fast', description: '', defaultReasoningEffort: 'medium', supportedReasoningEfforts: ['low', 'medium', 'high'], additionalSpeedTiers: ['fast'] },
          ],
        },
      },
    })

    await expect(useAppStore.getState().setSessionModel('chat-1', 'model-fast')).resolves.toBe(true)
    expect(useAppStore.getState().sessions[0]).toMatchObject({
      modelId: 'model-fast',
      reasoningEffort: 'medium',
      serviceTier: '',
      serviceTierExplicit: true,
    })
  })

  it('protects an in-flight model change and accepts the canonical native result', async () => {
    const now = new Date()
    const cefSuccess: { current: ((response: string) => void) | null } = { current: null }
    window.cefQuery = ({ onSuccess }) => {
      cefSuccess.current = onSuccess
    }
    useAppStore.setState({
      sessions: [{
        id: 'chat-1',
        name: 'Codex Session',
        viewMode: 'chat',
        folderId: 'default',
        providerId: 'codex-cli',
        modelId: 'model-old',
        reasoningEffort: 'ultra',
        serviceTier: 'flex',
        createdAt: now,
        updatedAt: now,
      }],
      acpBindingBySessionId: {
        'chat-1': {
          availableModels: [{
            id: 'model-new',
            name: 'New',
            supportedReasoningEfforts: ['low', 'medium', 'high'],
            defaultReasoningEffort: 'medium',
            additionalSpeedTiers: ['fast'],
          }],
        },
      },
      lastAppliedStateRevision: -1,
    })

    const change = useAppStore.getState().setSessionModel('chat-1', 'model-new')
    const staleState = makeCppState(1)
    staleState.chats[0].providerId = 'codex-cli'
    staleState.chats[0].modelId = 'model-old'
    staleState.chats[0].reasoningEffort = 'ultra'
    staleState.chats[0].serviceTier = 'flex'
    useAppStore.getState().loadFromCef(staleState)
    expect(useAppStore.getState().sessions[0]).toMatchObject({
      modelId: 'model-new',
      reasoningEffort: 'medium',
      serviceTier: '',
      serviceTierExplicit: true,
    })

    cefSuccess.current?.(JSON.stringify({
      modelId: 'model-new',
      reasoningEffort: 'low',
      serviceTier: 'fast',
      serviceTierExplicit: true,
    }))
    await expect(change).resolves.toBe(true)
    expect(useAppStore.getState().sessions[0]).toMatchObject({
      modelId: 'model-new',
      reasoningEffort: 'low',
      serviceTier: 'fast',
      serviceTierExplicit: true,
    })
  })

  it('keeps explicit Standard distinct from inherited Codex speed', async () => {
    const now = new Date()
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify({
      reasoningEffort: 'medium',
      serviceTier: '',
    }))
    useAppStore.setState({
      sessions: [{
        id: 'chat-1',
        name: 'Codex Session',
        viewMode: 'chat',
        folderId: 'default',
        providerId: 'codex-cli',
        reasoningEffort: 'invalid',
        serviceTier: 'fast',
        createdAt: now,
        updatedAt: now,
      }],
    })

    await expect(useAppStore.getState().setSessionCodexOptions('chat-1', { serviceTier: '', serviceTierExplicit: true })).resolves.toBe(true)
    expect(useAppStore.getState().sessions[0]).toMatchObject({
      reasoningEffort: 'medium',
      serviceTier: '',
      serviceTierExplicit: true,
    })

    await expect(useAppStore.getState().setSessionCodexOptions('chat-1', { serviceTier: '', serviceTierExplicit: false })).resolves.toBe(true)
    expect(useAppStore.getState().sessions[0]).toMatchObject({ serviceTier: '', serviceTierExplicit: false })
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
          reasoningEffort: 'high',
          serviceTier: 'flex',
          approvalMode: 'plan',
          commandSafetyTier: 'low',
          memoryLevel: 'balanced',
          createdAt: now,
          updatedAt: now,
        },
      ],
    })

    await expect(useAppStore.getState().setSessionModel('chat-1', 'auto-gemini-3')).resolves.toBe(true)

    expect(requests).toHaveLength(1)
    expect(requests[0].action).toBe('setChatModel')
    expect(requests[0].payload).toEqual({ chatId: 'chat-1', modelId: 'auto-gemini-3' })
    expect(useAppStore.getState().sessions[0]).toMatchObject({
      modelId: 'auto-gemini-3',
      reasoningEffort: 'high',
      serviceTier: 'flex',
      approvalMode: 'plan',
      commandSafetyTier: 'low',
      memoryLevel: 'balanced',
    })
  })

  it('persists the per-chat reviewer model independently from the worker model', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess(JSON.stringify({ reviewerModelId: 'reviewer-smart' }))
    }
    useAppStore.setState({
      sessions: [{
        id: 'chat-1', name: 'Chat', viewMode: 'chat', folderId: 'default',
        modelId: 'worker-fast', reviewerModelId: 'reviewer-old', createdAt: new Date(), updatedAt: new Date(),
      }],
    })

    await expect(useAppStore.getState().setSessionReviewerModel('chat-1', ' reviewer-smart ')).resolves.toBe(true)

    expect(requests[0]).toMatchObject({
      action: 'setChatModel',
      payload: { chatId: 'chat-1', modelId: 'reviewer-smart', modelRole: 'reviewer' },
    })
    expect(useAppStore.getState().sessions[0]).toMatchObject({ modelId: 'worker-fast', reviewerModelId: 'reviewer-smart' })
  })

  it('keeps newer workspace state when a model change rolls back', async () => {
    const now = new Date('2026-01-01T00:00:00.000Z')
    const workspaceUpdatedAt = new Date('2026-01-01T00:00:01.000Z')
    let rejectModel: () => void = () => {
      throw new Error('model request was not sent')
    }
    window.cefQuery = ({ onFailure }) => {
      rejectModel = () => onFailure(409, 'Model is busy')
    }
    useAppStore.setState({
      sessions: [{
        id: 'chat-1',
        name: 'Chat',
        viewMode: 'chat',
        folderId: 'default',
        modelId: 'model-old',
        workspaceDirectory: '/tmp/source',
        createdAt: now,
        updatedAt: now,
      }],
    })

    const change = useAppStore.getState().setSessionModel('chat-1', 'model-new')
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1'
        ? {
            ...session,
            workspaceDirectory: '/tmp/source/.uam-worktrees/chat-1',
            workspaceIsolationKind: 'gitWorktree',
            workspaceSourceDirectory: '/tmp/source',
            workspaceWorktreeDirectory: '/tmp/source/.uam-worktrees/chat-1',
            updatedAt: workspaceUpdatedAt,
          }
        : session),
    }))
    rejectModel()

    await expect(change).resolves.toBe(false)
    expect(useAppStore.getState().sessions[0]).toMatchObject({
      modelId: 'model-old',
      workspaceDirectory: '/tmp/source/.uam-worktrees/chat-1',
      workspaceIsolationKind: 'gitWorktree',
      workspaceSourceDirectory: '/tmp/source',
      workspaceWorktreeDirectory: '/tmp/source/.uam-worktrees/chat-1',
      updatedAt: workspaceUpdatedAt,
    })
  })

  it('refuses Codex option changes at the store boundary while the runtime is processing', async () => {
    const requests: unknown[] = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }
    const state = makeCppState(1)
    state.chats[0].providerId = 'codex-cli'
    state.chats[0].reasoningEffort = 'medium'
    state.chats[0].serviceTier = 'flex'
    state.chats[0].acpSession = {
      sessionId: 'acp-chat-1',
      running: true,
      processing: true,
      lifecycleState: 'processing',
    }
    useAppStore.getState().loadFromCef(state)

    await expect(useAppStore.getState().setSessionCodexOptions('chat-1', {
      reasoningEffort: 'high',
      serviceTier: 'fast',
    })).resolves.toBe(false)

    expect(requests).toEqual([])
    expect(useAppStore.getState().sessions[0]).toMatchObject({ reasoningEffort: 'medium', serviceTier: 'flex' })
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
    expect(requests[0].payload).toEqual({ chatId: 'chat-1', reasoningEffort: 'high', serviceTier: 'fast', serviceTierExplicit: true })
    expect(useAppStore.getState().sessions[0].reasoningEffort).toBe('high')
    expect(useAppStore.getState().sessions[0].serviceTier).toBe('fast')

    await expect(useAppStore.getState().setSessionCodexOptions('chat-1', { reasoningEffort: 'medium' })).resolves.toBe(true)
    expect(requests[1].payload).toEqual({ chatId: 'chat-1', reasoningEffort: 'medium', serviceTier: 'fast', serviceTierExplicit: true })
    await expect(useAppStore.getState().setSessionCodexOptions('chat-1', { serviceTier: 'flex' })).resolves.toBe(true)
    expect(requests[2].payload).toEqual({ chatId: 'chat-1', reasoningEffort: 'medium', serviceTier: 'flex', serviceTierExplicit: true })

    rejectNext = true
    await expect(useAppStore.getState().setSessionCodexOptions('chat-1', {
      reasoningEffort: 'low',
      serviceTier: 'flex',
    })).resolves.toBe(false)

    expect(requests[3].payload).toEqual({ chatId: 'chat-1', reasoningEffort: 'low', serviceTier: 'flex', serviceTierExplicit: true })
    expect(useAppStore.getState().sessions[0].reasoningEffort).toBe('medium')
    expect(useAppStore.getState().sessions[0].serviceTier).toBe('flex')
  })

  it('sends Copilot launch-time reasoning without a Codex speed tier', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }
    useAppStore.setState({
      sessions: [{
        id: 'chat-copilot',
        name: 'Copilot Session',
        viewMode: 'chat',
        folderId: 'default',
        providerId: 'copilot-cli',
        modelId: 'gpt-5.1',
        reasoningEffort: 'medium',
        serviceTier: '',
        createdAt: new Date(),
        updatedAt: new Date(),
      }],
      acpBindingBySessionId: {
        'chat-copilot': {
          availableModels: [{
            id: 'gpt-5.1',
            name: 'GPT-5.1',
            supportedReasoningEfforts: ['low', 'medium', 'high', 'max'],
          }],
        } as AcpBinding,
      },
    })

    await expect(useAppStore.getState().setSessionCodexOptions('chat-copilot', {
      reasoningEffort: 'max',
      serviceTier: 'flex',
    })).resolves.toBe(true)

    expect(requests[0]).toMatchObject({
      action: 'setChatCodexOptions',
      payload: { chatId: 'chat-copilot', reasoningEffort: 'max', serviceTier: '', serviceTierExplicit: false },
    })
    expect(useAppStore.getState().sessions[0]).toMatchObject({ reasoningEffort: 'max', serviceTier: '' })
  })

  it('changes AI Review without altering model reasoning or speed', async () => {
    const now = new Date()
    window.cefQuery = ({ onSuccess }) => onSuccess('{}')
    useAppStore.setState({
      sessions: [{
        id: 'chat-1', name: 'Codex Session', viewMode: 'chat', folderId: 'default', providerId: 'codex-cli',
        reasoningEffort: 'xhigh', serviceTier: 'flex', commandSafetyTier: 'off', createdAt: now, updatedAt: now,
      }],
    })

    await expect(useAppStore.getState().setSessionCommandSafetyTier('chat-1', 'aiReview')).resolves.toEqual({ ok: true })
    expect(useAppStore.getState().sessions[0]).toMatchObject({
      commandSafetyTier: 'aiReview',
      reasoningEffort: 'xhigh',
      serviceTier: 'flex',
    })
  })

  it('hydrates persisted reasoning and speed into the visible session state', () => {
    const state = makeCppState(1)
    state.chats[0].providerId = 'codex-cli'
    state.chats[0].reasoningEffort = 'xhigh'
    state.chats[0].serviceTier = 'fast'

    useAppStore.getState().loadFromCef(state)

    expect(useAppStore.getState().sessions[0]).toMatchObject({
      providerId: 'codex-cli',
      reasoningEffort: 'xhigh',
      serviceTier: 'fast',
    })
  })

	it('waits for CEF confirmation and then accepts later authoritative Codex state', async () => {
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
      reasoningEffort: 'xhigh',
      serviceTier: 'fast',
    })

	expect(useAppStore.getState().sessions[0].reasoningEffort).toBe('medium')
	expect(useAppStore.getState().sessions[0].serviceTier).toBe('flex')

    expect(cefSuccess.current).toBeTruthy()
	cefSuccess.current?.('{"reasoningEffort":"xhigh","serviceTier":"fast"}')
    await expect(change).resolves.toBe(true)
	expect(useAppStore.getState().sessions[0].reasoningEffort).toBe('xhigh')
	expect(useAppStore.getState().sessions[0].serviceTier).toBe('fast')

    const staleState = makeCppState(1)
    staleState.chats[0].providerId = 'codex-cli'
    staleState.chats[0].reasoningEffort = 'medium'
    staleState.chats[0].serviceTier = 'flex'
    staleState.chats[0].commandSafetyTier = 'medium'
    useAppStore.getState().loadFromCef(staleState)

	expect(useAppStore.getState().sessions[0].reasoningEffort).toBe('medium')
	expect(useAppStore.getState().sessions[0].serviceTier).toBe('flex')

    const confirmedState = makeCppState(2)
    confirmedState.chats[0].providerId = 'codex-cli'
    confirmedState.chats[0].reasoningEffort = 'xhigh'
    confirmedState.chats[0].serviceTier = 'fast'
    confirmedState.chats[0].commandSafetyTier = 'medium'
    useAppStore.getState().loadFromCef(confirmedState)
    expect(useAppStore.getState().sessions[0].reasoningEffort).toBe('xhigh')
  })

	it('waits for CEF confirmation and then accepts later authoritative Copilot state', async () => {
    const now = new Date()
    const cefSuccess: { current: ((response: string) => void) | null } = { current: null }
    window.cefQuery = ({ onSuccess }) => {
      cefSuccess.current = onSuccess
    }
    useAppStore.setState({
      sessions: [
        {
          id: 'chat-1',
          name: 'Copilot Session',
          viewMode: 'chat',
          folderId: 'default',
          providerId: 'copilot-cli',
          reasoningEffort: 'medium',
          serviceTier: '',
          createdAt: now,
          updatedAt: now,
        },
      ],
      lastAppliedStateRevision: -1,
    })

    const change = useAppStore.getState().setSessionCodexOptions('chat-1', {
      reasoningEffort: 'xhigh',
    })

	expect(useAppStore.getState().sessions[0].reasoningEffort).toBe('medium')
	cefSuccess.current?.('{"reasoningEffort":"xhigh","serviceTier":""}')
    await expect(change).resolves.toBe(true)
	expect(useAppStore.getState().sessions[0].reasoningEffort).toBe('xhigh')

    const staleState = makeCppState(1)
    staleState.providers.push({ id: 'copilot-cli', name: 'GitHub Copilot CLI', shortName: 'Copilot', outputMode: 'cli' })
    staleState.chats[0].providerId = 'copilot-cli'
    staleState.chats[0].reasoningEffort = 'medium'
    useAppStore.getState().loadFromCef(staleState)

	expect(useAppStore.getState().sessions[0].reasoningEffort).toBe('medium')

    const confirmedState = makeCppState(2)
    confirmedState.providers.push({ id: 'copilot-cli', name: 'GitHub Copilot CLI', shortName: 'Copilot', outputMode: 'cli' })
    confirmedState.chats[0].providerId = 'copilot-cli'
    confirmedState.chats[0].reasoningEffort = 'xhigh'
    useAppStore.getState().loadFromCef(confirmedState)
    expect(useAppStore.getState().sessions[0].reasoningEffort).toBe('xhigh')
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
          commandSafetyTier: 'yolo',
          memoryEnabled: false,
          smallModelMode: true,
          reasoningEffort: 'xhigh',
          serviceTier: 'fast',
        },
        'gemini-cli': {
          modelId: 'gemini-3-pro-preview',
          approvalMode: 'plan',
          commandSafetyTier: 'medium',
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
        commandSafetyTier: 'yolo',
        memoryEnabled: false,
        memoryLevel: 'off',
        smallModelMode: true,
        reasoningEffort: 'xhigh',
        serviceTier: 'fast',
        reviewerModelId: '',
        featurePreference: 'uam' as const,
      },
      'gemini-cli': {
        modelId: 'gemini-3-pro-preview',
        approvalMode: 'plan',
        commandSafetyTier: 'aiReview',
        memoryEnabled: true,
        memoryLevel: 'strict',
        smallModelMode: false,
        reasoningEffort: '',
        serviceTier: '',
        reviewerModelId: '',
        featurePreference: 'uam' as const,
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
          commandSafetyTier: 'medium',
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

  it('preserves generic provider defaults advertised by the selected runtime model', async () => {
    const now = new Date()
    window.cefQuery = ({ onSuccess }) => onSuccess('{}')
    useAppStore.setState({
      providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#f97316', description: '' }],
      sessions: [{
        id: 'chat-1',
        name: 'Gemini Session',
        viewMode: 'chat',
        folderId: 'default',
        providerId: 'gemini-cli',
        createdAt: now,
        updatedAt: now,
      }],
      acpBindingBySessionId: {
        'chat-1': {
          availableModels: [{
            id: 'gemini-reasoner',
            name: 'Gemini Reasoner',
            supportedReasoningEfforts: ['low', 'high'],
          }],
        },
      },
    })

    await expect(useAppStore.getState().setProviderChatDefaults({
      defaultNewChatProviderId: 'gemini-cli',
      providerChatDefaults: {
        'gemini-cli': {
          modelId: 'gemini-reasoner',
          approvalMode: 'default',
          commandSafetyTier: 'medium',
          memoryEnabled: false,
          reasoningEffort: 'high',
          serviceTier: '',
        },
      },
    })).resolves.toBe(true)
    expect(useAppStore.getState().providerChatDefaults['gemini-cli'].reasoningEffort).toBe('high')
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
          commandSafetyTier: 'medium',
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
        commandSafetyTier: 'yolo',
        memoryEnabled: false,
        memoryLevel: 'off' as const,
        smallModelMode: false,
        reasoningEffort: 'high',
        serviceTier: 'fast',
        reviewerModelId: '',
        featurePreference: 'uam' as const,
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
        commandSafetyTier: 'medium',
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
        id: 'chat-1', name: 'Chat', viewMode: 'chat', folderId: 'default', commandSafetyTier: 'off', approvalMode: 'plan', reasoningEffort: 'high', serviceTier: 'flex', memoryLevel: 'balanced', createdAt: now, updatedAt: now,
      }],
    })

    await expect(useAppStore.getState().setSessionCommandSafetyTier('chat-1', 'aiReview')).resolves.toEqual({ ok: true })
    expect(requests[0]).toMatchObject({ action: 'setChatCommandSafetyTier', payload: { chatId: 'chat-1', commandSafetyTier: 'aiReview' } })
    expect(useAppStore.getState().sessions[0]).toMatchObject({ commandSafetyTier: 'aiReview', approvalMode: 'plan', reasoningEffort: 'high', serviceTier: 'flex', memoryLevel: 'balanced' })

    await expect(useAppStore.getState().setSessionCommandSafetyTier('chat-1', 'yolo')).resolves.toEqual({ ok: true })
    expect(requests[1]).toMatchObject({ payload: { commandSafetyTier: 'yolo' } })
    await expect(useAppStore.getState().setSessionCommandSafetyTier('chat-1', 'off')).resolves.toEqual({ ok: true })
    expect(requests[2]).toMatchObject({ payload: { commandSafetyTier: 'off' } })

    rejectNext = true
    await expect(useAppStore.getState().setSessionCommandSafetyTier('chat-1', 'aiReview')).resolves.toEqual({ ok: false, error: 'Safety tier failed' })
    expect(useAppStore.getState().sessions[0].commandSafetyTier).toBe('off')
  })

  it('ignores stale command safety responses', async () => {
    const now = new Date()
    const callbacks: Array<(response: string) => void> = []
    window.cefQuery = ({ onSuccess }) => callbacks.push(onSuccess)
    useAppStore.setState({
      sessions: [{
        id: 'chat-1', name: 'Chat', viewMode: 'chat', folderId: 'default', commandSafetyTier: 'off', createdAt: now, updatedAt: now,
      }],
    })

    const older = useAppStore.getState().setSessionCommandSafetyTier('chat-1', 'aiReview')
    const newer = useAppStore.getState().setSessionCommandSafetyTier('chat-1', 'yolo')
    callbacks[1](JSON.stringify({ commandSafetyTier: 'yolo' }))
    await expect(newer).resolves.toEqual({ ok: true })
    callbacks[0](JSON.stringify({ commandSafetyTier: 'aiReview' }))
    await expect(older).resolves.toEqual({ ok: false, cancelled: true })
    expect(useAppStore.getState().sessions[0].commandSafetyTier).toBe('yolo')
  })

  it('keeps newer workspace state when a rename rolls back', async () => {
    const now = new Date('2026-01-01T00:00:00.000Z')
    let rejectRename: () => void = () => {
      throw new Error('rename request was not sent')
    }
    window.cefQuery = ({ onFailure }) => {
      rejectRename = () => onFailure(500, 'Rename failed')
    }
    useAppStore.setState({
      sessions: [{
        id: 'chat-1',
        name: 'Original',
        viewMode: 'chat',
        folderId: 'default',
        workspaceDirectory: '/tmp/source',
        createdAt: now,
        updatedAt: now,
      }],
    })

    useAppStore.getState().renameSession('chat-1', 'Renamed')
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1'
        ? {
            ...session,
            workspaceDirectory: '/tmp/source/.uam-worktrees/chat-1',
            workspaceIsolationKind: 'gitWorktree',
            workspaceSourceDirectory: '/tmp/source',
            workspaceWorktreeDirectory: '/tmp/source/.uam-worktrees/chat-1',
          }
        : session),
    }))
    rejectRename()
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(useAppStore.getState().sessions[0]).toMatchObject({
      name: 'Original',
      workspaceDirectory: '/tmp/source/.uam-worktrees/chat-1',
      workspaceIsolationKind: 'gitWorktree',
      workspaceSourceDirectory: '/tmp/source',
      workspaceWorktreeDirectory: '/tmp/source/.uam-worktrees/chat-1',
    })
  })

  it.each([
    ['Codex options', () => useAppStore.getState().setSessionCodexOptions('chat-1', { reasoningEffort: 'high' }), false],
    ['approval mode', () => useAppStore.getState().setSessionApprovalMode('chat-1', 'plan'), false],
    ['command safety', () => useAppStore.getState().setSessionCommandSafetyTier('chat-1', 'aiReview'), { ok: false, error: 'Settings failed' }],
    ['memory level', () => useAppStore.getState().setSessionMemoryLevel('chat-1', 'off'), false],
  ])('keeps newer workspace state when %s rolls back', async (_label, changeSetting, expectedResult) => {
    const now = new Date('2026-01-01T00:00:00.000Z')
    let rejectChange: () => void = () => {
      throw new Error('settings request was not sent')
    }
    window.cefQuery = ({ onFailure }) => {
      rejectChange = () => onFailure(500, 'Settings failed')
    }
    useAppStore.setState({
      sessions: [{
        id: 'chat-1',
        name: 'Chat',
        viewMode: 'chat',
        folderId: 'default',
        providerId: 'codex-cli',
        modelId: 'gpt-5',
        reasoningEffort: 'low',
        serviceTier: 'flex',
        approvalMode: 'default',
        commandSafetyTier: 'medium',
        memoryEnabled: true,
        memoryLevel: 'strict',
        workspaceDirectory: '/tmp/source',
        createdAt: now,
        updatedAt: now,
      }],
      acpBindingBySessionId: {},
    })

    const change = changeSetting()
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1'
        ? {
            ...session,
            workspaceDirectory: '/tmp/source/.uam-worktrees/chat-1',
            workspaceIsolationKind: 'gitWorktree',
            workspaceSourceDirectory: '/tmp/source',
            workspaceWorktreeDirectory: '/tmp/source/.uam-worktrees/chat-1',
          }
        : session),
    }))
    rejectChange()

    await expect(change).resolves.toEqual(expectedResult)
    expect(useAppStore.getState().sessions[0]).toMatchObject({
      workspaceDirectory: '/tmp/source/.uam-worktrees/chat-1',
      workspaceIsolationKind: 'gitWorktree',
      workspaceSourceDirectory: '/tmp/source',
      workspaceWorktreeDirectory: '/tmp/source/.uam-worktrees/chat-1',
    })
  })

  it('sends planning mode changes when the live runtime mode differs from the saved chat mode', async () => {
    const now = new Date()
    const requests: Array<{ action: string; payload?: unknown }> = []
	let confirm: ((response: string) => void) | null = null
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
	  confirm = onSuccess
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

	const change = useAppStore.getState().setSessionApprovalMode('chat-1', 'default')
	await Promise.resolve()

    expect(requests).toHaveLength(1)
    expect(requests[0].action).toBe('setChatApprovalMode')
    expect(requests[0].payload).toEqual({ chatId: 'chat-1', modeId: 'default' })
	expect(useAppStore.getState().sessions[0].approvalMode).toBe('default')
	expect(useAppStore.getState().acpBindingBySessionId['chat-1'].currentModeId).toBe('plan')
	confirm?.('{"approvalMode":"default","currentModeId":"default"}')
	await expect(change).resolves.toBe(true)
    expect(useAppStore.getState().sessions[0].approvalMode).toBe('default')
    expect(useAppStore.getState().acpBindingBySessionId['chat-1'].currentModeId).toBe('default')
  })

  it('deletes a selected chat batch with one backend request and clears keyed state', async () => {
    const now = new Date()
    const requests: Array<{ action: string; payload: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{"selectedChatId":"chat-c"}')
    }
    useAppStore.setState({
      sessions: ['chat-a', 'chat-b', 'chat-c'].map((id) => ({ id, name: id, viewMode: 'chat' as const, folderId: 'default', createdAt: now, updatedAt: now })),
      activeSessionId: 'chat-a',
      messages: { 'chat-a': [], 'chat-b': [], 'chat-c': [] },
      cliBindingBySessionId: {},
      acpBindingBySessionId: {},
      cliTranscriptBySessionId: {
        'chat-a': { terminalId: 'one', content: 'one' },
        'chat-b': { terminalId: 'two', content: 'two' },
        'chat-c': { terminalId: 'three', content: 'three' },
      },
      markdownStoreAttachedBySessionId: { 'chat-a': [], 'chat-b': [], 'chat-c': [] },
    })

    await expect(useAppStore.getState().deleteSessions([' chat-a ', 'chat-b', 'chat-a'])).resolves.toBe(true)

    expect(requests).toHaveLength(1)
    expect(requests[0]).toMatchObject({ action: 'deleteSessions', payload: { chatIds: ['chat-a', 'chat-b'] } })
    expect(useAppStore.getState().sessions.map(({ id }) => id)).toEqual(['chat-c'])
    expect(useAppStore.getState().activeSessionId).toBe('chat-c')
    expect(Object.keys(useAppStore.getState().messages)).toEqual(['chat-c'])
    expect(Object.keys(useAppStore.getState().cliTranscriptBySessionId)).toEqual(['chat-c'])
    expect(Object.keys(useAppStore.getState().markdownStoreAttachedBySessionId)).toEqual(['chat-c'])
  })

  it('clears every keyed chat record after a single delete succeeds', async () => {
    const now = new Date()
    window.cefQuery = ({ onSuccess }) => onSuccess('{}')
    useAppStore.setState({
      sessions: [{ id: 'chat-a', name: 'Delete me', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now }],
      activeSessionId: 'chat-a',
      messages: {
        'chat-a': [{
          id: 'message-a',
          sessionId: 'chat-a',
          role: 'user',
          content: 'Attached',
          attachments: [{ id: 'attachment-a', name: 'file.txt', type: 'text/plain', size: 4 }],
          createdAt: now,
        }],
      },
      goalsByChatId: { 'chat-a': [] },
      activeGoalIdByChatId: { 'chat-a': null },
      goalModeByChatId: { 'chat-a': true },
      defaultGoalTokenBudgetByChatId: { 'chat-a': 100 },
      cliBindingBySessionId: {
        'chat-a': {
          terminalId: 'terminal-a',
          boundChatId: 'chat-a',
          running: false,
          lifecycleState: 'idle',
          turnState: 'idle',
          processing: false,
          readySinceLastSelect: false,
          active: false,
          lastError: '',
        },
      },
      acpBindingBySessionId: { 'chat-a': {} as never },
      cliTranscriptBySessionId: { 'chat-a': { terminalId: 'terminal-a', content: 'transcript' } },
      markdownStoreAttachedBySessionId: { 'chat-a': [] },
      repositoryReviewBySessionId: { 'chat-a': {} as never },
      uamAgentsBySessionId: { 'chat-a': [] },
    })

    useAppStore.getState().deleteSession('chat-a')
    await new Promise((resolve) => setTimeout(resolve, 0))

    const state = useAppStore.getState()
    expect(state.sessions).toEqual([])
    expect(state.activeSessionId).toBeNull()
    expect(Object.keys(state.messages)).toEqual([])
    expect(Object.keys(state.goalsByChatId)).toEqual([])
    expect(Object.keys(state.activeGoalIdByChatId)).toEqual([])
    expect(Object.keys(state.goalModeByChatId)).toEqual([])
    expect(Object.keys(state.defaultGoalTokenBudgetByChatId)).toEqual([])
    expect(Object.keys(state.cliBindingBySessionId)).toEqual([])
    expect(Object.keys(state.acpBindingBySessionId)).toEqual([])
    expect(Object.keys(state.cliTranscriptBySessionId)).toEqual([])
    expect(Object.keys(state.markdownStoreAttachedBySessionId)).toEqual([])
    expect(Object.keys(state.repositoryReviewBySessionId)).toEqual([])
    expect(Object.keys(state.uamAgentsBySessionId)).toEqual([])
  })

  it('retries a remote chat deletion after its runtime begins stopping', async () => {
    vi.useFakeTimers()
    const now = new Date()
    let attempts = 0
    window.cefQuery = ({ onSuccess, onFailure }) => {
      attempts += 1
      if (attempts === 1) {
        onFailure(409, 'The remote runtime is stopping. Retry deletion after it finishes.')
        return
      }
      onSuccess('{"selectedChatId":null,"deletedChatIds":["chat-a"]}')
    }
    useAppStore.setState({
      sessions: [{ id: 'chat-a', name: 'Remote chat', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now }],
      activeSessionId: 'chat-a',
      messages: { 'chat-a': [] },
    })

    const deletion = useAppStore.getState().deleteSession('chat-a')
    await vi.runAllTimersAsync()

    await expect(deletion).resolves.toBe(true)
    expect(attempts).toBe(2)
    expect(useAppStore.getState().sessions).toEqual([])
    vi.useRealTimers()
  })

  it('removes dependent chats returned by the authoritative delete response', async () => {
    const now = new Date()
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify({
      selectedChatId: null,
      deletedChatIds: ['chat-a', 'chat-dependent'],
    }))
    useAppStore.setState({
      sessions: [
        { id: 'chat-a', name: 'Owner', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
        { id: 'chat-dependent', name: 'Managed transcript', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
        { id: 'chat-keep', name: 'Keep', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
      ],
      activeSessionId: 'chat-a',
      messages: { 'chat-a': [], 'chat-dependent': [], 'chat-keep': [] },
    })

    await expect(useAppStore.getState().deleteSession('chat-a')).resolves.toBe(true)

    expect(useAppStore.getState().sessions.map(({ id }) => id)).toEqual(['chat-keep'])
    expect(Object.keys(useAppStore.getState().messages)).toEqual(['chat-keep'])
  })

  it('keeps a single chat and its keyed state mounted until deletion succeeds', async () => {
    const now = new Date()
    const previousStorage = Object.getOwnPropertyDescriptor(globalThis, 'localStorage')
    const removedDraftKeys: string[] = []
    Object.defineProperty(globalThis, 'localStorage', {
      configurable: true,
      value: { removeItem: (key: string) => removedDraftKeys.push(key) },
    })
    let finishDelete: () => void = () => { throw new Error('delete request was not sent') }
    window.cefQuery = ({ onSuccess }) => { finishDelete = () => onSuccess('{"selectedChatId":null}') }
    useAppStore.setState({
      sessions: [{ id: 'chat-a', name: 'Keep mounted', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now }],
      activeSessionId: 'chat-a',
      messages: { 'chat-a': [] },
    })

    const deletion = useAppStore.getState().deleteSession('chat-a')
    expect(useAppStore.getState().sessions.map(({ id }) => id)).toEqual(['chat-a'])
    expect(useAppStore.getState().messages).toHaveProperty('chat-a')
    expect(removedDraftKeys).toEqual([])
    finishDelete()
    await expect(deletion).resolves.toBe(true)
    expect(useAppStore.getState().sessions).toEqual([])
    expect(removedDraftKeys).toEqual([
      'uam-chat-composer-draft-v1:chat-a',
      'uam-terminal-steer-draft-v1:chat-a',
    ])
    if (previousStorage) Object.defineProperty(globalThis, 'localStorage', previousStorage)
    else Reflect.deleteProperty(globalThis, 'localStorage')
  })

  it('preserves a newer session selection when deletion is rejected', async () => {
    const now = new Date()
    let rejectDelete: () => void = () => {
      throw new Error('delete request was not sent')
    }
    const consoleSpy = vi.spyOn(console, 'error').mockImplementation(() => {})
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const action = (JSON.parse(request) as { action: string }).action
      if (action === 'deleteSessions') {
        rejectDelete = () => onFailure(409, 'Cannot delete while runtime is running')
        return
      }
      onSuccess('{}')
    }
    useAppStore.setState({
      sessions: [
        { id: 'chat-a', name: 'Delete me', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
        { id: 'chat-b', name: 'Optimistic fallback', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
        { id: 'chat-c', name: 'New selection', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
      ],
      activeSessionId: 'chat-a',
    })

    const deletion = useAppStore.getState().deleteSession('chat-a')
    useAppStore.getState().setActiveSession('chat-c')
    useAppStore.getState().setActiveSession('chat-b')
    expect(useAppStore.getState().activeSessionId).toBe('chat-b')

    rejectDelete()
    await expect(deletion).resolves.toBe(false)

    expect(useAppStore.getState().sessions.map(({ id }) => id)).toEqual(['chat-a', 'chat-b', 'chat-c'])
    expect(useAppStore.getState().activeSessionId).toBe('chat-b')
    consoleSpy.mockRestore()
  })

  it('preserves a newer session selection when deletion succeeds', async () => {
    const now = new Date()
    let finishDelete: () => void = () => { throw new Error('delete request was not sent') }
    window.cefQuery = ({ request, onSuccess }) => {
      const action = (JSON.parse(request) as { action: string }).action
      if (action === 'deleteSessions') {
        finishDelete = () => onSuccess('{"selectedChatId":"chat-b"}')
        return
      }
      onSuccess('{}')
    }
    useAppStore.setState({
      sessions: [
        { id: 'chat-a', name: 'Delete me', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
        { id: 'chat-b', name: 'Backend fallback', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
        { id: 'chat-c', name: 'New selection', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
      ],
      activeSessionId: 'chat-a',
    })

    const deletion = useAppStore.getState().deleteSession('chat-a')
    useAppStore.getState().setActiveSession('chat-c')
    expect(useAppStore.getState().activeSessionId).toBe('chat-c')
    finishDelete()
    await expect(deletion).resolves.toBe(true)

    expect(useAppStore.getState().sessions.map(({ id }) => id)).toEqual(['chat-b', 'chat-c'])
    expect(useAppStore.getState().activeSessionId).toBe('chat-c')
  })

  it('does not recreate deleted session state from a late ACP prompt response', async () => {
    const now = new Date()
    for (const outcome of ['success', 'failure'] as const) {
      let finishPrompt: () => void = () => { throw new Error('prompt request was not sent') }
      const consoleSpy = vi.spyOn(console, 'error').mockImplementation(() => {})
      window.cefQuery = ({ request, onSuccess, onFailure }) => {
        const action = (JSON.parse(request) as { action: string }).action
        if (action === 'sendAcpPrompt') {
          finishPrompt = () => outcome === 'success'
            ? onSuccess('{}')
            : onFailure(500, 'late failure')
        } else if (action === 'deleteSessions') {
          onSuccess('{"selectedChatId":null}')
        }
      }
      useAppStore.setState({
        sessions: [{ id: 'chat-late', name: 'Late', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now }],
        activeSessionId: 'chat-late',
        messages: { 'chat-late': [] },
        markdownStoreAttachedBySessionId: { 'chat-late': [] },
      })

      const sending = useAppStore.getState().sendAcpPrompt('chat-late', 'Do not resurrect')
      await useAppStore.getState().deleteSession('chat-late')
      finishPrompt()
      await expect(sending).resolves.toBe(false)
      expect(useAppStore.getState().messages).not.toHaveProperty('chat-late')
      expect(useAppStore.getState().acpBindingBySessionId).not.toHaveProperty('chat-late')
      expect(useAppStore.getState().markdownStoreAttachedBySessionId).not.toHaveProperty('chat-late')
      consoleSpy.mockRestore()
    }
  })

  it('rejects a deleted backend selection after deletion succeeds', async () => {
    const now = new Date()
    window.cefQuery = ({ onSuccess }) => onSuccess('{"selectedChatId":"chat-a"}')
    useAppStore.setState({
      sessions: [
        { id: 'chat-a', name: 'Delete me', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
        { id: 'chat-b', name: 'Keep me', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
      ],
      activeSessionId: 'chat-a',
    })

    await expect(useAppStore.getState().deleteSession('chat-a')).resolves.toBe(true)
    expect(useAppStore.getState().activeSessionId).toBe('chat-b')
  })

  it('preserves the selection after an unrelated session refresh during rejected delete', async () => {
    const now = new Date()
    let rejectDelete: () => void = () => {
      throw new Error('delete request was not sent')
    }
    const consoleSpy = vi.spyOn(console, 'error').mockImplementation(() => {})
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const action = (JSON.parse(request) as { action: string }).action
      if (action === 'deleteSessions') {
        rejectDelete = () => onFailure(409, 'Cannot delete while runtime is running')
        return
      }
      onSuccess('{}')
    }
    useAppStore.setState({
      sessions: [
        { id: 'chat-a', name: 'Delete me', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
        { id: 'chat-b', name: 'Optimistic fallback', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
      ],
      activeSessionId: 'chat-a',
    })

    const deletion = useAppStore.getState().deleteSession('chat-a')
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-b' ? { ...session, name: 'Refreshed fallback' } : session
      ),
    }))

    rejectDelete()
    await expect(deletion).resolves.toBe(false)

    expect(useAppStore.getState().sessions.map(({ id }) => id)).toEqual(['chat-a', 'chat-b'])
    expect(useAppStore.getState().activeSessionId).toBe('chat-a')
    consoleSpy.mockRestore()
  })

  it('keeps newer messages when a delete rolls back after a backend refresh', async () => {
    const now = new Date()
    let rejectDelete: () => void = () => {
      throw new Error('delete request was not sent')
    }
    window.cefQuery = ({ onFailure }) => {
      rejectDelete = () => onFailure(409, 'Delete failed')
    }
    useAppStore.setState({
      sessions: [{ id: 'chat-a', name: 'Chat', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now }],
      activeSessionId: 'chat-a',
      messages: {
        'chat-a': [{ id: 'old', sessionId: 'chat-a', role: 'assistant', content: 'Old message', createdAt: now }],
      },
    })

    useAppStore.getState().deleteSession('chat-a')
    useAppStore.setState({
      sessions: [{ id: 'chat-a', name: 'Refreshed chat', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now }],
      messages: {
        'chat-a': [{ id: 'new', sessionId: 'chat-a', role: 'assistant', content: 'New message', createdAt: now }],
      },
    })
    rejectDelete()
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(useAppStore.getState().sessions[0].name).toBe('Refreshed chat')
    expect(useAppStore.getState().messages['chat-a'][0].content).toBe('New message')
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

  it('sends the selected computer when creating a workspace', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess(JSON.stringify({
        id: 'remote-workspace',
        title: 'Containers',
        directory: '/opt/containers',
        executionHostId: 'homelab',
        collapsed: false,
      }))
    }

    await expect(useAppStore.getState().addFolder(
      'Containers', null, '/opt/containers', 'homelab',
    )).resolves.toBe(true)

    expect(requests).toEqual([expect.objectContaining({
      action: 'createFolder',
      payload: {
        title: 'Containers',
        directory: '/opt/containers',
        executionHostId: 'homelab',
      },
    })])
    expect(useAppStore.getState().folders[0]).toMatchObject({
      id: 'remote-workspace',
      executionHostId: 'homelab',
    })
  })

  it('cleans folder chat state when a removal push arrives before delete success', async () => {
    const now = new Date()
    const previousStorage = Object.getOwnPropertyDescriptor(globalThis, 'localStorage')
    const removedDraftKeys: string[] = []
    Object.defineProperty(globalThis, 'localStorage', {
      configurable: true,
      value: { removeItem: (key: string) => removedDraftKeys.push(key) },
    })
    useAppStore.setState({
      folders: [
        { id: 'default', name: 'General', parentId: null, directory: '/tmp/general', isExpanded: true, createdAt: now },
        { id: 'project', name: 'Project', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: now },
      ],
      sessions: [
        { id: 'chat-folder', name: 'Folder chat', viewMode: 'chat', folderId: 'project', createdAt: now, updatedAt: now },
        { id: 'chat-keep', name: 'Keep', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now },
      ],
      activeSessionId: 'chat-folder',
      messages: { 'chat-folder': [], 'chat-keep': [] },
    })
    window.cefQuery = ({ onSuccess }) => {
      useAppStore.setState((state) => ({
        sessions: state.sessions.filter((session) => session.id !== 'chat-folder'),
        activeSessionId: 'chat-keep',
      }))
      onSuccess('{}')
    }

    await expect(useAppStore.getState().deleteFolder('project')).resolves.toBe(true)

    expect(Object.keys(useAppStore.getState().messages)).toEqual(['chat-keep'])
    expect(removedDraftKeys).toEqual([
      'uam-chat-composer-draft-v1:chat-folder',
      'uam-terminal-steer-draft-v1:chat-folder',
    ])
    if (previousStorage) Object.defineProperty(globalThis, 'localStorage', previousStorage)
    else Reflect.deleteProperty(globalThis, 'localStorage')
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

  it('keeps a newer successful folder order when an older reorder fails', async () => {
    const callbacks: Array<{ succeed: () => void; fail: () => void }> = []
    window.cefQuery = ({ onSuccess, onFailure }) => {
      callbacks.push({
        succeed: () => onSuccess('{}'),
        fail: () => onFailure(500, 'Reorder failed'),
      })
    }
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

    const older = useAppStore.getState().reorderFolders(['three', 'one', 'two'])
    const newer = useAppStore.getState().reorderFolders(['two', 'three', 'one'])
    callbacks[1].succeed()
    callbacks[0].fail()
    await Promise.all([older, newer])

    expect(useAppStore.getState().folders.map((folder) => folder.id)).toEqual(['two', 'three', 'one'])
  })

  it('keeps newer folder expansion state when a rename rolls back', async () => {
    let rejectRename: () => void = () => {
      throw new Error('rename request was not sent')
    }
    window.cefQuery = ({ onFailure }) => {
      rejectRename = () => onFailure(500, 'Rename failed')
    }
    const createdAt = new Date()
    useAppStore.setState({
      folders: [{ id: 'one', name: 'Original', parentId: null, directory: '/tmp/one', isExpanded: true, createdAt }],
    })

    useAppStore.getState().renameFolder('one', 'Renamed', '/tmp/renamed')
    useAppStore.setState((state) => ({
      folders: state.folders.map((folder) => ({ ...folder, isExpanded: false })),
    }))
    rejectRename()
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(useAppStore.getState().folders[0]).toMatchObject({
      name: 'Original',
      directory: '/tmp/one',
      isExpanded: false,
    })
  })

  it('rescans chats for one workspace folder', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }

    await expect(useAppStore.getState().rescanFolderChats('project')).resolves.toBe(true)

    expect(requests).toHaveLength(1)
    expect(requests[0]).toMatchObject({
      action: 'rescanFolderChats',
      payload: { folderId: 'project' },
    })
  })

  it('does not report a native-history rescan as successful when the backend reports failure', async () => {
    window.cefQuery = ({ onSuccess }) => {
      onSuccess('{"success":false,"partial":false,"errors":["Could not read provider history"]}')
    }

    await expect(useAppStore.getState().rescanFolderChats('project')).resolves.toBe(false)
  })

  it('reports a partial history rescan as usable when some chats were imported', async () => {
    window.cefQuery = ({ onSuccess }) => {
      onSuccess('{"success":false,"partial":true,"errors":["One provider history was unavailable"]}')
    }

    await expect(useAppStore.getState().rescanFolderChats('project')).resolves.toBe(true)
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
    expect(state.sessions[0].commandSafetyTier).toBe('yolo')
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
    expect(state.theme).toBe('focus')
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
    cppState.chats[0].smallModelMode = true
    cppState.chats[0].uamControlEnabled = true
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
    expect(useAppStore.getState().sessions[0].smallModelMode).toBe(true)
    expect(useAppStore.getState().sessions[0].uamControlEnabled).toBe(true)
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
    expect(requests[0].payload).toEqual({ chatId: 'chat-1', enabled: true, memoryLevel: 'strict' })
    expect(useAppStore.getState().sessions[0].memoryEnabled).toBe(true)
    expect(useAppStore.getState().sessions[0].memoryLevel).toBe('strict')

    await expect(useAppStore.getState().setSessionSmallModelMode('chat-1', false)).resolves.toBe(true)
    expect(requests[1].action).toBe('setChatSmallModelMode')
    expect(requests[1].payload).toEqual({ chatId: 'chat-1', enabled: false })
    expect(useAppStore.getState().sessions[0].smallModelMode).toBe(false)

    await expect(useAppStore.getState().setSessionUamControlEnabled('chat-1', false)).resolves.toBe(true)
    expect(requests[2].action).toBe('setChatUamControlEnabled')
    expect(requests[2].payload).toEqual({ chatId: 'chat-1', enabled: false })
    expect(useAppStore.getState().sessions[0].uamControlEnabled).toBe(false)
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

  it('persists MCP environment references and rolls back a rejected update', async () => {
    const cppState = makeCppState(1)
    cppState.settings.mcpServers = []
    useAppStore.getState().loadFromCef(cppState)
    const server = {
      id: 'computer-use',
      name: 'Computer Use',
      workspaceDirectory: '/tmp/project',
      transport: 'http' as const,
      command: '',
      args: [],
      url: 'http://127.0.0.1:43123/mcp',
      environment: [],
      headers: [{ name: 'Authorization', environmentVariable: 'COMPUTER_USE_AUTH' }],
      enabled: true,
    }
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }

    await expect(useAppStore.getState().setMcpServers([server])).resolves.toEqual({ ok: true, error: undefined })
    expect(requests[0]).toMatchObject({ action: 'setMcpServers', payload: { servers: [server] } })
    expect(JSON.stringify(requests[0])).not.toContain('secret-value')

    window.cefQuery = ({ onFailure }) => onFailure(400, 'invalid MCP configuration')
    await expect(useAppStore.getState().setMcpServers([])).resolves.toEqual({ ok: false, error: 'invalid MCP configuration' })
    expect(useAppStore.getState().mcpServers).toEqual([server])
  })

  it('hydrates and persists ordered UAM agent preferences with rollback', async () => {
    const cppState = makeCppState(1)
    cppState.settings.favoriteUamAgentIds = ['writer', 'reviewer', 'writer', 'build']
    cppState.settings.uamAgentCycleShortcut = 'alt+shift+tab'
    useAppStore.getState().loadFromCef(cppState)
    expect(useAppStore.getState().favoriteUamAgentIds).toEqual(['writer', 'reviewer'])
    expect(useAppStore.getState().uamAgentCycleShortcut).toBe('alt+shift+tab')

    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess(JSON.stringify({ favoriteUamAgentIds: ['reviewer', 'writer'], uamAgentCycleShortcut: 'control+shift+tab' }))
    }
    await expect(useAppStore.getState().setUamAgentPreferences({
      favoriteUamAgentIds: ['reviewer', 'writer'],
      uamAgentCycleShortcut: 'control+shift+tab',
    })).resolves.toBe(true)
    expect(requests[0]).toMatchObject({
      action: 'setUamAgentPreferences',
      payload: { favoriteUamAgentIds: ['reviewer', 'writer'], uamAgentCycleShortcut: 'control+shift+tab' },
    })

    window.cefQuery = ({ onFailure }) => onFailure(500, 'save failed')
    await expect(useAppStore.getState().setUamAgentPreferences({
      favoriteUamAgentIds: [],
      uamAgentCycleShortcut: 'disabled',
    })).resolves.toBe(false)
    expect(useAppStore.getState()).toMatchObject({
      favoriteUamAgentIds: ['reviewer', 'writer'],
      uamAgentCycleShortcut: 'control+shift+tab',
    })
  })

  it('loads only primary-capable UAM agents for a chat catalog', async () => {
    useAppStore.getState().loadFromCef(makeCppState(1))
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess(JSON.stringify({ agents: [
        { id: 'build', description: 'Build', builtIn: true, mode: 'primary' },
        { id: 'reviewer', description: 'Review', builtIn: false, mode: 'both' },
        { id: 'helper', description: 'Help', builtIn: false, mode: 'subagent' },
      ] }))
    }

    await expect(useAppStore.getState().refreshUamAgents('chat-1')).resolves.toBe(true)
    expect(requests[0]).toMatchObject({ action: 'listUamAgents', payload: { chatId: 'chat-1' } })
    expect(useAppStore.getState().uamAgentsBySessionId['chat-1'].map((agent) => agent.id)).toEqual(['build', 'reviewer'])
  })

  it('reconciles and persists sidebar display settings through CEF', async () => {
    const cppState = makeCppState(1)
    cppState.settings.showProviderIconsInSidebar = false
    cppState.settings.showWorktreePathInSidebar = false
    useAppStore.getState().loadFromCef(cppState)

    expect(useAppStore.getState().showProviderIconsInSidebar).toBe(false)
    expect(useAppStore.getState().showWorktreePathInSidebar).toBe(false)

    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }

    await expect(useAppStore.getState().setSidebarSettings({
      showProviderIconsInSidebar: true,
      showWorktreePathInSidebar: false,
    })).resolves.toBe(true)

    expect(requests[0]).toMatchObject({
      action: 'setSidebarSettings',
      payload: { showProviderIconsInSidebar: true, showWorktreePathInSidebar: false },
    })
    expect(useAppStore.getState().showProviderIconsInSidebar).toBe(true)
    expect(useAppStore.getState().showWorktreePathInSidebar).toBe(false)
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
      groupPath: ['GitHub', 'Review'],
      acceptsFiles: true,
      acceptsFolders: false,
      enabled: true,
      openWorkspace: false,
    }]

    await expect(useAppStore.getState().setShellActions(actions)).resolves.toBe(true)
    await expect(useAppStore.getState().applyShellActions()).resolves.toBe(true)
    await expect(useAppStore.getState().dismissShellActionNotification()).resolves.toBeUndefined()

    expect(requests).toHaveLength(3)
    expect(requests[0]).toMatchObject({ action: 'setShellActions', payload: { actions } })
    expect(requests[1]).toMatchObject({ action: 'applyShellActions' })
    expect(requests[2]).toMatchObject({ action: 'dismissShellActionNotification' })
    expect(useAppStore.getState().shellActions).toEqual(actions)
    expect(useAppStore.getState().shellActionNotification).toBe('')
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
      acpSetupInactivityTimeoutSeconds: 999999,
      acpTurnOutputLimitMiB: 999999,
    })).resolves.toBe(true)

    expect(useAppStore.getState().memoryIdleDelaySeconds).toBe(30)
    expect(useAppStore.getState().memoryRecallBudgetBytes).toBe(8192)
    expect(useAppStore.getState().goalMaxLoopIterations).toBe(200)
    expect(useAppStore.getState().acpSetupInactivityTimeoutSeconds).toBe(3600)
    expect(useAppStore.getState().acpTurnOutputLimitMiB).toBe(4096)
    expect(requests[0].action).toBe('setMemorySettings')
    expect(requests[0].payload?.idleDelaySeconds).toBe(30)
    expect(requests[0].payload?.recallBudgetBytes).toBe(8192)
    expect(requests[0].payload?.goalMaxLoopIterations).toBe(200)
    expect(requests[0].payload?.acpSetupInactivityTimeoutSeconds).toBe(3600)
    expect(requests[0].payload?.acpTurnOutputLimitMiB).toBe(4096)
  })

  it('keeps a later memory setting when an older request fails', async () => {
    const callbacks: Array<{ succeed: () => void; fail: () => void }> = []
    window.cefQuery = ({ onSuccess, onFailure }) => {
      callbacks.push({
        succeed: () => onSuccess('{}'),
        fail: () => onFailure(500, 'save failed'),
      })
    }

    const older = useAppStore.getState().setMemorySettings({ memoryIdleDelaySeconds: 90 })
    const newer = useAppStore.getState().setMemorySettings({ memoryRecallBudgetBytes: 4096 })
    callbacks[1].succeed()
    callbacks[0].fail()
    await Promise.all([older, newer])

    expect(useAppStore.getState().memoryIdleDelaySeconds).toBe(60)
    expect(useAppStore.getState().memoryRecallBudgetBytes).toBe(4096)
  })

  it.each([
    {
      label: 'sidebar',
      prepare: () => useAppStore.setState({ showProviderIconsInSidebar: false, showWorktreePathInSidebar: false }),
      older: () => useAppStore.getState().setSidebarSettings({ showProviderIconsInSidebar: true, showWorktreePathInSidebar: false }),
      newer: () => useAppStore.getState().setSidebarSettings({ showProviderIconsInSidebar: false, showWorktreePathInSidebar: true }),
      assertLatest: () => expect(useAppStore.getState()).toMatchObject({ showProviderIconsInSidebar: false, showWorktreePathInSidebar: true }),
    },
    {
      label: 'update',
      prepare: () => useAppStore.setState({ updateChecksEnabled: false, updateLastCheckedAt: '', dismissedUpdateVersions: {} }),
      older: () => useAppStore.getState().setUpdateSettings({ updateChecksEnabled: true }),
      newer: () => useAppStore.getState().setUpdateSettings({ dismissedUpdateVersions: { 'codex-cli': '1.2.3' } }),
      assertLatest: () => expect(useAppStore.getState().dismissedUpdateVersions).toEqual({ 'codex-cli': '1.2.3' }),
    },
    {
      label: 'editor',
      prepare: () => useAppStore.setState({ defaultEditorPresetId: '', editorFileAssociations: [] }),
      older: () => useAppStore.getState().setEditorSettings({ defaultEditorPresetId: 'vscode', editorFileAssociations: [] }),
      newer: () => useAppStore.getState().setEditorSettings({
        defaultEditorPresetId: 'vscode',
        editorFileAssociations: [{ id: 'markdown', name: 'Markdown', extensions: ['md'], editorPresetId: 'vscode' }],
      }),
      assertLatest: () => expect(useAppStore.getState().editorFileAssociations).toEqual([
        { id: 'markdown', name: 'Markdown', extensions: ['.md'], editorPresetId: 'vscode' },
      ]),
    },
    {
      label: 'provider-default',
      prepare: () => useAppStore.setState({
        providers: [
          { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '', description: '' },
          { id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', color: '', description: '' },
        ],
        defaultNewChatProviderId: 'gemini-cli',
        providerChatDefaults: {
          'gemini-cli': { modelId: '', approvalMode: 'default', commandSafetyTier: 'medium', memoryEnabled: true, memoryLevel: 'strict', reasoningEffort: '', serviceTier: '' },
        },
      }),
      older: () => useAppStore.getState().setProviderChatDefaults({ defaultNewChatProviderId: 'codex-cli' }),
      newer: () => useAppStore.getState().setProviderChatDefaults({
        providerChatDefaults: {
          'gemini-cli': { modelId: '', approvalMode: 'default', commandSafetyTier: 'medium', memoryEnabled: false, memoryLevel: 'off', reasoningEffort: '', serviceTier: '' },
        },
      }),
      assertLatest: () => expect(useAppStore.getState().providerChatDefaults['gemini-cli'].memoryLevel).toBe('off'),
    },
  ] as Array<{
    label: string
    prepare: () => void
    older: () => Promise<boolean>
    newer: () => Promise<boolean>
    assertLatest: () => void
  }>)('keeps a newer successful $label setting when an older request fails', async ({ prepare, older, newer, assertLatest }) => {
    const callbacks: Array<{ succeed: () => void; fail: () => void }> = []
    window.cefQuery = ({ onSuccess, onFailure }) => {
      callbacks.push({
        succeed: () => onSuccess('{}'),
        fail: () => onFailure(500, 'save failed'),
      })
    }
    prepare()

    const olderRequest = older()
    const newerRequest = newer()
    callbacks[1].succeed()
    callbacks[0].fail()
    await Promise.all([olderRequest, newerRequest])

    assertLatest()
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

  it('keeps the most recently requested memory-library scope', async () => {
    const callbacks = new Map<string, (response: string) => void>()
    window.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      if (parsed.action === 'listMemoryEntries') {
        callbacks.set(parsed.payload.scopeType, onSuccess)
      } else {
        onSuccess('{}')
      }
    }
    useAppStore.setState({
      folders: [{
        id: 'project',
        name: 'Project',
        parentId: null,
        directory: '/tmp/project',
        isExpanded: true,
        createdAt: new Date(),
      }],
    })

    const older = useAppStore.getState().openGlobalMemoryLibrary()
    const newer = useAppStore.getState().openFolderMemoryLibrary('project')
    callbacks.get('folder')?.(JSON.stringify({
      scope: { scopeType: 'folder', folderId: 'project', label: 'Project', rootPath: '/tmp/project/.UAM' },
      entries: [],
    }))
    await Promise.resolve()
    callbacks.get('global')?.(JSON.stringify({
      scope: { scopeType: 'global', folderId: '', label: 'Global memory', rootPath: '/tmp/global-memory' },
      entries: [],
    }))
    await Promise.resolve()
    await Promise.all([older, newer])

    expect(useAppStore.getState().memoryLibraryScope).toMatchObject({
      scopeType: 'folder',
      folderId: 'project',
    })
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

  it('rejects failed VCS file diff responses instead of rendering the error as a diff', async () => {
    const testWindow = ensureTestWindow()
    testWindow.cefQuery = vi.fn(({ request, onSuccess }) => {
      const parsed = JSON.parse(request as string)
      expect(parsed).toMatchObject({
        action: 'getVcsFileDiff',
        payload: {
          chatId: 'chat-1',
          path: 'src/app.ts',
          vcsType: 'git',
          comparisonRef: 'a'.repeat(40),
        },
      })
      onSuccess?.(JSON.stringify({ ok: false, error: 'Bad comparison reference.' }))
    }) as TestWindow['cefQuery']

    await expect(useAppStore.getState().getVcsFileDiff(
      'chat-1',
      'src/app.ts',
      'git',
      'a'.repeat(40)
    )).rejects.toThrow('Bad comparison reference.')
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

  it.each(['open', 'refresh'])('ignores a late memory %s response after closing the library', async (operation) => {
    let finish: ((response: string) => void) | undefined
    ensureTestWindow().cefQuery = vi.fn(({ onSuccess }) => { finish = onSuccess }) as TestWindow['cefQuery']
    useAppStore.setState({ memoryLibraryScope: {scopeType:'all',folderId:'',label:'All memory',rootPath:''} })
    const pending = operation === 'open' ? useAppStore.getState().openAllMemoryLibrary() : useAppStore.getState().refreshMemoryLibrary()
    useAppStore.getState().closeMemoryLibrary()
    finish?.(JSON.stringify({ scope: {scopeType:'all',folderId:'',label:'All memory',rootPath:''}, entries: [] }))
    await expect(pending).resolves.toBe(false)
    expect(useAppStore.getState().memoryLibraryScope).toBeNull()
    expect(useAppStore.getState().memoryLibraryLoading).toBe(false)
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

  it('does not report a created memory as unsaved when refreshing the list fails', async () => {
    ensureTestWindow().cefQuery = vi.fn(({ request, onSuccess, onFailure }) => {
      if (JSON.parse(request as string).action === 'listMemoryEntries') onFailure?.(1, 'Read failed')
      else onSuccess?.('{}')
    }) as TestWindow['cefQuery']
    useAppStore.setState({memoryLibraryScope:{scopeType:'global',folderId:'',label:'Global memory',rootPath:'/tmp/memory'}})
    await expect(useAppStore.getState().createMemoryEntry({category:'Lessons/AI_Lessons',title:'Saved once',memory:'Keep the created entry.',evidence:'',confidence:'medium',sourceChatId:''})).resolves.toBe(true)
    expect(useAppStore.getState().memoryLibraryError).toBeTruthy()
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

  it('updates only a UAM goal objective and preserves progress', async () => {
    const createdAt = new Date('2026-01-01T00:00:00.000Z')
    const updatedAt = new Date('2026-01-02T00:00:00.000Z')
    useAppStore.setState({
      goalsByChatId: {
        'chat-1': [{
          id: 'goal-1', chatId: 'chat-1', objective: 'Old objective', status: 'active',
          completedItems: ['one'], remainingItems: ['two'], currentStep: 'two', loopCount: 4,
          createdAt, updatedAt, executionOwner: 'uam',
        }],
      },
      activeGoalIdByChatId: { 'chat-1': 'goal-1' },
    })

    await expect(useAppStore.getState().updateGoalObjective('chat-1', 'goal-1', '  New objective  ')).resolves.toEqual({ ok: true })
    expect(useAppStore.getState().goalsByChatId['chat-1'][0]).toMatchObject({
      objective: 'New objective', completedItems: ['one'], remainingItems: ['two'], currentStep: 'two', loopCount: 4,
      createdAt,
    })
    expect(useAppStore.getState().activeGoalIdByChatId['chat-1']).toBe('goal-1')

    useAppStore.setState((state) => ({
      goalsByChatId: {
        ...state.goalsByChatId,
        'chat-1': state.goalsByChatId['chat-1'].map((goal) => ({ ...goal, executionOwner: 'provider' as const })),
      },
    }))
    await expect(useAppStore.getState().updateGoalObjective('chat-1', 'goal-1', 'Provider edit')).resolves.toEqual({
      ok: false,
      error: 'Only non-complete UAM-managed goals can be edited.',
    })
    await expect(useAppStore.getState().updateGoalObjective('chat-1', 'goal-1', '   ')).resolves.toEqual({
      ok: false,
      error: 'Goal objective is required.',
    })
  })

  it('preserves goal mutation errors returned by CEF', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    ensureTestWindow().cefQuery = ({ request, onFailure }) => {
      requests.push(JSON.parse(request))
      onFailure(409, 'Goal is provider managed.')
    }

    await expect(useAppStore.getState().updateGoalObjective('chat-1', 'goal-1', 'New objective')).resolves.toEqual({
      ok: false,
      error: 'Goal is provider managed.',
    })
    expect(requests[0]).toMatchObject({
      action: 'updateGoalObjective',
      payload: { chatId: 'chat-1', goalId: 'goal-1', objective: 'New objective' },
    })
  })

  it('removes a persisted goal locally before its state patch arrives', async () => {
    const goal = {
      id: 'goal-1', chatId: 'chat-1', objective: 'Delete me', status: 'complete' as const,
      createdAt: new Date('2026-01-01T00:00:00.000Z'), updatedAt: new Date('2026-01-01T00:00:01.000Z'),
    }
    useAppStore.setState({
      goalsByChatId: { 'chat-1': [goal] },
      activeGoalIdByChatId: { 'chat-1': null },
    })
    ensureTestWindow().cefQuery = ({ onSuccess }) => onSuccess('{}')

    await expect(useAppStore.getState().removeGoal('chat-1', 'goal-1')).resolves.toEqual({ ok: true })
    expect(useAppStore.getState().goalsByChatId['chat-1']).toEqual([])
  })

  it('removes a goal when an unrelated state revision arrives first', async () => {
    const goal = {
      id: 'goal-1', chatId: 'chat-1', objective: 'Delete me', status: 'complete' as const,
      createdAt: new Date('2026-01-01T00:00:00.000Z'), updatedAt: new Date('2026-01-01T00:00:01.000Z'),
    }
    let finishMutation: () => void = () => { throw new Error('goal request was not sent') }
    ensureTestWindow().cefQuery = ({ onSuccess }) => { finishMutation = () => onSuccess('{}') }
    useAppStore.setState({
      lastAppliedStateRevision: 1,
      goalsByChatId: { 'chat-1': [goal] },
      activeGoalIdByChatId: { 'chat-1': null },
    })

    const mutation = useAppStore.getState().removeGoal('chat-1', 'goal-1')
    useAppStore.setState({ lastAppliedStateRevision: 2 })
    finishMutation()

    await expect(mutation).resolves.toEqual({ ok: true })
    expect(useAppStore.getState().goalsByChatId['chat-1']).toEqual([])
  })

  it('waits for a remote goal stop and deletes the goal from one action', async () => {
    vi.useFakeTimers()
    const goal = {
      id: 'goal-1', chatId: 'chat-1', objective: 'Delete me', status: 'active' as const,
      createdAt: new Date('2026-01-01T00:00:00.000Z'), updatedAt: new Date('2026-01-01T00:00:01.000Z'),
    }
    let attempts = 0
    ensureTestWindow().cefQuery = ({ onSuccess, onFailure }) => {
      attempts += 1
      if (attempts === 1) {
        onFailure(409, 'The remote turn is stopping.')
        return
      }
      onSuccess('{}')
    }
    useAppStore.setState({
      goalsByChatId: { 'chat-1': [goal] },
      activeGoalIdByChatId: { 'chat-1': 'goal-1' },
    })

    const mutation = useAppStore.getState().removeGoal('chat-1', 'goal-1')
    await vi.runAllTimersAsync()

    await expect(mutation).resolves.toEqual({ ok: true })
    expect(attempts).toBe(2)
    expect(useAppStore.getState().goalsByChatId['chat-1']).toEqual([])
    vi.useRealTimers()
  })

  it('reports a remote stop timeout after bounded retries', async () => {
    vi.useFakeTimers()
    const errorSpy = vi.spyOn(console, 'error').mockImplementation(() => {})
    let attempts = 0
    ensureTestWindow().cefQuery = ({ onFailure }) => {
      attempts += 1
      onFailure(409, 'The remote turn is stopping.')
    }

    const mutation = useAppStore.getState().removeGoal('chat-1', 'goal-1')
    await vi.runAllTimersAsync()

    await expect(mutation).resolves.toEqual({ ok: false, error: 'The remote stop timed out.' })
    expect(attempts).toBe(120)
    expect(errorSpy).toHaveBeenCalledTimes(1)
    expect(errorSpy).toHaveBeenCalledWith('[CEF] Error: The remote stop timed out.')
    vi.useRealTimers()
  })

  it('does not overwrite a newer goal patch when an older mutation response arrives', async () => {
    const goal = {
      id: 'goal-1', chatId: 'chat-1', objective: 'Keep current state', status: 'active' as const,
      createdAt: new Date('2026-01-01T00:00:00.000Z'), updatedAt: new Date('2026-01-01T00:00:01.000Z'),
    }
    let finishMutation: () => void = () => { throw new Error('goal request was not sent') }
    ensureTestWindow().cefQuery = ({ onSuccess }) => { finishMutation = () => onSuccess('{}') }
    useAppStore.setState({
      lastAppliedStateRevision: 1,
      goalsByChatId: { 'chat-1': [goal] },
      activeGoalIdByChatId: { 'chat-1': 'goal-1' },
    })

    const mutation = useAppStore.getState().updateGoalStatus('chat-1', 'goal-1', 'paused')
    useAppStore.setState({
      lastAppliedStateRevision: 2,
      goalsByChatId: { 'chat-1': [{ ...goal, status: 'blocked', lastBlocker: 'Newer backend state' }] },
      activeGoalIdByChatId: { 'chat-1': null },
    })
    finishMutation()
    await expect(mutation).resolves.toEqual({ ok: true })

    expect(useAppStore.getState().goalsByChatId['chat-1'][0]).toMatchObject({
      status: 'blocked',
      lastBlocker: 'Newer backend state',
    })
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
    expect(promptRequest?.payload?.steerNow).toBe(false)

    await expect(useAppStore.getState().sendAcpPrompt('chat-1', 'steer now', [], true)).resolves.toBe(true)
    expect(requests[requests.length - 1]?.payload?.steerNow).toBe(true)
  })

  it('requests non-blocking provider model discovery and marks the selector loading', async () => {
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    ensureTestWindow().cefQuery = ((params: { request: string; onSuccess?: (response: string) => void }) => {
      const parsed = JSON.parse(params.request)
      requests.push(parsed)
      params.onSuccess?.(JSON.stringify({ started: true, pending: true }))
    }) as unknown as TestWindow['cefQuery']
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], availableModels: [], modelsLoading: false },
      },
    }))

    await expect(useAppStore.getState().discoverProviderModels('chat-1')).resolves.toBe(true)
    expect(requests).toContainEqual(expect.objectContaining({ action: 'discoverProviderModels', payload: { chatId: 'chat-1' } }))
    expect(useAppStore.getState().acpBindingBySessionId['chat-1'].modelsLoading).toBe(true)
  })

	it('requests execution-host-scoped provider model discovery without a chat', async () => {
	const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
	ensureTestWindow().cefQuery = ((params: { request: string; onSuccess?: (response: string) => void }) => {
	  requests.push(JSON.parse(params.request))
	  params.onSuccess?.(JSON.stringify({ started: true, pending: true }))
	}) as unknown as TestWindow['cefQuery']

	await expect(useAppStore.getState().discoverProviderModels('', 'opencode-cli', '/srv/first-workspace', 'ssh-lab')).resolves.toBe(true)
	expect(requests).toContainEqual(expect.objectContaining({
	  action: 'discoverProviderModels',
	  payload: { chatId: '', providerId: 'opencode-cli', workspaceDirectory: '/srv/first-workspace', executionHostId: 'ssh-lab' },
	}))
  })

  it('sends only provider-offered ACP config values and waits for pushed confirmation', async () => {
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    ensureTestWindow().cefQuery = ((params: { request: string; onSuccess?: (response: string) => void }) => {
      requests.push(JSON.parse(params.request))
      params.onSuccess?.(JSON.stringify({ pending: true }))
    }) as unknown as TestWindow['cefQuery']
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          configOptions: [{
            id: 'thought_level', name: 'Thinking Style', description: '', category: '', currentValue: 'balanced.v2',
            options: [{ value: 'balanced.v2', name: 'Balanced', description: '' }, { value: 'deep/custom', name: 'Deep', description: '' }],
          }],
        },
      },
    }))

    await expect(useAppStore.getState().setAcpConfigOption('chat-1', 'thought_level', 'not-offered')).resolves.toBe(false)
    await expect(useAppStore.getState().setAcpConfigOption('chat-1', 'thought_level', 'deep/custom')).resolves.toBe(true)
    expect(requests).toEqual([expect.objectContaining({ action: 'setAcpConfigOption', payload: { chatId: 'chat-1', configId: 'thought_level', value: 'deep/custom' } })])
    expect(useAppStore.getState().acpBindingBySessionId['chat-1'].configOptions?.[0].currentValue).toBe('balanced.v2')
  })

  it('removes and prioritizes individual queued ACP prompts through one backend action', async () => {
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    ensureTestWindow().cefQuery = ((params: { request: string; onSuccess?: (response: string) => void }) => {
      requests.push(JSON.parse(params.request))
      params.onSuccess?.('{}')
    }) as unknown as TestWindow['cefQuery']

    await expect(useAppStore.getState().removeQueuedAcpPrompt('chat-1', 2)).resolves.toBe(true)
    await expect(useAppStore.getState().steerQueuedAcpPrompt('chat-1', 1)).resolves.toBe(true)

    expect(requests.map(({ action, payload }) => ({ action, payload }))).toEqual([
      { action: 'manageQueuedAcpPrompt', payload: { chatId: 'chat-1', operation: 'remove', index: 2 } },
      { action: 'manageQueuedAcpPrompt', payload: { chatId: 'chat-1', operation: 'steer', index: 1 } },
    ])
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
		  uamAgentId: 'build',
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

	expect(useAppStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts).toEqual([
	  { ...state.chats[0].acpSession.queuedPrompts![0], prioritySteer: undefined },
	])
    const requests: unknown[] = []
    ensureTestWindow().cefQuery = ((params: { request: string }) => {
      requests.push(JSON.parse(params.request))
    }) as unknown as TestWindow['cefQuery']

    await expect(useAppStore.getState().setSessionProvider('chat-1', 'codex-cli')).resolves.toBe(false)
    expect(requests).toEqual([])
  })

  it('resumes a goal through the runtime orchestration action', async () => {
    const requests: { action: string; payload?: any }[] = []
    ensureTestWindow().cefQuery = ((params: { request: string; onSuccess?: (response: string) => void }) => {
      const parsed = JSON.parse(params.request) as { action: string; payload?: any }
      requests.push(parsed)
      params.onSuccess?.('{}')
    }) as unknown as TestWindow['cefQuery']

    await expect(useAppStore.getState().resumeGoal('chat-1', 'goal-1')).resolves.toEqual({ ok: true })

    const resumeRequest = requests.find((request) => request.action === 'resumeGoal')
    expect(resumeRequest?.payload).toEqual({ chatId: 'chat-1', goalId: 'goal-1' })
  })

  it('routes enabled computer-use prompts and runtime controls through CEF', async () => {
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }
    const now = new Date()
    useAppStore.setState({
      sessions: [{
        id: 'chat-1', name: 'Chat', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now,
		computerUseEnabled: true, computerUseEffectiveBackend: 'uam', computerUseTargetId: '42', computerUseTargetTitle: 'Fixture',
		computerUse: { enabled: true, state: 'running', history: [] },
      }],
    })

	await expect(useAppStore.getState().setSessionComputerUseControl('chat-1', 'paused')).resolves.toEqual({ ok: true })
	await expect(useAppStore.getState().sendAcpPrompt('chat-1', 'Click Run')).resolves.toBe(true)

    expect(requests).toEqual([
	  expect.objectContaining({ action: 'setComputerUseControl', payload: { chatId: 'chat-1', state: 'paused' } }),
	  expect.objectContaining({ action: 'sendAcpPrompt', payload: expect.objectContaining({ text: 'Click Run', computerUseMode: true }) }),
    ])
  })

	it('keeps UAM computer use model-requested instead of user-enabled', async () => {
	  const requests: Array<{ action: string }> = []
	  window.cefQuery = ({ request, onSuccess }) => {
		requests.push(JSON.parse(request))
		onSuccess('{}')
	  }
	  const now = new Date()
	  useAppStore.setState({ sessions: [{
		id: 'chat-1', name: 'Chat', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now,
		computerUseEffectiveBackend: 'uam',
	  }] })

	  await expect(useAppStore.getState().setSessionComputerUseEnabled('chat-1', true)).resolves.toEqual({
		ok: false,
		error: 'Ask the AI to use Computer Use. UAM will ask you once to approve its chosen target.',
	  })
	  expect(requests).toEqual([])
	})

  it('keeps provider computer use targetless and blocks UAM-only pause controls', async () => {
    const requests: Array<{ action: string }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }
    const now = new Date()
    useAppStore.setState({
      sessions: [{
        id: 'chat-1', name: 'Chat', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now,
        computerUseEffectiveBackend: 'provider',
      }],
    })

    await expect(useAppStore.getState().setSessionComputerUseEnabled('chat-1', true)).resolves.toEqual({ ok: true })
    await expect(useAppStore.getState().setSessionComputerUseControl('chat-1', 'paused')).resolves.toEqual({ ok: false })
    expect(requests).toHaveLength(1)
  })
})
