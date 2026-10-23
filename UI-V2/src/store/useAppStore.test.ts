import { subagentParentFor, subagentRouteFor, clearSubagentView } from '../utils/subagentNavigation'
import { defaultChatGridLayout, readChatGridLayout, writeChatGridLayout } from '../utils/chatGridStorage'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import type { AcpBinding } from './cpp/types'
import type { Attachment, Message } from '../types/message'
import { CppAppState, useAppStore } from './useAppStore'
import { createUiSlice } from './slices/uiSlice'
import { parseUamPushPayload } from './push/uamPush'
import { sanitizeCppAppState, sanitizeCppStatePatch } from './cpp/sanitizers'
import { preferredFamilyChat, setPreferredFamilyChat } from '../utils/branchPreferenceStorage'

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

/** Explicit recovery prerequisite from an older renderer, never a generic ACK result. */
function legacySyntheticPrompt(content: string, attachments: Attachment[] = []): Message {
  return {
    id: `legacy-synthetic-${content}`,
    sessionId: 'chat-1',
    role: 'user',
    content,
    attachments,
    createdAt: new Date('2026-01-01T00:00:02.000Z'),
  }
}

function resetStore() {
  useAppStore.setState({
    folders: [],
    sessions: [],
    activeSessionId: null,
    lastAppliedStateRevision: -1,
    messages: {},
    historyStartIndexBySessionId: {},
    chatHistoryErrorBySessionId: {},
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
    vi.restoreAllMocks()
    const stored = new Map<string, string>()
    const storage: Storage = {
      get length() { return stored.size },
      clear: () => stored.clear(),
      getItem: (key) => stored.get(key) ?? null,
      setItem: (key, value) => { stored.set(key, String(value)) },
      removeItem: (key) => { stored.delete(key) },
      key: (index) => Array.from(stored.keys())[index] ?? null,
    }
    Object.defineProperty(testWindow, 'localStorage', { configurable: true, value: storage })
    Object.defineProperty(globalThis, 'localStorage', { configurable: true, value: storage })
    resetStore()
    delete testWindow.cefQuery
  })

  it('keeps loaded skills cached when the Skills window closes', () => {
    const entries = [{ id: 'github', title: 'GitHub', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', filePath: '/tmp/github.uam' }]
    useAppStore.setState({ isMarkdownStoreOpen: true, markdownStoreEntries: entries })

    useAppStore.getState().closeMarkdownStore()

    expect(useAppStore.getState().isMarkdownStoreOpen).toBe(false)
    expect(useAppStore.getState().markdownStoreEntries).toEqual(entries)
  })

  it.each(['complete', 'other chat', 'same chat', 'newer child'])('respects selection intent when child loading finishes: %s', async (intent) => {
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    const completions = new Map<string, (response: string) => void>()
    window.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (parsed.action === 'openNativeSessionChat') completions.set(parsed.payload.nativeSessionId, onSuccess)
      else onSuccess('{}')
    }
    const parent = { id: 'parent', name: 'Parent', viewMode: 'chat' as const, folderId: null, createdAt: new Date(), updatedAt: new Date() }
    useAppStore.setState({ sessions: [parent], activeSessionId: 'parent' })
    writeChatGridLayout({ ...defaultChatGridLayout, root: { type: 'leaf', id: 'leaf-1', sessionId: parent.id } })
    const opening = useAppStore.getState().openSubAgentSession('parent', 'native-first')
    if (intent === 'other chat') useAppStore.getState().setActiveSession('other')
    if (intent === 'same chat') useAppStore.getState().setActiveSession('parent')
    if (intent === 'newer child') {
      const newer = useAppStore.getState().openSubAgentSession('parent', 'native-second')
      completions.get('native-second')?.('{"chatId":"second"}')
      await newer
    }
    completions.get('native-first')?.('{"chatId":"first"}')
    expect(await opening).toBe(intent === 'complete' ? 'first' : null)
    expect(useAppStore.getState().activeSessionId).toBe(
      intent === 'other chat' ? 'other' : 'parent'
    )
    if (intent === 'complete' || intent === 'newer child') expect(subagentRouteFor(readChatGridLayout().activeLeafId, parent)).toMatchObject({ kind: 'native', chatId: intent === 'complete' ? 'first' : 'second' })
    clearSubagentView(readChatGridLayout().activeLeafId)
    expect(requests.filter((request) => request.action === 'openNativeSessionChat').every(
      (request) => request.payload?.selectChat === false && request.payload?.refreshHistory === false
    )).toBe(true)
  })

  it.each(['missing', 'recreated', 'provider', 'host', 'workspace'])('rejects a native child receipt after its parent identity changed: %s', async (change) => {
    const parent = { id: 'guarded-parent', name: 'Parent', providerId: 'codex-cli', executionHostId: 'local', viewMode: 'chat' as const, folderId: null, createdAt: new Date('2026-10-16T10:00:00Z'), updatedAt: new Date('2026-10-16T10:00:00Z') }
    useAppStore.setState({ sessions: [parent], activeSessionId: parent.id })
    let complete: ((response: string) => void) | undefined
    const actions: string[] = []
    window.cefQuery = ({ request, onSuccess }) => { actions.push(JSON.parse(request).action); complete = onSuccess }
    const opening = useAppStore.getState().openSubAgentSession(parent.id, 'guarded-native-child')
    useAppStore.setState({ sessions: change === 'missing' ? [] : [{ ...parent,
      ...(change === 'recreated' ? { createdAt: new Date(parent.createdAt.getTime() + 1) } : {}),
      ...(change === 'provider' ? { providerId: 'claude-cli' } : {}),
      ...(change === 'host' ? { executionHostId: 'other-host' } : {}),
      ...(change === 'workspace' ? { workspaceDirectory: '/foreign-workspace' } : {}),
    }] })
    complete?.('{"chatId":"guarded-child"}')
    expect(await opening).toBeNull()
    expect(actions).toEqual(['openNativeSessionChat'])
    const child = { ...parent, id: 'guarded-child' }
    expect(subagentParentFor(child, [parent, child])).toBeUndefined()
    expect(useAppStore.getState().activeSessionId).toBe(parent.id)
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

  it('receives startup import readiness through patches without changing the active chat or messages', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    testWindow.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(makeCppState(1)))
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const ready = { phase: 'ready' as const, generation: 'startup-fixture', totalCount: 3, importedCount: 0 }
    cefStore.getState().loadFromCef({ ...makeCppState(1), startupNativeImports: { ...ready, phase: 'scanning', totalCount: 0 } })
    const current = cefStore.getState()
    expect(testWindow.uamPush).toBeTypeOf('function')
    testWindow.uamPush!({ type: 'statePatch', data: { stateRevision: 2, startupNativeImports: ready } })
    expect(cefStore.getState().startupNativeImports).toEqual(ready)
    expect(cefStore.getState().activeSessionId).toBe(current.activeSessionId)
    expect(cefStore.getState().sessions).toBe(current.sessions)
    expect(cefStore.getState().messages).toBe(current.messages)
    testWindow.uamPush!({ type: 'statePatch', data: { stateRevision: 1, startupNativeImports: { ...ready, totalCount: 9 } } })
    expect(cefStore.getState().startupNativeImports).toEqual(ready)
  })

  it('reopens cached startup candidates when a workspace requests history review', async () => {
    const requests: string[] = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request).action)
      onSuccess(JSON.stringify({ success: true, reviewStartupImports: true }))
    }
    const previous = useAppStore.getState().startupNativeImportReviewSequence
    expect(await useAppStore.getState().rescanFolderChats('default')).toBe(true)
    expect(useAppStore.getState().startupNativeImportReviewSequence).toBe(previous + 1)
    expect(useAppStore.getState().startupNativeImportFolderId).toBe('default')
    expect(requests).toEqual(['rescanFolderChats'])
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

  it('preserves the sessions array during optimistic CEF selection and rollback', async () => {
    let failSelection: ((errorCode: number, errorMessage: string) => void) | undefined
    window.cefQuery = ({ request, onFailure }) => {
      if (JSON.parse(request).action === 'selectSession') failSelection = onFailure
    }
    const sessions = [
      { id: 'chat-1', name: 'Chat 1', viewMode: 'chat' as const, folderId: 'default', createdAt: new Date(), updatedAt: new Date() },
      { id: 'chat-2', name: 'Chat 2', viewMode: 'chat' as const, folderId: 'default', createdAt: new Date(), updatedAt: new Date() },
    ]
    useAppStore.setState({ sessions, activeSessionId: 'chat-1' })

    useAppStore.getState().setActiveSession('chat-2')
    expect(useAppStore.getState().activeSessionId).toBe('chat-2')
    expect(useAppStore.getState().sessions).toBe(sessions)

    expect(failSelection).toBeDefined()
    failSelection?.(1, 'Selection failed')
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(useAppStore.getState().activeSessionId).toBe('chat-1')
    expect(useAppStore.getState().sessions).toBe(sessions)
  })

  it('keeps the saved family choice when native selection is rejected', async () => {
    let failSelection: ((errorCode: number, errorMessage: string) => void) | undefined
    window.cefQuery = ({ request, onFailure }) => {
      if (JSON.parse(request).action === 'selectSession') failSelection = onFailure
    }
    const now = new Date()
    useAppStore.setState({
      sessions: [
        { id: 'root', name: 'Root', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now, branchRootChatId: 'root' },
        { id: 'branch-a', name: 'A', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now, parentChatId: 'root', branchRootChatId: 'root' },
        { id: 'branch-b', name: 'B', viewMode: 'chat', folderId: 'default', createdAt: now, updatedAt: now, parentChatId: 'root', branchRootChatId: 'root' },
      ],
      activeSessionId: 'root',
    })
    setPreferredFamilyChat('root', 'branch-a')

    useAppStore.getState().setActiveSession('branch-b')
    expect(preferredFamilyChat('root', ['root', 'branch-a', 'branch-b'])).toBe('branch-a')
    failSelection?.(1, 'Selection failed')
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(useAppStore.getState().activeSessionId).toBe('root')
    expect(preferredFamilyChat('root', ['root', 'branch-a', 'branch-b'])).toBe('branch-a')
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

  it('resolves a superseded Skills refresh as success without applying its stale result', async () => {
    const replies: Array<(response: string) => void> = []
    window.cefQuery = ({ request, onSuccess }) => {
      if (JSON.parse(request).action === 'listMarkdownStoreEntries') replies.push(onSuccess)
    }
    useAppStore.setState({ markdownStoreDirectory: '/tmp/start', markdownStoreEntries: [], markdownStoreError: '' })

    const older = useAppStore.getState().refreshMarkdownStore()
    const newer = useAppStore.getState().refreshMarkdownStore()
    replies[1](JSON.stringify({ ok: true, data: { directory: '/tmp/new', entries: [] } }))
    await expect(newer).resolves.toBe(true)
    replies[0](JSON.stringify({ ok: true, data: { directory: '/tmp/old', entries: [] } }))
    await expect(older).resolves.toBe(true)

    expect(useAppStore.getState().markdownStoreDirectory).toBe('/tmp/new')
    expect(useAppStore.getState().markdownStoreError).toBe('')
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

  it('persists working display preferences and expands traces by default', () => {
    expect(createUiSlice(vi.fn(), () => useAppStore.getState(), false).expandWorkTraces).toBe(true)
    expect(createUiSlice(vi.fn(), () => useAppStore.getState(), false).collapsibleWorkSections).toBe(true)
    useAppStore.getState().setWorkingDisplayMode('compact')
    useAppStore.getState().setExpandWorkTraces(false)
    useAppStore.getState().setCollapsibleWorkSections(false)

    expect(useAppStore.getState().workingDisplayMode).toBe('compact')
    expect(JSON.parse(window.localStorage.getItem('uam-app-shell-layout-v2') ?? '{}')).toMatchObject({
      workingDisplayMode: 'compact',
      expandWorkTraces: false,
      collapsibleWorkSections: false,
    })
    expect(createUiSlice(vi.fn(), () => useAppStore.getState(), false).workingDisplayMode).toBe('compact')
    expect(createUiSlice(vi.fn(), () => useAppStore.getState(), false).expandWorkTraces).toBe(false)
    expect(createUiSlice(vi.fn(), () => useAppStore.getState(), false).collapsibleWorkSections).toBe(false)
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

  it('restarts the ACP clock on a new turn even when the idle snapshot was coalesced', () => {
    const clock = vi.spyOn(Date, 'now').mockReturnValue(1000)
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
    expect(firstStartedAt).toBe(1000)

    clock.mockReturnValue(6000)
    useAppStore.getState().loadFromCef({ ...firstState, stateRevision: 2 })
    expect(useAppStore.getState().acpBindingBySessionId['chat-1'].processingStartedAtMs).toBe(firstStartedAt)

    const secondState = makeCppState(3)
    secondState.chats[0].acpSession = {
      ...(firstState.chats[0].acpSession ?? {}),
      turnSerial: 2,
    }
    useAppStore.getState().loadFromCef(secondState)

    const secondBinding = useAppStore.getState().acpBindingBySessionId['chat-1']
    expect(secondBinding.turnSerial).toBe(2)
    expect(secondBinding.processingStartedAtMs).toBe(6000)

    clock.mockReturnValue(9000)
    useAppStore.getState().loadFromCef({
      ...secondState,
      stateRevision: 4,
      chats: [{ ...secondState.chats[0], acpSession: {
        ...secondState.chats[0].acpSession,
        lifecycleState: 'waitingPermission',
        processing: false,
      } }],
    })
    expect(useAppStore.getState().acpBindingBySessionId['chat-1'].processingStartedAtMs).toBe(6000)
    clock.mockRestore()
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
    testWindow.dispatchEvent = vi.fn(() => true)
    testWindow.uamPush?.({
      type: 'cliOutput', sessionId: 'chat-1', sourceChatId: 'chat-1',
      terminalId: 'term-old', data: btoa('late stale output'),
    })
    expect(testWindow.dispatchEvent).not.toHaveBeenCalled()
    await new Promise((resolve) => setTimeout(resolve, 120))

    expect(cefStore.getState().cliBindingBySessionId['chat-1']?.terminalId).toBe('term-new')
    expect(cefStore.getState().cliTranscriptBySessionId['chat-1']).toBeUndefined()
  })

  it('rejects old launch output when the terminal ID is reused, including buffered chunks', async () => {
    vi.useFakeTimers()
    try {
      const testWindow = ensureTestWindow()
      vi.resetModules()
      testWindow.dispatchEvent = vi.fn(() => true)
      const initial = makeCppState(1)
      Object.assign(initial.chats[0].cliTerminal!, { startupTime: 1 })
      testWindow.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(initial))
      const { useAppStore: cefStore } = await import('./useAppStore')
      await vi.runAllTimersAsync()
      const output = (startupTime: number | undefined, text: string) => testWindow.uamPush?.({
        type: 'cliOutput', sessionId: 'chat-1', sourceChatId: 'chat-1',
        terminalId: 'term-chat-1', startupTime, data: btoa(text),
      })
      output(1, 'buffered old output')
      const replacement = makeCppState(2)
      Object.assign(replacement.chats[0].cliTerminal!, { startupTime: 2 })
      testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, chats: replacement.chats } })
      testWindow.dispatchEvent = vi.fn(() => true)
      output(1, 'late old output')
      output(undefined, 'unowned output')
      expect(testWindow.dispatchEvent).not.toHaveBeenCalled()
      output(2, 'current output')
      const stopped = makeCppState(3, 'chat-1', { running: false })
      Object.assign(stopped.chats[0].cliTerminal!, { startupTime: 2 })
      testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 3, chats: stopped.chats } })
      await vi.runAllTimersAsync()
      expect(cefStore.getState().cliBindingBySessionId['chat-1']).toMatchObject({ startupTime: 2 })
      expect(cefStore.getState().cliTranscriptBySessionId['chat-1']?.content).toBe('current output')
    } finally { vi.useRealTimers() }
  })

  it('preserves remote CLI versions and routes refresh/install without changing local payloads', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.cliVersionManager = { providers: [], remoteProviders: [{
      providerId: 'codex-cli', executionHostId: 'alpha', executionHostName: 'Alpha',
      installedVersion: '1.0.0', selectedVersion: '', availableVersions: [], preferredVersion: 'latest',
      status: 'verified', message: '', running: false, lastCommand: '', lastOutput: '', lastInstallStatus: 'awaiting-verification',
    }] }
    testWindow.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(initial))
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    expect(cefStore.getState().cliVersionManager.remoteProviders?.[0]).toMatchObject({ executionHostId: 'alpha', executionHostName: 'Alpha', providerId: 'codex-cli', lastInstallStatus: 'awaiting-verification' })
    const requests: { action: string; payload?: unknown }[] = []
    testWindow.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request)); onSuccess('{}') }
    await cefStore.getState().refreshCliProviderVersion('codex-cli', 'alpha')
    await cefStore.getState().applyCliProviderVersion('codex-cli', 'latest', 'alpha')
    await cefStore.getState().applyCliProviderVersion('codex-cli', 'latest')
    await cefStore.getState().refreshCliProviderVersion('unknown-provider', 'beta')
    expect(requests.map((request) => request.payload)).toEqual([
      { providerId: 'codex-cli', executionHostId: 'alpha' },
      { providerId: 'codex-cli', version: 'latest', executionHostId: 'alpha', allowInteractiveElevation: true },
      { providerId: 'codex-cli', version: 'latest', allowInteractiveElevation: true },
      { providerId: 'unknown-provider', executionHostId: 'beta' },
    ])
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, cliVersionManager: { providers: [], remoteProviders: [] } } })
    expect(cefStore.getState().cliVersionManager).toMatchObject({ providers: [], remoteProviders: [] })
  })

  it('preserves unknown native activity across full state, patches, and binding updates', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats[0].cliTerminal = {
      terminalId: 'term-chat-1', sourceChatId: 'chat-1', running: true,
      lifecycleState: 'unknown', turnState: 'unknown', processing: false,
      active: false, readySinceLastSelect: false, lastError: '',
    }
    testWindow.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(initial))
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const expectUnknown = () => expect(cefStore.getState().cliBindingBySessionId['chat-1']).toMatchObject({
      running: true, lifecycleState: 'unknown', turnState: 'unknown', processing: false, active: false,
    })
    expectUnknown()
    cefStore.getState().setCliBinding('chat-1', { lifecycleState: 'busy', turnState: 'busy', processing: true, active: true })
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, chats: [initial.chats[0]] } })
    expectUnknown()
    cefStore.getState().setCliBinding('chat-1', { lifecycleState: 'busy', turnState: 'busy', processing: true, active: true })
    cefStore.getState().setCliBinding('chat-1', { lifecycleState: 'unknown', turnState: 'unknown' })
    expectUnknown()
    cefStore.getState().setCliBinding('chat-1', { lastError: 'Input rejected' })
    expectUnknown()
    cefStore.getState().setCliBinding('chat-1', { lifecycleState: 'busy', turnState: 'busy', processing: true })
    cefStore.getState().setCliBinding('chat-1', { turnState: 'unknown' })
    expectUnknown()
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
            pendingContinuation: true,
            tokensUsed: 12,
            updatedAt: '2026-01-01T00:00:02.000Z',
          }],
        }],
      },
    })

    expect(cefStore.getState().goalsByChatId['chat-1'][0]).toMatchObject({
      tokensUsed: 12,
      pendingContinuation: true,
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

  it('bootstraps desktop summaries and streams into a bounded long-chat tail without sparse history', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats[0].cliTerminal = undefined
    delete initial.chats[0].messages
    initial.chats[0].messageCount = 10000
    initial.chats[0].messagesDigest = 'long'
    initial.chats[0].acpSession = { sessionId: 'live', running: true, processing: true,
      turnAssistantMessageIndex: 9999, turnSerial: 1 }
    const requests: Array<{ action: string; payload: Record<string, unknown> }> = []
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request); requests.push(parsed)
      if (parsed.action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else if (parsed.action === 'getChatMessages') onSuccess(JSON.stringify({ chatId: 'chat-1',
        startIndex: 9950, totalCount: 10000, messagesDigest: 'long', messages: Array.from({ length: 50 }, (_, index) => ({
          role: 'assistant', content: `saved ${9950 + index}`, createdAt: '2026-01-01T00:00:00Z',
        })) }))
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise(resolve => setTimeout(resolve, 0))
    expect(requests[0]).toMatchObject({ action: 'getInitialState', payload: { summaryOnly: true } })
    expect(cefStore.getState().messages['chat-1']).toBeUndefined()
    await cefStore.getState().loadSessionMessages('chat-1')
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', messageIndex: 9999, token: ' continued' })
    await new Promise(resolve => setTimeout(resolve, 80))
    expect(cefStore.getState().messages['chat-1']).toHaveLength(50)
    expect(cefStore.getState().messages['chat-1'].at(-1)).toMatchObject({ content: 'saved 9999 continued', nativeMessageIndex: 9999 })
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, chats: [{ ...initial.chats[0],
      messageCount: 10001, acpSession: { ...initial.chats[0].acpSession, turnAssistantMessageIndex: 10000 },
    }] } })
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', messageIndex: 10000, token: 'next assistant' })
    await new Promise(resolve => setTimeout(resolve, 80))
    expect(cefStore.getState().messages['chat-1']).toHaveLength(51)
    expect(cefStore.getState().messages['chat-1'].at(-1)?.content).toBe('next assistant')
    expect(cefStore.getState().historyStartIndexBySessionId['chat-1']).toBe(9950)
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

  it.each(['new', 'reattached', 'failed'] as const)('preserves %s skill attachments across pending prompt acknowledgement', async (scenario) => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    const initial = makeCppState(1)
    initial.chats[0].messages = []
    initial.chats[0].messageCount = 0
    initial.chats[0].acpSession = { sessionId: 'native', running: true, processing: false, lastError: '' }
    let finish = () => { throw new Error('Prompt request not started') }
    const requests: Array<{ payload: { markdownStoreFiles: string[] } }> = []
    testWindow.cefQuery = ({ request, onSuccess, onFailure }) => {
      const parsed = JSON.parse(request)
      if (parsed.action !== 'sendAcpPrompt') { onSuccess(JSON.stringify(initial)); return }
      requests.push(parsed)
      finish = () => scenario === 'failed' ? onFailure(500, 'Admission failed') : onSuccess('{}')
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const sent = { id: 'sent', title: 'Sent skill', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', filePath: '/skills/sent.md' }
    const added = { ...sent, id: 'next', filePath: '/skills/next.md' }
    cefStore.getState().attachMarkdownStoreEntry('chat-1', sent)
    const sending = cefStore.getState().sendAcpPrompt('chat-1', 'Use the skill')
    expect(requests[0].payload.markdownStoreFiles).toEqual([sent.filePath])
    if (scenario === 'reattached') {
      cefStore.getState().detachMarkdownStoreEntry('chat-1', sent.filePath)
      cefStore.getState().attachMarkdownStoreEntry('chat-1', sent)
    } else cefStore.getState().attachMarkdownStoreEntry('chat-1', added)
    finish()
    await expect(sending).resolves.toBe(scenario !== 'failed')
    expect(cefStore.getState().markdownStoreAttachedBySessionId['chat-1'].map((entry) => entry.filePath)).toEqual(
      scenario === 'reattached' ? [sent.filePath] : scenario === 'failed' ? [sent.filePath, added.filePath] : [added.filePath]
    )
  })

  it.each(['held-before-ack', 'ack-before-held'] as const)('keeps a recovered prompt outside history across %s without fabricated admission rows', async (order) => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    const initial = makeCppState(1)
    initial.chats[0].messageCount = 0
    initial.chats[0].messagesDigest = 'empty'
    initial.chats[0].acpSession = { sessionId: '', running: false, processing: false }
    let acknowledge = () => { throw new Error('Send not started') }
    let hydrate: (() => void) | undefined
    let historyRequests = 0
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const action = JSON.parse(request).action
      if (action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else if (action === 'sendAcpPrompt') acknowledge = () => onSuccess('{}')
      else if (action === 'getChatMessages') {
        historyRequests += 1
        hydrate = () => onSuccess(JSON.stringify({ chatId: 'chat-1', messages: [], startIndex: 0, totalCount: 0, messagesDigest: 'empty', unsentMessageIndices: [], recoverableUnsentMessageIndices: [] }))
      } else onSuccess('{}')
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const attachment = { id: 'image', name: 'image.png', type: 'image/png', size: 1, path: '/staged/image.png' }
    const sending = cefStore.getState().sendAcpPrompt('chat-1', 'Keep this input', [attachment])
    const held = { ...initial.chats[0], messages: undefined, acpSession: { ...initial.chats[0].acpSession, queueHeld: true,
      queuedPrompts: [{ id: 'real-outbox', revision: 1, text: 'Keep this input', attachments: [attachment] }] } }
    if (order === 'ack-before-held') {
      acknowledge()
      await expect(sending).resolves.toBe(true)
      expect(cefStore.getState().messages['chat-1']).toEqual([])
    }
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, chats: [held] } })
    if (order === 'held-before-ack') {
      acknowledge()
      await expect(sending).resolves.toBe(true)
      expect(cefStore.getState().messages['chat-1']).toEqual([])
    }
    // Fresh ACK and a held native summary both leave history empty; no opaque mismatch exists to hydrate.
    expect(cefStore.getState().messages['chat-1']).toEqual([])
    expect(historyRequests).toBe(0)
    expect(hydrate).toBeUndefined()
    expect(cefStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts).toEqual([expect.objectContaining({ id: 'real-outbox', revision: 1, text: 'Keep this input', attachments: [expect.objectContaining(attachment)] })])
  })

  it('does not associate an edited pre-existing queue entry with a newer acknowledged input', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    const initial = makeCppState(1)
    initial.chats[0].messageCount = 0
    initial.chats[0].messagesDigest = 'empty'
    initial.chats[0].acpSession = { sessionId: '', running: false, processing: false, queueHeld: true,
      queuedPrompts: [{ id: 'already-held', revision: 1, text: 'Repeat' }] }
    let acknowledge = () => { throw new Error('Send not started') }
    testWindow.cefQuery = ({ request, onSuccess }) => {
      if (JSON.parse(request).action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else acknowledge = () => onSuccess('{}')
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const sending = cefStore.getState().sendAcpPrompt('chat-1', 'Repeat')
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, chats: [{ ...initial.chats[0], messages: undefined,
      acpSession: { ...initial.chats[0].acpSession, queuedPrompts: [{ id: 'already-held', revision: 2, text: 'Repeat' }] } }] } })
    acknowledge()
    await expect(sending).resolves.toBe(true)
    // Admission of a new same-text input cannot create history or claim the existing queue entry.
    expect(cefStore.getState().messages['chat-1']).toEqual([])
    expect(cefStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts).toEqual([
      expect.objectContaining({ id: 'already-held', revision: 2, text: 'Repeat' }),
    ])
  })

  it('forces history for a legacy opaque held mismatch when the chat is loaded later', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    const initial = makeCppState(1)
    initial.chats[0].messageCount = 0
    initial.chats[0].messagesDigest = 'empty'
    initial.chats[0].acpSession = { sessionId: '', running: false, processing: false }
    const digests: string[] = []
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      if (parsed.action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else if (parsed.action === 'getChatMessages') {
        digests.push(parsed.payload.messagesDigest)
        onSuccess(JSON.stringify(parsed.payload.messagesDigest === 'empty' ? { chatId: 'chat-1', unchanged: true } :
          { chatId: 'chat-1', messages: [], startIndex: 0, totalCount: 0, messagesDigest: 'empty' }))
      } else onSuccess('{}')
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    await cefStore.getState().loadSessionMessages('chat-1')
    await cefStore.getState().sendAcpPrompt('chat-1', 'Recovered while away')
    expect(cefStore.getState().messages['chat-1']).toEqual([])
    // Seed the old renderer's opaque projection explicitly; ACK alone cannot satisfy the mismatch guard.
    cefStore.setState({ messages: { 'chat-1': [legacySyntheticPrompt('Recovered while away')] } })
    cefStore.getState().loadFromCef({ ...initial, stateRevision: 2, chats: [{ ...initial.chats[0], messages: undefined,
      acpSession: { ...initial.chats[0].acpSession, queueHeld: true, queuedPrompts: [{ id: 'held-away', revision: 1, text: 'Recovered while away' }] } }] })
    expect(cefStore.getState().messages['chat-1']).toHaveLength(1)
    await cefStore.getState().loadSessionMessages('chat-1')
    expect(digests).toEqual(['', ''])
    expect(cefStore.getState().messages['chat-1']).toEqual([])
  })

  it.each(['none', 'ack-before-removal', 'removal-before-ack'] as const)('retires a proven referenced raw tail atomically while preserving a later legacy optimistic attempt (%s)', async (order) => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    const initial = makeCppState(1)
    initial.chats[0].messageCount = 3
    initial.chats[0].messagesDigest = 'three'
    initial.chats[0].messages = [
      { role: 'user', content: 'Delivered', createdAt: '' }, { role: 'assistant', content: 'Reply', createdAt: '' },
      { role: 'user', content: 'Referenced unsent', acpPromptNotSent: true, createdAt: '' },
    ]
    initial.chats[0].acpSession = { sessionId: 'native', running: true, processing: false, queueHeld: true,
      unsentMessageIndices: [2], recoverableUnsentMessageIndices: [], queuedPrompts: [{ id: 'referenced', revision: 1,
        existingUserMessageIndex: 2, text: 'Referenced unsent', uamAgentId: 'read-only', attachments: [], markdownStoreFiles: [], goalMode: false, goalId: '' }] }
    let acknowledge = () => { throw new Error('Send not started') }
    testWindow.cefQuery = ({ request, onSuccess }) => {
      if (JSON.parse(request).action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else if (JSON.parse(request).action === 'sendAcpPrompt') acknowledge = () => onSuccess('{}')
      else if (JSON.parse(request).action !== 'getChatMessages') onSuccess('{}')
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const prefix = cefStore.getState().messages['chat-1'].slice(0, 2)
    const sending = order !== 'none' ? cefStore.getState().sendAcpPrompt('chat-1', 'Later pending input') : undefined
    if (order !== 'none') {
      // Legacy-only raw tail prerequisite, independent of fresh generic admission and its callback order.
      expect(cefStore.getState().messages['chat-1']).toHaveLength(3)
      cefStore.setState((state) => ({ messages: { ...state.messages,
        'chat-1': [...state.messages['chat-1'], legacySyntheticPrompt('Later pending input')],
      } }))
    }
    if (order === 'ack-before-removal') { acknowledge(); await sending }
    const optimistic = cefStore.getState().messages['chat-1'].find((message) => message.content === 'Later pending input')
    const observed: { rows: string[]; known: number[] }[] = []
    const unsubscribe = cefStore.subscribe((state) => observed.push({ rows: state.messages['chat-1']?.map((message) => message.content) ?? [], known: state.acpBindingBySessionId['chat-1'].unsentMessageIndices ?? [] }))
    try {
      testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, chats: [{ ...initial.chats[0], messages: undefined,
        messageCount: 2, messagesDigest: 'two', acpSession: { ...initial.chats[0].acpSession, unsentMessageIndices: [], queuedPrompts: [] } }] } })
      expect(cefStore.getState().messages['chat-1']).toEqual(optimistic ? [...prefix, optimistic] : prefix)
      expect(cefStore.getState().messages['chat-1'][0]).toBe(prefix[0])
      expect(observed.every(({ rows, known }) => !rows.includes('Referenced unsent') || known.includes(2))).toBe(true)
      expect(cefStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts).toEqual([])
      if (order === 'removal-before-ack') { acknowledge(); await expect(sending).resolves.toBe(true) }
      if (order !== 'none') {
        expect(cefStore.getState().messages['chat-1'].map((message) => message.content)).toEqual(['Delivered', 'Reply', 'Later pending input'])
        if (optimistic) expect(cefStore.getState().messages['chat-1'][2]).toBe(optimistic)
      }
    } finally { unsubscribe() }
  })

  it('retires only a matched native failed suffix atomically before a deferred history response', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    const initial = makeCppState(1)
    initial.chats[0].messageCount = 3
    initial.chats[0].messagesDigest = 'old-three'
    initial.chats[0].messages = [
      { role: 'user', content: 'Delivered', createdAt: '2026-01-01T00:00:00Z' },
      { role: 'assistant', content: 'Reply', createdAt: '2026-01-01T00:00:01Z' },
      { role: 'user', content: 'Failed', acpPromptNotSent: true, createdAt: '2026-01-01T00:00:02Z' },
    ]
    initial.chats[0].acpSession = { sessionId: '', running: false, processing: false, unsentMessageIndices: [2] }
    testWindow.cefQuery = ({ request, onSuccess }) => {
      if (JSON.parse(request).action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else if (JSON.parse(request).action !== 'getChatMessages') onSuccess('{}')
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const prefix = cefStore.getState().messages['chat-1'].slice(0, 2)
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, chats: [{ ...initial.chats[0], messages: undefined,
      messageCount: 2, messagesDigest: 'new-two', acpSession: { ...initial.chats[0].acpSession, unsentMessageIndices: [], queueHeld: true,
        queuedPrompts: [{ id: 'held-failed', revision: 1, text: 'Failed' }] } }] } })
    expect(cefStore.getState().messages['chat-1']).toEqual(prefix)
    expect(cefStore.getState().messages['chat-1'][0]).toBe(prefix[0])
    expect(cefStore.getState().messages['chat-1'][1].nativeMessageIndex).toBe(1)
    expect(cefStore.getState().acpBindingBySessionId['chat-1'].unsentMessageIndices).toEqual([])
  })

  it('preserves a newer legacy optimistic attempt when an older held snapshot and history response arrive', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    const initial = makeCppState(1)
    initial.chats[0].messageCount = 0
    initial.chats[0].messagesDigest = 'empty'
    initial.chats[0].acpSession = { sessionId: '', running: false, processing: false }
    const acknowledgements: Array<() => void> = []
    let hydrate = () => { throw new Error('Hydration not started') }
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const action = JSON.parse(request).action
      if (action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else if (action === 'sendAcpPrompt') acknowledgements.push(() => onSuccess('{}'))
      else if (action === 'getChatMessages') hydrate = () => onSuccess(JSON.stringify({ chatId: 'chat-1', messages: [], startIndex: 0, totalCount: 0, messagesDigest: 'empty', unsentMessageIndices: [], recoverableUnsentMessageIndices: [] }))
      else onSuccess('{}')
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const first = cefStore.getState().sendAcpPrompt('chat-1', 'Old failed input')
    acknowledgements[0]()
    await first
    expect(cefStore.getState().messages['chat-1']).toEqual([])
    // Force history only for an explicitly restored legacy opaque row, not for generic admission.
    cefStore.setState({ messages: { 'chat-1': [legacySyntheticPrompt('Old failed input')] } })
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, chats: [{ ...initial.chats[0], messages: undefined,
      acpSession: { ...initial.chats[0].acpSession, queueHeld: true, queuedPrompts: [{ id: 'held-old', revision: 1, text: 'Old failed input' }] } }] } })
    const second = cefStore.getState().sendAcpPrompt('chat-1', 'Newer input')
    expect(cefStore.getState().messages['chat-1'].map((message) => message.content)).toEqual(['Old failed input'])
    // A later legacy projection changes row identity while the old authoritative response is outstanding.
    const laterLegacy = legacySyntheticPrompt('Newer input')
    cefStore.setState((state) => ({ messages: { ...state.messages, 'chat-1': [...state.messages['chat-1'], laterLegacy] } }))
    acknowledgements[1]()
    await expect(second).resolves.toBe(true)
    hydrate()
    await new Promise((resolve) => setTimeout(resolve, 0))
    expect(cefStore.getState().messages['chat-1'].map((message) => message.content)).toContain('Newer input')
    expect(cefStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts?.[0].text).toBe('Old failed input')
  })

  it('ignores an older prompt failure after a newer prompt succeeds', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    const initial = makeCppState(1)
    initial.chats[0].messages = []
    initial.chats[0].messageCount = 0
    initial.chats[0].acpSession = { sessionId: 'native', running: true, processing: false, lastError: '' }
    let rejectFirst = () => { throw new Error('First request not started') }
    let promptCount = 0
    testWindow.cefQuery = ({ request, onSuccess, onFailure }) => {
      if (JSON.parse(request).action !== 'sendAcpPrompt') { onSuccess(JSON.stringify(initial)); return }
      if (++promptCount === 1) rejectFirst = () => onFailure(500, 'Old admission failure')
      else onSuccess('{}')
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const first = cefStore.getState().sendAcpPrompt('chat-1', 'First attempt')
    await expect(cefStore.getState().sendAcpPrompt('chat-1', 'Accepted attempt')).resolves.toBe(true)
    rejectFirst()
    await expect(first).resolves.toBe(false)
    expect(cefStore.getState().acpBindingBySessionId['chat-1'].promptActionError).toBeUndefined()
    // A newer successful admission suppresses the stale error, but neither callback owns a native row.
    expect(cefStore.getState().messages['chat-1']).toEqual([])
  })

  it('preserves failed prompt actions across native snapshots and gives retries a fresh identity', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    const initial = makeCppState(1)
    initial.chats[0].acpSession = { sessionId: 'native', running: true, processing: false, lastError: '' }
    testWindow.cefQuery = ({ request, onSuccess, onFailure }) => {
      if (JSON.parse(request).action === 'sendAcpPrompt') onFailure(500, 'Could not load chat history.')
      else onSuccess(JSON.stringify(initial))
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    await expect(cefStore.getState().sendAcpPrompt('chat-1', 'Retry me')).resolves.toBe(false)
    const first = cefStore.getState().acpBindingBySessionId['chat-1'].promptActionError
    expect(first?.message).toBe('Could not load chat history.')
    testWindow.uamPush?.({ type: 'stateUpdate', data: { ...initial, stateRevision: 2 } })
    expect(cefStore.getState().acpBindingBySessionId['chat-1'].promptActionError).toEqual(first)
    expect(cefStore.getState().acpBindingBySessionId['chat-1'].processing).toBe(false)
    await expect(cefStore.getState().sendAcpPrompt('chat-1', 'Retry me')).resolves.toBe(false)
    expect(cefStore.getState().acpBindingBySessionId['chat-1'].promptActionError?.id).not.toBe(first?.id)
    testWindow.cefQuery = ({ onSuccess }) => onSuccess('{}')
    await expect(cefStore.getState().sendAcpPrompt('chat-1', 'Retry me')).resolves.toBe(true)
    expect(cefStore.getState().acpBindingBySessionId['chat-1'].promptActionError).toBeUndefined()
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

  it('preserves the active runtime when a delayed queued prompt is rejected', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats[0].acpSession = {
      sessionId: 'acp-chat-1', running: true, processing: true, lifecycleState: 'processing',
      turnSerial: 1, turnUserMessageIndex: -1, turnAssistantMessageIndex: -1,
    }
    let rejectPrompt = () => { throw new Error('prompt was not sent') }
    testWindow.cefQuery = ({ request, onSuccess, onFailure }) => {
      if (JSON.parse(request).action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else rejectPrompt = () => onFailure(409, 'Queue is full')
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const consoleSpy = vi.spyOn(console, 'error').mockImplementation(() => {})
    for (const advanceTurn of [false, true]) {
      const sending = cefStore.getState().sendAcpPrompt('chat-1', 'Follow up', [], advanceTurn)
      if (advanceTurn) {
        cefStore.setState((state) => ({
          acpBindingBySessionId: {
            ...state.acpBindingBySessionId,
            'chat-1': { ...state.acpBindingBySessionId['chat-1'], turnSerial: 2, processingStartedAtMs: 1234 },
          },
        }))
      }
      const before = cefStore.getState().acpBindingBySessionId['chat-1']
      rejectPrompt()
      await expect(sending).resolves.toBe(false)
      expect(cefStore.getState().acpBindingBySessionId['chat-1']).toEqual({
        ...before, promptActionError: { id: expect.any(String), message: 'Queue is full' },
      })
    }
    consoleSpy.mockRestore()
  })

  it('sends an image-only prompt through the bridge and rejects a completely empty send', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const requests: Array<{ action: string; payload: Record<string, unknown> }> = []
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      if (parsed.action === 'getInitialState') onSuccess(JSON.stringify(makeCppState(1)))
      else { requests.push(parsed); onSuccess('{}') }
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const image = { id: 'image-1', name: 'image.png', type: 'image', size: 10, path: '/tmp/image.png' }
    await expect(cefStore.getState().sendAcpPrompt('chat-1', '', [image])).resolves.toBe(true)
    expect(requests).toContainEqual(expect.objectContaining({ action: 'sendAcpPrompt', payload: expect.objectContaining({ text: '', attachments: [image] }) }))
    const count = requests.length
    await expect(cefStore.getState().sendAcpPrompt('chat-1', '  ')).resolves.toBe(false)
    expect(requests).toHaveLength(count)
  })

  it('matches a late send response to its user position rather than the last message', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats[0].messages = [{ role: 'user', content: 'Repeat', createdAt: '2026-01-01T00:00:00.000Z' }]
    initial.chats[0].messageCount = 1
    initial.chats[0].acpSession = {
      sessionId: 'acp-chat-1', running: true, processing: false, lifecycleState: 'ready',
      turnSerial: 1, turnUserMessageIndex: 0, turnAssistantMessageIndex: -1,
    }
    let finishPrompt = () => { throw new Error('prompt was not sent') }
    testWindow.cefQuery = ({ request, onSuccess }) => {
      if (JSON.parse(request).action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else finishPrompt = () => onSuccess('{}')
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const first = cefStore.getState().sendAcpPrompt('chat-1', 'Repeat')
    finishPrompt()
    await expect(first).resolves.toBe(true)
    // Ordinary ACK leaves the one pre-existing native position unchanged until an authoritative push.
    expect(cefStore.getState().messages['chat-1']).toHaveLength(1)
    expect(cefStore.getState().messages['chat-1'][0]).toMatchObject({ content: 'Repeat', nativeMessageIndex: 0 })

    const attachment = { id: 'file-1', name: 'test.txt', type: 'text/plain', size: 10, path: '/tmp/test.txt' }
    const second = cefStore.getState().sendAcpPrompt('chat-1', 'Repeat', [attachment])
    testWindow.uamPush?.({ type: 'stateUpdate', data: {
      ...initial, stateRevision: 2,
      chats: [{ ...initial.chats[0], messageCount: 4, messages: [
        ...initial.chats[0].messages!,
        { role: 'user', content: 'Repeat', createdAt: '2026-01-01T00:00:01.000Z' },
        { role: 'user', content: 'Repeat', attachments: [attachment], createdAt: '2026-01-01T00:00:02.000Z' },
        { role: 'assistant', content: 'Answer', createdAt: '2026-01-01T00:00:03.000Z' },
      ], acpSession: { ...initial.chats[0].acpSession, processing: true, lifecycleState: 'processing',
        turnSerial: 3, turnUserMessageIndex: 2, turnAssistantMessageIndex: 3 } }],
    } })
    finishPrompt()
    await expect(second).resolves.toBe(true)
    expect(cefStore.getState().messages['chat-1']).toHaveLength(4)
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: ' continued' })
    await new Promise((resolve) => setTimeout(resolve, 80))
    expect(cefStore.getState().messages['chat-1'].map(({ content }) => content)).toEqual([
      'Repeat', 'Repeat', 'Repeat', 'Answer continued',
    ])
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
    initial.chats[0].messagesDigest = 'turn-one'
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
          // Patch chat entries are summaries; the full native transcript uses this zero-based carrier.
          messagesByChatId: { 'chat-1': [...initial.chats[0].messages!, {
            role: 'user', content: 'New question', createdAt: '2026-01-01T00:00:02.000Z',
          }] },
          chats: [{
            ...initial.chats[0],
            messages: undefined,
            messageCount: 3,
            messagesDigest: 'turn-two',
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
    const oldAssistant = cefStore.getState().messages['chat-1'][1]
    await expect(cefStore.getState().sendAcpPrompt('chat-1', 'New question')).resolves.toBe(true)
    expect(cefStore.getState().messages['chat-1'][2]).toMatchObject({ content: 'New question', nativeMessageIndex: 2 })
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: 'Second' })
    await new Promise((resolve) => setTimeout(resolve, 80))

    expect(cefStore.getState().messages['chat-1'][1]).toBe(oldAssistant)
    expect(oldAssistant.content).toBe('First')
    expect(cefStore.getState().messages['chat-1'].map((message) => message.content)).toEqual([
      'Old question',
      'First',
      'New question',
      'Second',
    ])
  })

  it.each(['continuous', 'native-steer'] as const)('commits a %s boundary before streaming into a new bubble', async (boundary) => {
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
          ...(boundary === 'native-steer' ? [
            { role: 'user', content: 'Steering', continuesTurn: true, createdAt: '2026-01-01T00:00:02.000Z' },
            { role: 'assistant', content: '', createdAt: '2026-01-01T00:00:03.000Z' },
          ] : []),
        ],
      }))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: ' tail' })
    await new Promise((resolve) => setTimeout(resolve, 80))
    if (boundary === 'continuous') testWindow.uamPush?.({ type: 'streamDone', chatId: 'chat-1' })
    testWindow.uamPush?.({
      type: 'statePatch',
      data: {
        stateRevision: 2,
        chats: [{
          ...initial.chats[0],
          messages: undefined,
          messagesDigest: 'turn-2',
          messageCount: boundary === 'native-steer' ? 4 : 2,
          acpSession: {
            ...initial.chats[0].acpSession,
            turnSerial: boundary === 'native-steer' ? 1 : 2,
            turnUserMessageIndex: boundary === 'native-steer' ? 0 : -1,
            turnAssistantMessageIndex: boundary === 'native-steer' ? 3 : -1,
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
      ...(boundary === 'native-steer' ? ['Steering'] : []),
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

  it('validates optional native stream message positions', () => {
    const token = { type: 'streamToken', chatId: 'chat-1', token: 'text' }
    expect(parseUamPushPayload(token).ok).toBe(true)
    expect(parseUamPushPayload({ ...token, messageIndex: 0 })).toMatchObject({ ok: true, message: { messageIndex: 0 } })
    for (const messageIndex of [-1, 1.5, NaN, Infinity, Number.MAX_SAFE_INTEGER + 1, '3', null])
      expect(parseUamPushPayload({ ...token, messageIndex }).ok).toBe(false)
  })

  it('keeps streamed segment keys unique within the same millisecond', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    const initial = makeCppState(1)
    initial.chats[0].messages = []
    initial.chats[0].acpSession = { sessionId: 'native', running: true, processing: true }
    testWindow.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(initial))
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise(resolve => setTimeout(resolve, 0))
    const clock = vi.spyOn(Date, 'now').mockReturnValue(123456789)
    try {
      for (const messageIndex of [1, 3, 5]) {
        testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: `Reply ${messageIndex}`, messageIndex })
      }
      await new Promise(resolve => setTimeout(resolve, 80))
      const messages = cefStore.getState().messages['chat-1']
      expect(messages.map(message => message.content)).toEqual(['Reply 1', 'Reply 3', 'Reply 5'])
      expect(new Set(messages.map(message => message.id)).size).toBe(3)
    } finally {
      clock.mockRestore()
    }
  })

  it('routes identified tokens to a new segment before its summary arrives', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    const initial = makeCppState(1)
    const original = [
      { role: 'user', content: 'Original', createdAt: '2026-01-01T00:00:00Z' },
      { role: 'assistant', content: 'Earlier reply', createdAt: '2026-01-01T00:00:01Z' },
    ]
    initial.chats[0].messages = original
    initial.chats[0].messageCount = 2
    initial.chats[0].acpSession = { sessionId: 'native', running: true, processing: true, turnSerial: 1, turnUserMessageIndex: 0, turnAssistantMessageIndex: 1 }
    let reply: (() => void) | undefined
    testWindow.cefQuery = ({ request, onSuccess }) => {
      if (JSON.parse(request).action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else reply = () => onSuccess(JSON.stringify({ chatId: 'chat-1', messages: [original[0], { ...original[1], content: 'Earlier reply previous' },
        { role: 'user', content: 'Steer', continuesTurn: true, createdAt: '2026-01-01T00:00:02Z' },
        { role: 'assistant', content: 'New reply', createdAt: '2026-01-01T00:00:03Z' },
      ] }))
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise(resolve => setTimeout(resolve, 0))
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: ' previous', messageIndex: 1 })
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: 'New reply', messageIndex: 3 })
    await new Promise(resolve => setTimeout(resolve, 80))
    expect(cefStore.getState().messages['chat-1'][1].content).toBe('Earlier reply previous')
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, chats: [{ ...initial.chats[0], messages: undefined, messageCount: 4,
      acpSession: { ...initial.chats[0].acpSession, turnAssistantMessageIndex: 3 },
    }] } })
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: ' continued', messageIndex: 3 })
    await new Promise(resolve => setTimeout(resolve, 80))
    expect(reply).toBeTypeOf('function')
    reply!()
    await new Promise(resolve => setTimeout(resolve, 0))
    expect(cefStore.getState().messages['chat-1'].map(message => message.content)).toEqual(['Original', 'Earlier reply previous', 'Steer', 'New reply continued'])
    expect(cefStore.getState().messages['chat-1'][2].continuesTurn).toBe(true)
  })

  it.each([[false, false], [false, true], [true, false], [true, true]])('hydrates a steered turn despite streamed tail changes (done: %s, prior streaming: %s)', async (doneRace, priorStreaming) => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    const initial = makeCppState(1)
    const oldMessages = [
      { role: 'user', content: 'Original prompt', createdAt: '2026-01-01T00:00:00.000Z' },
      { role: 'assistant', content: 'Old response', createdAt: '2026-01-01T00:00:01.000Z' },
    ]
    initial.chats[0].messages = oldMessages
    initial.chats[0].acpSession = { sessionId: 'native', running: true, processing: true, turnSerial: 2,
      turnUserMessageIndex: 2, turnAssistantMessageIndex: 3, turnEvents: [{ type: 'assistant_text', text: 'Newer response' }] }
    const nativeMessages = [oldMessages[0], { ...oldMessages[1], interrupted: true,
      blocks: [{ type: 'assistant_text', text: 'Old response' }, { type: 'tool_call', toolCallId: 'old-tool' }] },
      { role: 'user', content: 'Steered prompt', prioritySteer: true, createdAt: '2026-01-01T00:00:02.000Z' },
      { role: 'assistant', content: 'New', createdAt: '2026-01-01T00:00:03.000Z' },
    ]
    const replies: Array<() => void> = []
    testWindow.cefQuery = ({ request, onSuccess }) => {
      if (JSON.parse(request).action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else {
        const snapshot = replies.length === 0 ? nativeMessages : [...nativeMessages.slice(0, -1), { ...nativeMessages[3], content: 'Newer response' }]
        replies.push(() => onSuccess(JSON.stringify({ chatId: 'chat-1', messages: snapshot, messagesDigest: 'native-digest' })))
      }
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    if (priorStreaming) cefStore.setState((state) => ({ messages: { ...state.messages, 'chat-1': state.messages['chat-1'].map((message, index) => index === 1 ? { ...message, isStreaming: true } : message) } }))
    cefStore.getState().loadSessionMessages('chat-1', true)
    testWindow.uamPush?.({ type: 'streamToken', chatId: 'chat-1', token: 'Newer response' })
    await new Promise((resolve) => setTimeout(resolve, 80))
    if (doneRace) testWindow.uamPush?.({ type: 'streamDone', chatId: 'chat-1' })
    for (const reply of replies) reply()
    await new Promise((resolve) => setTimeout(resolve, 0))
    const messages = cefStore.getState().messages['chat-1']
    expect(messages.map((message) => message.role)).toEqual(['user', 'assistant', 'user', 'assistant'])
    expect(messages[1].interrupted).toBe(true)
    expect(messages[1].blocks).toHaveLength(2)
    expect(messages[2].content).toBe('Steered prompt')
    expect(messages[3].content).toBe('Newer response')
    expect(Boolean(messages[3].isStreaming)).toBe(!doneRace)
  })

  it.each(['continuous', 'native-steer'] as const)('hydrates %s user input before any assistant output', async (boundary) => {
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
          { role: 'user', content: 'Queued question', continuesTurn: boundary === 'native-steer', createdAt: '2026-01-01T00:00:01.000Z' },
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
            turnSerial: boundary === 'native-steer' ? 1 : 2,
            turnUserMessageIndex: boundary === 'native-steer' ? 0 : 1,
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


  describe('bounded exact-refusal cold history reread', () => {
    const refusal = 'Chat history changed while loading. Retry to refresh it.'
    async function coldHistoryFixture(loaded = false) {
      const testWindow = ensureTestWindow()
      vi.resetModules()
      testWindow.dispatchEvent = vi.fn(() => true)
      const initial = makeCppState(1)
      initial.chats[0].providerId = 'codex-cli'
      initial.chats[0].cliTerminal = undefined
      initial.chats[0].messages = loaded ? [{ role: 'assistant', content: 'Already loaded', createdAt: '2026-01-01T00:00:00.000Z' }] : undefined
      initial.chats[0].messageCount = 1
      initial.chats[0].messagesDigest = 'cold-summary'
      initial.chats[0].acpSession = {
        providerId: 'codex-cli', sessionId: 'native-cold', threadId: 'thread-cold',
        running: true, processing: false, lifecycleState: 'ready', turnSerial: 4,
        idleCountdownStartsAtMs: 1000, idleShutdownAtMs: 31000,
        unsentMessageIndices: [], recoverableUnsentMessageIndices: [], diagnostics: [],
      }
      const requests: Array<{ action: string; requestId?: string; payload?: Record<string, unknown> }> = []
      const replies: Array<{ success: (value: string) => void; failure: (code: number, error: string) => void }> = []
      let finishSend: ((value: string) => void) | undefined
      testWindow.cefQuery = ({ request, onSuccess, onFailure }) => {
        const parsed = JSON.parse(request) as typeof requests[number]
        requests.push(parsed)
        if (parsed.action === 'getInitialState') onSuccess(JSON.stringify(initial))
        else if (parsed.action === 'getChatMessages') replies.push({ success: onSuccess, failure: onFailure })
        else if (parsed.action === 'sendAcpPrompt') finishSend = onSuccess
        else if (parsed.action === 'releaseChatMessages') onSuccess('{}')
      }
      const { useAppStore: store } = await import('./useAppStore')
      await new Promise(resolve => setTimeout(resolve, 0))
      const historyRequests = () => requests.filter(request => request.action === 'getChatMessages')
      const page = { chatId: 'chat-1', startIndex: 0, totalCount: 1, messagesDigest: 'fresh-native',
        messages: [{ role: 'user', content: 'Retained rejected input', createdAt: '2026-01-01T00:00:00.000Z', interrupted: true, acpPromptNotSent: true }],
        unsentMessageIndices: [0], recoverableUnsentMessageIndices: [0] }
      const tick = () => new Promise(resolve => setTimeout(resolve, 0))
      const metadataPush = () => testWindow.uamPush!({ type: 'statePatch', data: { stateRevision: 2,
        chats: [{ ...initial.chats[0], messages: undefined, acpSession: { ...initial.chats[0].acpSession, idleShutdownAtMs: 32000 } }],
      } })
      return { store, requests, replies, historyRequests, page, tick, metadataPush, finishSend: () => finishSend?.('{}') }
    }

    it('rereads one exact cold refusal with a new ID and atomically adopts only the fresh page', async () => {
      const f = await coldHistoryFixture()
      const loading = f.store.getState().loadSessionMessages('chat-1')
      expect(f.store.getState().messages['chat-1']).toBeUndefined()
      f.replies[0].failure(409, refusal)
      await f.tick()
      expect(f.historyRequests()).toHaveLength(2)
      expect(f.historyRequests()[1].requestId).not.toBe(f.historyRequests()[0].requestId)
      expect(f.historyRequests()[1].payload).toEqual({ chatId: 'chat-1', messagesDigest: '', limit: 50, deferToolCallContent: true })
      expect(f.store.getState().messages['chat-1']).toBeUndefined()
      expect(f.store.getState().chatHistoryErrorBySessionId['chat-1']).toBeUndefined()
      expect(f.store.getState().acpBindingBySessionId['chat-1'].recoverableUnsentMessageIndices).toEqual([])
      const observed: Array<{ count: number; recoverable: number[] | undefined }> = []
      const unsubscribe = f.store.subscribe(state => observed.push({ count: state.messages['chat-1']?.length ?? 0,
        recoverable: state.acpBindingBySessionId['chat-1'].recoverableUnsentMessageIndices }))
      try {
        f.replies[1].success(JSON.stringify(f.page))
        await loading
        expect(f.store.getState().messages['chat-1'].map(row => row.content)).toEqual(['Retained rejected input'])
        expect(f.store.getState().acpBindingBySessionId['chat-1'].unsentMessageIndices).toEqual([0])
        expect(f.store.getState().acpBindingBySessionId['chat-1'].recoverableUnsentMessageIndices).toEqual([0])
        expect(observed.filter(state => state.count === 1).every(state => state.recoverable?.join() === '0')).toBe(true)
        expect(f.requests.map(request => request.action)).toEqual(['getInitialState', 'getChatMessages', 'getChatMessages'])
      } finally { unsubscribe() }
    })

    it('shows the second exact refusal normally without a third read', async () => {
      const f = await coldHistoryFixture()
      const loading = f.store.getState().loadSessionMessages('chat-1')
      f.replies[0].failure(409, refusal)
      await f.tick()
      expect(f.replies).toHaveLength(2)
      f.replies[1].failure(409, refusal)
      expect(await loading).toBe(false)
      expect(f.historyRequests()).toHaveLength(2)
      expect(f.store.getState().messages['chat-1']).toBeUndefined()
      expect(f.store.getState().chatHistoryErrorBySessionId['chat-1']).toBe(refusal)
    })

    it.each(['refusal-first', 'presentation-first'] as const)('shares the existing one-reread budget (%s)', async order => {
      const f = await coldHistoryFixture()
      const loading = f.store.getState().loadSessionMessages('chat-1')
      if (order === 'refusal-first') {
        f.replies[0].failure(409, refusal)
        await f.tick()
        expect(f.replies).toHaveLength(2)
        f.metadataPush()
        f.replies[1].success(JSON.stringify(f.page))
      } else {
        f.metadataPush()
        f.replies[0].success(JSON.stringify(f.page))
        await f.tick()
        expect(f.replies).toHaveLength(2)
        f.replies[1].failure(409, refusal)
      }
      await loading
      expect(f.historyRequests()).toHaveLength(2)
      expect(f.historyRequests().every(request => !('refreshNative' in request.payload!))).toBe(true)
      expect(f.store.getState().messages['chat-1']).toBeUndefined()
      expect(f.store.getState().acpBindingBySessionId['chat-1'].recoverableUnsentMessageIndices).toEqual([])
      expect(f.store.getState().chatHistoryErrorBySessionId['chat-1']).toBe(refusal)
    })

    it.each(['different-error', 'already-loaded', 'native-refresh'] as const)('does not automatically reread %s', async mode => {
      const f = await coldHistoryFixture(mode === 'already-loaded')
      const before = f.store.getState().messages['chat-1']
      const loading = f.store.getState().loadSessionMessages('chat-1', mode === 'native-refresh', mode === 'native-refresh')
      const error = mode === 'different-error' ? 'Chat history changed while loading.' : refusal
      f.replies[0].failure(409, error)
      expect(await loading).toBe(false)
      expect(f.historyRequests()).toHaveLength(1)
      expect(f.store.getState().messages['chat-1']).toBe(before)
      expect(f.store.getState().chatHistoryErrorBySessionId['chat-1']).toBe(error)
    })

    it.each(['superseded', 'deleted', 'new-birth', 'authored-message', 'unloaded', 'owner', 'binding', 'search', 'send-in-flight'] as const)(
      'suppresses exact-refusal reread after %s', async change => {
        const f = await coldHistoryFixture()
        const loading = f.store.getState().loadSessionMessages('chat-1')
        let newer: Promise<void | false> | undefined
        let sending: Promise<boolean> | undefined
        if (change === 'superseded') newer = f.store.getState().loadSessionMessages('chat-1', true)
        if (change === 'deleted') f.store.setState({ sessions: [] })
        if (change === 'new-birth') f.store.setState(state => ({ sessions: state.sessions.map(chat => ({ ...chat, createdAt: new Date(chat.createdAt.getTime() + 1) })) }))
        if (change === 'authored-message') f.store.setState({ messages: { 'chat-1': [legacySyntheticPrompt('Newly authored input')] } })
        if (change === 'unloaded') f.store.getState().unloadSessionMessages('chat-1')
        if (change === 'owner') f.store.setState(state => ({ sessions: state.sessions.map(chat => ({ ...chat, providerId: 'opencode-cli', executionHostId: 'new-host', workspaceDirectory: '/tmp/other' })) }))
        if (change === 'binding') f.store.setState(state => ({ acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], turnSerial: 5 } } }))
        if (change === 'search') f.store.setState({ chatSearchNavigation: { generation: 1, queryRequestId: 'test-search', chatId: 'chat-1', messageIndex: 0, messagesDigest: 'cold-summary', phase: 'loading', anchored: false } })
        if (change === 'send-in-flight') sending = f.store.getState().sendAcpPrompt('chat-1', 'New pending input')
        const rowsBefore = f.store.getState().messages['chat-1']
        f.replies[0].failure(409, refusal)
        await loading
        await f.tick()
        expect(f.historyRequests()).toHaveLength(change === 'superseded' ? 2 : 1)
        expect(f.store.getState().messages['chat-1']).toBe(rowsBefore)
        if (change === 'deleted' || change === 'new-birth' || change === 'unloaded' || change === 'superseded') {
          expect(f.store.getState().chatHistoryErrorBySessionId['chat-1']).toBeUndefined()
        }
        if (change === 'superseded') { f.replies[1].failure(502, 'Newer request stopped'); await newer }
        if (sending) { f.finishSend(); await sending }
      })
  })

  it.each(['identical', 'idle-clock', 'diagnostic', 'newer-turn', 'repeated-metadata'] as const)('keeps cold history authority scoped across a real summary push (%s)', async (change) => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats[0].providerId = 'codex-cli'
    initial.chats[0].cliTerminal = undefined
    initial.chats[0].messages = undefined
    initial.chats[0].messageCount = 2
    initial.chats[0].messagesDigest = 'cold-summary'
    initial.chats[0].acpSession = {
      providerId: 'codex-cli', protocolKind: 'codex-app-server',
      sessionId: 'native-cold-chat-1', threadId: 'thread-cold-chat-1',
      running: true, processing: false, lifecycleState: 'ready', turnSerial: 4,
      idleCountdownStartsAtMs: 1000, idleShutdownAtMs: 31000, idleShutdownTimeoutSeconds: 30,
      unsentMessageIndices: [], recoverableUnsentMessageIndices: [], diagnostics: [],
    }
    const requests: Array<{ action: string; payload?: Record<string, unknown>; requestId?: string }> = []
    const replies: Array<(response: string) => void> = []
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request) as { action: string; payload?: Record<string, unknown>; requestId?: string }
      requests.push(parsed)
      if (parsed.action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else if (parsed.action === 'getChatMessages') replies.push(onSuccess)
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    expect(cefStore.getState().messages['chat-1']).toBeUndefined()
    const loading = cefStore.getState().loadSessionMessages('chat-1')
    expect(requests.filter(request => request.action === 'getChatMessages')).toHaveLength(1)
    expect(requests.at(-1)?.payload?.messagesDigest).toBe('')
    const originalBinding = cefStore.getState().acpBindingBySessionId['chat-1']
    const acp = { ...initial.chats[0].acpSession }
    if (change === 'idle-clock' || change === 'repeated-metadata') acp.idleShutdownAtMs = 32000
    if (change === 'diagnostic') acp.diagnostics = [{
      time: '2026-01-01T00:00:02.000Z', event: 'diagnostic', reason: 'presentation-only',
      method: '', requestId: '', code: null, message: 'Recorded diagnostic.', detail: '', lifecycleState: 'ready',
    }]
    if (change === 'newer-turn') acp.turnSerial = 5
    // This is a real sparse statePatch through sanitizeCppStatePatch/applyStatePatch,
    // not an injected replacement AcpBinding. Native identity, history summary,
    // queue contents and both unsent arrays remain unchanged in this push.
    testWindow.uamPush!({ type: 'statePatch', data: { stateRevision: 2,
      chats: [{ ...initial.chats[0], messages: undefined, acpSession: acp }],
    } })
    const pushedBinding = cefStore.getState().acpBindingBySessionId['chat-1']
    expect(cefStore.getState().lastAppliedStateRevision).toBe(2)
    if (change === 'identical') expect(pushedBinding).toBe(originalBinding)
    else expect(pushedBinding).not.toBe(originalBinding)
    expect(pushedBinding.sessionId).toBe(originalBinding.sessionId)
    expect(pushedBinding.threadId).toBe(originalBinding.threadId)
    expect(pushedBinding.providerId).toBe(originalBinding.providerId)
    expect(pushedBinding.queueHeld).toBe(originalBinding.queueHeld)
    expect(pushedBinding.unsentMessageIndices).toEqual([])
    expect(pushedBinding.recoverableUnsentMessageIndices).toEqual([])
    expect(requests.filter(request => request.action === 'getChatMessages')).toHaveLength(1)

    const rows = [
      { role: 'user' as const, content: 'Earlier rejected input', createdAt: '2026-01-01T00:00:00.000Z', interrupted: true, acpPromptNotSent: true },
      { role: 'user' as const, content: 'Latest rejected input', createdAt: '2026-01-01T00:00:01.000Z', interrupted: true, acpPromptNotSent: true },
    ]
    const page = { chatId: 'chat-1', startIndex: 0, totalCount: 2,
      messagesDigest: 'native-hash', messages: rows, unsentMessageIndices: [0, 1], recoverableUnsentMessageIndices: [1] }
    const observed: { count: number; known: number[] | undefined; retry: number[] | undefined; error: string | undefined }[] = []
    const unsubscribe = cefStore.subscribe(state => observed.push({ count: state.messages['chat-1']?.length ?? 0,
      known: state.acpBindingBySessionId['chat-1'].unsentMessageIndices,
      retry: state.acpBindingBySessionId['chat-1'].recoverableUnsentMessageIndices,
      error: state.chatHistoryErrorBySessionId['chat-1'] }))
    let expectedRows = rows
    try {
      replies[0](JSON.stringify(page))
      if (change === 'idle-clock' || change === 'diagnostic' || change === 'repeated-metadata') {
        // R2 design: the first page is stale and MUST NOT be adopted. The
        // original call may make one new read under the CURRENT binding.
        await new Promise((resolve) => setTimeout(resolve, 0))
        const historyRequests = requests.filter(request => request.action === 'getChatMessages')
        expect(historyRequests).toHaveLength(2)
        expect(historyRequests[1].requestId).toBeTypeOf('string')
        expect(historyRequests[1].requestId).not.toBe(historyRequests[0].requestId)
        expect(historyRequests[1].payload).toMatchObject({ chatId: 'chat-1', messagesDigest: '' })
        expect(historyRequests[1].payload).not.toHaveProperty('refreshNative')
        expect(historyRequests[0].payload).not.toHaveProperty('refreshNative')
        expect(replies).toHaveLength(2)
        expect(cefStore.getState().messages['chat-1']).toBeUndefined()
        expect(cefStore.getState().acpBindingBySessionId['chat-1'].unsentMessageIndices).toEqual([])
        expect(cefStore.getState().acpBindingBySessionId['chat-1'].recoverableUnsentMessageIndices).toEqual([])
        expect(cefStore.getState().chatHistoryErrorBySessionId['chat-1']).toBeUndefined()
        expect(observed.every(state => state.count === 0 && state.known?.length === 0 && state.retry?.length === 0 && state.error === undefined)).toBe(true)
        expectedRows = rows.map(row => ({ ...row, content: `Fresh read: ${row.content}` }))
        const freshPage = { ...page, messagesDigest: 'fresh-native-hash', messages: expectedRows }
        if (change === 'repeated-metadata') {
          const bindingAtFreshRead = cefStore.getState().acpBindingBySessionId['chat-1']
          testWindow.uamPush!({ type: 'statePatch', data: { stateRevision: 3,
            chats: [{ ...initial.chats[0], messages: undefined, acpSession: { ...acp, diagnostics: [{
              time: '2026-01-01T00:00:03.000Z', event: 'diagnostic', reason: 'second-metadata-conflict',
              method: '', requestId: '', code: null, message: 'Another recorded diagnostic.', detail: '', lifecycleState: 'ready',
            }] } }],
          } })
          expect(cefStore.getState().lastAppliedStateRevision).toBe(3)
          expect(cefStore.getState().acpBindingBySessionId['chat-1']).not.toBe(bindingAtFreshRead)
        }
        replies[1](JSON.stringify(freshPage))
        await loading
        await new Promise((resolve) => setTimeout(resolve, 0))
        if (change === 'repeated-metadata') {
          // A second conflict exhausts this call's retry budget. No third
          // request or stale page/unsent projection may escape the guard.
          expect(requests.map(request => request.action)).toEqual(['getInitialState', 'getChatMessages', 'getChatMessages'])
          expect(replies).toHaveLength(2)
          expect(cefStore.getState().messages['chat-1']).toBeUndefined()
          expect(cefStore.getState().acpBindingBySessionId['chat-1'].unsentMessageIndices).toEqual([])
          expect(cefStore.getState().acpBindingBySessionId['chat-1'].recoverableUnsentMessageIndices).toEqual([])
          expect(cefStore.getState().chatHistoryErrorBySessionId['chat-1']).toBe('Chat history changed while loading. Retry to refresh it.')
          expect(observed.every(state => state.count === 0 && state.known?.length === 0 && state.retry?.length === 0)).toBe(true)
          return
        }
      } else await loading
      if (change === 'newer-turn') {
        // A genuine authority change must still reject the old page atomically.
        expect(cefStore.getState().messages['chat-1']).toBeUndefined()
        expect(cefStore.getState().acpBindingBySessionId['chat-1'].turnSerial).toBe(5)
        expect(cefStore.getState().acpBindingBySessionId['chat-1'].unsentMessageIndices).toEqual([])
        expect(cefStore.getState().acpBindingBySessionId['chat-1'].recoverableUnsentMessageIndices).toEqual([])
        expect(cefStore.getState().chatHistoryErrorBySessionId['chat-1']).toBe('Chat history changed while loading. Retry to refresh it.')
        const fresh = cefStore.getState().loadSessionMessages('chat-1', true)
        expect(requests.filter(request => request.action === 'getChatMessages')).toHaveLength(2)
        replies[1](JSON.stringify(page))
        await fresh
      }
      // Identical pushes keep the first-page control unchanged. Metadata-only
      // conflicts require the distinguishable fresh response above; no stale
      // response is accepted by weakening turn/queue/identity validation.
      expect(cefStore.getState().chatHistoryErrorBySessionId['chat-1']).toBeUndefined()
      expect(cefStore.getState().messages['chat-1'].map(message => message.content)).toEqual(expectedRows.map(row => row.content))
      expect(cefStore.getState().acpBindingBySessionId['chat-1'].unsentMessageIndices).toEqual([0, 1])
      expect(cefStore.getState().acpBindingBySessionId['chat-1'].recoverableUnsentMessageIndices).toEqual([1])
      expect(observed.filter(state => state.count === 2).every(state => state.known?.join() === '0,1' && state.retry?.join() === '1')).toBe(true)
      if (change === 'idle-clock') expect(cefStore.getState().acpBindingBySessionId['chat-1'].idleShutdownAtMs).toBe(32000)
      if (change === 'diagnostic') expect(cefStore.getState().acpBindingBySessionId['chat-1'].diagnostics).toEqual(acp.diagnostics)
      expect(requests.map(request => request.action)).toEqual(change === 'identical'
        ? ['getInitialState', 'getChatMessages'] : ['getInitialState', 'getChatMessages', 'getChatMessages'])
    } finally { unsubscribe() }
  })

  it.each(['adopted', 'unchanged', 'newer-binding', 'rejected'] as const)('applies cold unsent authority atomically only with a current accepted history response (%s)', async (outcome) => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats[0].messages = undefined
    initial.chats[0].messageCount = 2
    initial.chats[0].messagesDigest = 'cold-summary'
    initial.chats[0].acpSession = { providerId: 'opencode-cli', sessionId: '', running: false, processing: false, lifecycleState: 'stopped', unsentMessageIndices: [], recoverableUnsentMessageIndices: [] }
    let finish!: (response: string) => void
    testWindow.cefQuery = ({ request, onSuccess }) => {
      if ((JSON.parse(request) as { action: string }).action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else finish = onSuccess
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    cefStore.getState().loadSessionMessages('chat-1')
    const rows = [{ role: 'user' as const, content: 'Earlier rejected input', createdAt: '2026-01-01T00:00:00.000Z', interrupted: true, acpPromptNotSent: true },
      { role: 'user' as const, content: 'Latest rejected input', createdAt: '2026-01-01T00:00:01.000Z', interrupted: true, acpPromptNotSent: true }]
    if (outcome === 'unchanged') {
      finish(JSON.stringify({ chatId: 'chat-1', startIndex: 0, totalCount: 2, messagesDigest: 'native-hash', messages: rows }))
      await new Promise((resolve) => setTimeout(resolve, 0))
      cefStore.getState().loadSessionMessages('chat-1')
    }
    if (outcome === 'newer-binding') cefStore.setState((state) => ({ acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], turnSerial: 2, unsentMessageIndices: [], recoverableUnsentMessageIndices: [] } } }))
    const observed: { count: number; known: number[] | undefined; retry: number[] | undefined }[] = []
    const unsubscribe = cefStore.subscribe((state) => observed.push({ count: state.messages['chat-1']?.length ?? 0, known: state.acpBindingBySessionId['chat-1'].unsentMessageIndices, retry: state.acpBindingBySessionId['chat-1'].recoverableUnsentMessageIndices }))
    try {
      finish(JSON.stringify(outcome === 'rejected' ? { ok: false, error: 'Chat history changed while loading.' } : {
        chatId: 'chat-1', startIndex: 0, totalCount: 2, messagesDigest: 'native-hash',
        ...(outcome === 'unchanged' ? { unchanged: true } : { messages: rows }),
        unsentMessageIndices: [0, 1], recoverableUnsentMessageIndices: [1],
      }))
      await new Promise((resolve) => setTimeout(resolve, 0))
      const binding = cefStore.getState().acpBindingBySessionId['chat-1']
      if (outcome === 'adopted' || outcome === 'unchanged') {
        expect(binding.unsentMessageIndices).toEqual([0, 1])
        expect(binding.recoverableUnsentMessageIndices).toEqual([1])
        expect(cefStore.getState().messages['chat-1'].map((message) => message.content)).toEqual(rows.map((row) => row.content))
        expect(observed.filter((state) => state.count === 2).every((state) => state.known?.join() === '0,1' && state.retry?.join() === '1')).toBe(true)
      } else {
        expect(binding.unsentMessageIndices).toEqual([])
        expect(binding.recoverableUnsentMessageIndices).toEqual([])
        expect(cefStore.getState().messages['chat-1']).toBeUndefined()
        expect(cefStore.getState().chatHistoryErrorBySessionId['chat-1']).toContain('Chat history changed while loading.')
        if (outcome === 'newer-binding') {
          const retrying = cefStore.getState().loadSessionMessages('chat-1', true)
          finish(JSON.stringify({ chatId: 'chat-1', startIndex: 0, totalCount: 2, messagesDigest: 'native-hash', messages: rows,
            unsentMessageIndices: [0, 1], recoverableUnsentMessageIndices: [1] }))
          await retrying
          expect(cefStore.getState().chatHistoryErrorBySessionId['chat-1']).toBeUndefined()
          expect(cefStore.getState().messages['chat-1']).toHaveLength(2)
          expect(cefStore.getState().acpBindingBySessionId['chat-1'].unsentMessageIndices).toEqual([0, 1])
        }
      }
    } finally { unsubscribe() }
  })

  it.each(['older', 'search'] as const)('applies native unsent authority with an accepted %s page using absolute indices', async (operation) => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats[0].messages = undefined
    initial.chats[0].messageCount = 4
    initial.chats[0].messagesDigest = 'cold-summary'
    initial.chats[0].acpSession = { providerId: 'opencode-cli', sessionId: '', running: false, processing: false, lifecycleState: 'stopped', unsentMessageIndices: [], recoverableUnsentMessageIndices: [] }
    const rows = [
      { role: 'user' as const, content: 'Earlier unsent input', acpPromptNotSent: true, interrupted: true, createdAt: '2026-01-01T00:00:00.000Z' },
      { role: 'assistant' as const, content: '', createdAt: '2026-01-01T00:00:01.000Z' },
      { role: 'user' as const, content: 'Delivered input', createdAt: '2026-01-01T00:00:02.000Z' },
      { role: 'assistant' as const, content: 'Delivered response', createdAt: '2026-01-01T00:00:03.000Z' },
    ]
    let requests = 0
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const action = (JSON.parse(request) as { action: string }).action
      if (action === 'getInitialState') { onSuccess(JSON.stringify(initial)); return }
      if (action !== 'getChatMessages') { onSuccess('{}'); return }
      requests += 1
      onSuccess(JSON.stringify(requests === 1
        ? { chatId: 'chat-1', startIndex: 2, totalCount: 4, messagesDigest: 'full-hash', messages: rows.slice(2) }
        : { chatId: 'chat-1', startIndex: 0, totalCount: 4, messagesDigest: 'full-hash', messages: operation === 'older' ? rows.slice(0, 2) : rows,
            unsentMessageIndices: [0], recoverableUnsentMessageIndices: [] }))
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    await cefStore.getState().loadSessionMessages('chat-1')
    if (operation === 'older') await expect(cefStore.getState().loadOlderSessionMessages('chat-1')).resolves.toBe(true)
    else await expect(cefStore.getState().jumpToChatSearchHit({ chatId: 'chat-1', messageIndex: 0, messagesDigest: 'full-hash', field: 'content', excerpt: 'Earlier unsent input' }, 'unsent-search')).resolves.toBe(true)
    expect(cefStore.getState().acpBindingBySessionId['chat-1'].unsentMessageIndices).toEqual([0])
    expect(cefStore.getState().acpBindingBySessionId['chat-1'].recoverableUnsentMessageIndices).toEqual([])
    expect(cefStore.getState().messages['chat-1'].map((message) => message.nativeMessageIndex)).toEqual([0, 1, 2, 3])
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

  it.each(['appended turn', 'same-count edit'])('refreshes selected idle history after a summary-only %s', async (change) => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats[0].cliTerminal = undefined
    initial.chats[0].messages = [
      { role: 'user', content: 'External question', createdAt: '2026-01-01T00:00:00Z' },
      { role: 'assistant', content: 'External answer', createdAt: '2026-01-01T00:00:01Z' },
    ]
    initial.chats[0].messageCount = 2
    initial.chats[0].messagesDigest = 'before'
    const refreshed = change === 'same-count edit'
      ? [initial.chats[0].messages[0], { ...initial.chats[0].messages[1], content: 'Corrected answer' }]
      : [...initial.chats[0].messages,
          { role: 'user', content: 'Next external question', createdAt: '2026-01-01T00:00:02Z' },
          { role: 'assistant', content: 'Next external answer', createdAt: '2026-01-01T00:00:03Z' },
        ]
    const requests: string[] = []
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const action = JSON.parse(request).action
      requests.push(action)
      if (action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else if (action === 'getChatMessages') onSuccess(JSON.stringify({
        chatId: 'chat-1', messagesDigest: 'after', messages: refreshed,
      }))
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise(resolve => setTimeout(resolve, 0))
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, chats: [{
      ...initial.chats[0], messages: undefined, messagesDigest: 'after', messageCount: refreshed.length,
    }] } })
    await new Promise(resolve => setTimeout(resolve, 0))
    expect(requests.filter(action => action === 'getChatMessages')).toHaveLength(1)
    expect(cefStore.getState().messages['chat-1'].map(message => message.content)).toEqual(refreshed.map(message => message.content))
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 3, statusLine: 'Scan finished.' } })
    await new Promise(resolve => setTimeout(resolve, 0))
    expect(requests.filter(action => action === 'getChatMessages')).toHaveLength(1)
  })

  it('keeps cold summary change tokens separate from page hashes across repeated pushes and older pages', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats[0].cliTerminal = undefined
    initial.chats[0].messages = undefined
    initial.chats[0].messageCount = 1000
    initial.chats[0].messagesDigest = 'saved-time:1000'
    initial.chats[0].acpSession = { sessionId: 'stopped-runtime', running: false, processing: false, lifecycleState: 'stopped' }
    let contentHash = 'content-hash'
    const requests: Array<{ action: string; payload?: { before?: number; messagesDigest?: string } }> = []
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (parsed.action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else if (parsed.action === 'getChatMessages') {
        const end = parsed.payload.before ?? 1000
        const start = Math.max(0, end - parsed.payload.limit)
        onSuccess(JSON.stringify({ chatId: 'chat-1', messagesDigest: contentHash, startIndex: start, totalCount: 1000,
          messages: Array.from({ length: end - start }, (_, index) => ({ role: 'assistant', content: `${contentHash}:${start + index}`, createdAt: String(start + index) })),
        }))
      }
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise(resolve => setTimeout(resolve, 0))
    const pushSummary = async (stateRevision: number, messagesDigest = 'saved-time:1000') => {
      testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision, chats: [{ ...initial.chats[0], messagesDigest }] } })
      await new Promise(resolve => setTimeout(resolve, 0))
    }
    await cefStore.getState().loadSessionMessages('chat-1')
    await pushSummary(2)
    await pushSummary(3)
    expect(requests.filter(request => request.action === 'getChatMessages')).toHaveLength(1)
    expect(cefStore.getState().sessions[0].messagesDigest).toBe('saved-time:1000')
    await expect(cefStore.getState().loadOlderSessionMessages('chat-1')).resolves.toBe(true)
    expect(cefStore.getState().historyStartIndexBySessionId['chat-1']).toBe(850)
    contentHash = 'edited-content-hash'
    await pushSummary(4, 'new-saved-time:1000')
    expect(requests.filter(request => request.action === 'getChatMessages')).toHaveLength(3)
    expect(cefStore.getState().messages['chat-1'].at(-1)?.content).toBe('edited-content-hash:999')
    expect(cefStore.getState().sessions[0].messagesDigest).toBe('new-saved-time:1000')
    await pushSummary(5, 'new-saved-time:1000')
    expect(requests.filter(request => request.action === 'getChatMessages')).toHaveLength(3)
    await expect(cefStore.getState().loadOlderSessionMessages('chat-1')).resolves.toBe(true)
    expect(cefStore.getState().messages['chat-1'][0].content).toBe('edited-content-hash:850')
  })

  it.each(['search page', 'return to latest'] as const)('preserves the native summary token when loading a %s', async (operation) => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats[0].cliTerminal = undefined
    initial.chats[0].messages = [{ role: 'assistant', content: 'Visible tail', createdAt: '999' }]
    initial.chats[0].messageCount = 1000
    initial.chats[0].messagesDigest = 'saved-time:1000'
    let pageRequests = 0
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      if (parsed.action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else if (parsed.action === 'getChatMessages') {
        ++pageRequests
        const end = parsed.payload.before ?? 1000
        const start = Math.max(0, end - parsed.payload.limit)
        onSuccess(JSON.stringify({ chatId: 'chat-1', messagesDigest: 'content-hash', startIndex: start, totalCount: 1000,
          messages: Array.from({ length: end - start }, (_, index) => ({ role: 'assistant', content: `Message ${start + index}`, createdAt: String(start + index) })),
        }))
      }
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise(resolve => setTimeout(resolve, 0))
    await expect(cefStore.getState().jumpToChatSearchHit({ chatId: 'chat-1', messageIndex: 200, messagesDigest: 'content-hash', field: 'content', excerpt: 'Message 200' }, 'search-request')).resolves.toBe(true)
    if (operation === 'return to latest') {
      testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, chats: [{ ...initial.chats[0], messages: undefined }] } })
      await cefStore.getState().clearChatSearchNavigation()
    }
    expect(cefStore.getState().sessions[0].messagesDigest).toBe('saved-time:1000')
    const beforePush = pageRequests
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 3, chats: [{ ...initial.chats[0], messages: undefined }] } })
    await new Promise(resolve => setTimeout(resolve, 0))
    expect(pageRequests).toBe(beforePush)
    await expect(cefStore.getState().loadOlderSessionMessages('chat-1')).resolves.toBe(operation === 'return to latest')
  })

  it.each(['ACP turn', 'CLI turn', 'streamed tail', 'background chat'])('preserves %s during summary-only history changes', async (live) => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1, live === 'background chat' ? null : 'chat-1')
    initial.chats[0].cliTerminal = live === 'CLI turn'
      ? { ...initial.chats[0].cliTerminal!, processing: true, lifecycleState: 'processing' } : undefined
    if (live === 'ACP turn') initial.chats[0].acpSession = { sessionId: 'live', running: true, processing: true, turnSerial: 1 }
    initial.chats[0].messages = [{ role: 'assistant', content: 'Keep visible text', createdAt: '2026-01-01T00:00:01Z' }]
    initial.chats[0].messageCount = 1
    initial.chats[0].messagesDigest = 'before'
    const requests: string[] = []
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const action = JSON.parse(request).action
      requests.push(action)
      if (action === 'getInitialState') onSuccess(JSON.stringify(initial))
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise(resolve => setTimeout(resolve, 0))
    if (live === 'streamed tail') cefStore.setState(state => ({ messages: { ...state.messages,
      'chat-1': state.messages['chat-1'].map(message => ({ ...message, isStreaming: true })),
    } }))
    const original = cefStore.getState().messages['chat-1']
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, chats: [{
      ...initial.chats[0], messages: undefined, messagesDigest: 'after', messageCount: 1,
    }] } })
    await new Promise(resolve => setTimeout(resolve, 0))
    expect(requests.filter(action => action === 'getChatMessages')).toHaveLength(0)
    expect(cefStore.getState().messages['chat-1']).toBe(original)
  })

  it.each(['new live turn', 'unloaded transcript'])('ignores an idle history refresh superseded by a %s', async (change) => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats[0].cliTerminal = undefined
    initial.chats[0].messages = [{ role: 'assistant', content: 'Saved answer', createdAt: '2026-01-01T00:00:01Z' }]
    initial.chats[0].messageCount = 1
    initial.chats[0].messagesDigest = 'before'
    let finish: ((response: string) => void) | undefined
    testWindow.cefQuery = ({ request, onSuccess }) => {
      if (JSON.parse(request).action === 'getInitialState') onSuccess(JSON.stringify(initial))
      else if (JSON.parse(request).action === 'getChatMessages') finish = onSuccess
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise(resolve => setTimeout(resolve, 0))
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, chats: [{
      ...initial.chats[0], messages: undefined, messagesDigest: 'after', messageCount: 2,
    }] } })
    expect(finish).toBeTypeOf('function')
    const newMessages = [{ id: 'new-answer', sessionId: 'chat-1', role: 'assistant' as const,
      content: 'New live answer', createdAt: new Date(), isStreaming: true }]
    if (change === 'unloaded transcript') cefStore.getState().unloadSessionMessages('chat-1')
    else cefStore.setState({ messages: { 'chat-1': newMessages } })
    finish!(JSON.stringify({ chatId: 'chat-1', messagesDigest: 'after', messages: [
      ...initial.chats[0].messages,
      { role: 'assistant', content: 'Earlier external answer', createdAt: '2026-01-01T00:00:02Z' },
    ] }))
    await new Promise(resolve => setTimeout(resolve, 0))
    expect(cefStore.getState().messages['chat-1']).toBe(change === 'unloaded transcript' ? undefined : newMessages)
  })

  it('allows a failed chat hydration to be retried without losing the transcript', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    let attempts = 0
    testWindow.cefQuery = ({ request, onSuccess, onFailure }) => {
      const action = (JSON.parse(request) as { action: string }).action
      if (action === 'getInitialState') {
        onSuccess(JSON.stringify(makeCppState(1)))
        return
      }
      if (action === 'getChatMessages') {
        attempts += 1
        if (attempts === 1) {
          onFailure(502, 'Remote transcript could not be read.')
        } else {
          onSuccess(JSON.stringify({
            chatId: 'chat-1',
            messagesDigest: 'recovered',
            unchanged: false,
            messages: [{ role: 'user', content: 'Recovered prompt', createdAt: '2026-01-01T00:00:00.000Z' }],
          }))
        }
      }
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    expect(await cefStore.getState().loadSessionMessages('chat-1')).toBe(false)
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(cefStore.getState().chatHistoryErrorBySessionId['chat-1']).toBe('Remote transcript could not be read.')
    const acceptedPage = await cefStore.getState().loadSessionMessages('chat-1', true)
    expect(acceptedPage).toMatchObject({ chatId: 'chat-1', startIndex: 0, totalCount: 1, messagesDigest: 'recovered' })
    expect(acceptedPage && acceptedPage.messages).toBe(cefStore.getState().messages['chat-1'])
    expect(cefStore.getState().chatHistoryErrorBySessionId['chat-1']).toBeUndefined()
    expect(cefStore.getState().messages['chat-1'].map((message) => message.content)).toEqual(['Recovered prompt'])
  })

  it.each(['newer messages', 'unloaded chat'])('keeps %s when native refresh resolves late', async (change) => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    let finishRefresh: ((payload: string) => void) | undefined
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (parsed.action === 'getInitialState') onSuccess(JSON.stringify(makeCppState(1)))
      else if (parsed.action === 'getChatMessages') finishRefresh = onSuccess
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const completion = cefStore.getState().loadSessionMessages('chat-1', true, true)
    expect(requests.filter((request) => request.action === 'getChatMessages').at(-1)?.payload).toEqual({
      chatId: 'chat-1', messagesDigest: '', refreshNative: true, limit: 50, deferToolCallContent: true,
    })
    const newerMessages = [{ id: 'newer', sessionId: 'chat-1', role: 'assistant' as const, content: 'New streamed answer', createdAt: new Date(), isStreaming: true }]
    if (change === 'unloaded chat') cefStore.getState().unloadSessionMessages('chat-1')
    else cefStore.setState({ messages: { 'chat-1': newerMessages } })
    finishRefresh?.(JSON.stringify({ chatId: 'chat-1', messagesDigest: 'late-native', unchanged: false, messages: [{ role: 'assistant', content: 'Old native answer', createdAt: '2026-01-01T00:00:00.000Z' }] }))
    await completion
    expect(cefStore.getState().messages['chat-1']).toBe(change === 'unloaded chat' ? undefined : newerMessages)
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
    let settled = false
    const completion = Promise.resolve(cefStore.getState().loadSessionMessages('chat-1')).then(() => { settled = true })
    await Promise.resolve()
    expect(settled).toBe(false)
    cefStore.getState().unloadSessionMessages('chat-1')
    finishHydration?.(JSON.stringify({ chatId: 'chat-1', messagesDigest: 'late', unchanged: false, messages: [{ role: 'user', content: 'Late', createdAt: '2026-01-01T00:00:00.000Z' }] }))
    await completion
    expect(settled).toBe(true)
    expect(cefStore.getState().messages['chat-1']).toBeUndefined()
  })

  it('ignores a chat hydration failure after the transcript is unloaded', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    let failHydration: ((code: number, message: string) => void) | undefined
    testWindow.cefQuery = ({ request, onSuccess, onFailure }) => {
      const action = (JSON.parse(request) as { action: string }).action
      if (action === 'getInitialState') onSuccess(JSON.stringify(makeCppState(1)))
      else if (action === 'getChatMessages') failHydration = onFailure
    }
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const completion = cefStore.getState().loadSessionMessages('chat-1')
    await Promise.resolve()
    cefStore.getState().unloadSessionMessages('chat-1')
    failHydration?.(502, 'Remote transcript could not be read.')
    await completion
    expect(cefStore.getState().chatHistoryErrorBySessionId['chat-1']).toBeUndefined()
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

  it('retains bootstrap runtime metadata when a newer patch arrives first', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.appVersion = 'V9.9.9'
    initial.runnerProtocolVersion = 3
    let finishInitial: () => void = () => { throw new Error('initial state was not requested') }
    testWindow.cefQuery = ({ onSuccess }) => {
      finishInitial = () => onSuccess(JSON.stringify(initial))
    }

    const { useAppStore: cefStore } = await import('./useAppStore')
    cefStore.setState({ appVersion: 'V0.0.0', runnerProtocolVersion: 0 })
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2 } })
    expect(cefStore.getState().lastAppliedStateRevision).toBe(2)

    finishInitial()
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(cefStore.getState().appVersion).toBe('V9.9.9')
    expect(cefStore.getState().runnerProtocolVersion).toBe(3)
    expect(cefStore.getState().lastAppliedStateRevision).toBe(2)
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

  it('selects the exact companion chat returned by CEF', async () => {
    const previousPath = window.location.pathname
    window.history.replaceState(null, '', '/companion')
    window.localStorage.setItem('uam-companion-token', 'test-token')
    const fetchMock = vi.fn().mockResolvedValue({
      ok: true,
      json: async () => ({ ok: true, data: { chatId: 'phone-created' } }),
    })
    vi.stubGlobal('fetch', fetchMock)
    useAppStore.setState({
      folders: [{ id: 'default', name: 'General', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: new Date() }],
      providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#f97316', description: '' }],
    })

    await expect(useAppStore.getState().addSession('Phone chat', 'default')).resolves.toBe(true)
    expect(JSON.parse((fetchMock.mock.calls[0][1] as RequestInit).body as string).action).toBe('createSession')
    expect(useAppStore.getState().activeSessionId).toBe('phone-created')
    expect(useAppStore.getState().isNewChatModalOpen).toBe(false)
    window.history.replaceState(null, '', previousPath)
    vi.unstubAllGlobals()
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

  it('preserves the native branch rejection cause', async () => {
    window.cefQuery = ({ onFailure }) => onFailure(1, 'The source image is unavailable.')
    await expect(useAppStore.getState().branchFromMessage('chat-1', 7, 'Keep text', ['image'])).rejects.toThrow('The source image is unavailable.')
  })

  it('distinguishes preserved and removed branch attachments, including an idempotent retry', async () => {
    const requests: Array<{ payload: Record<string, unknown> }> = []
    let failed = false
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (!failed && parsed.payload.retainedAttachmentIds) {
        failed = true
        onFailure(1, 'Connection lost; action may have reached the application')
      } else onSuccess(JSON.stringify({ chatId: 'branch-1' }))
    }
    await expect(useAppStore.getState().branchFromMessage('chat-1', 7, '', ['native-image'])).resolves.toBe('branch-1')
    expect(requests[0].payload).toEqual(requests[1].payload)
    expect(requests[0].payload).toMatchObject({ messageIndex: 7, content: '', retainedAttachmentIds: ['native-image'] })
    await useAppStore.getState().branchFromMessage('chat-1', 7, 'Text only', [])
    expect(requests[2].payload.retainedAttachmentIds).toEqual([])
    await useAppStore.getState().branchFromMessage('chat-1', 7)
    expect(requests[3].payload).not.toHaveProperty('retainedAttachmentIds')
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
      { action: 'branchFromMessage', payload: expect.objectContaining({ chatId: 'chat-1', messageIndex: 2, content: 'Edited prompt' }) },
      { action: 'branchFromMessage', payload: expect.objectContaining({ chatId: 'chat-1', messageIndex: 0 }) },
    ])
    expect(requests[0].payload.operationId).toEqual(expect.any(String))
    expect(requests[1].payload.operationId).toEqual(expect.any(String))
    expect(requests[0].payload.operationId).not.toBe(requests[1].payload.operationId)
  })

  it('retries an ambiguous branch response with the same operation ID', async () => {
    const requests: Array<{ payload: { operationId: string } }> = []
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      requests.push(JSON.parse(request))
      if (requests.length === 1) onFailure(0, 'Connection lost. The action may have reached UAM.')
      else onSuccess(JSON.stringify({ chatId: 'branch-1' }))
    }

    await expect(useAppStore.getState().branchFromMessage('chat-1', 0)).resolves.toBe('branch-1')
    expect(requests).toHaveLength(2)
    expect(requests[0].payload.operationId).toBeTruthy()
    expect(requests[1].payload).toEqual(requests[0].payload)
  })

  it('shows a saved branch when its regeneration failed', async () => {
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify({
      chatId: 'branch-1',
      warning: 'Branch was created, but regeneration could not start.',
    }))

    await expect(useAppStore.getState().branchFromMessage('chat-1', 0)).resolves.toBe('branch-1')
    expect(useAppStore.getState().statusLine).toBe('Branch was created, but regeneration could not start.')
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
      memoryEnabled: true, // ponytail: local preview chats inherit the memory default (68a702da)
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

  it('adds the settings scope host only to scoped saves', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }
    useAppStore.setState({
      providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#f97316', description: '' }],
      defaultNewChatProviderId: 'gemini-cli',
      providerChatDefaults: {},
    })

    await useAppStore.getState().setProviderChatDefaults({ defaultNewChatProviderId: 'gemini-cli' }, 'box-1')
    await useAppStore.getState().setProviderChatDefaults({ defaultNewChatProviderId: 'gemini-cli' }, '*')
    await useAppStore.getState().setMcpServers([], 'box-1')
    await useAppStore.getState().setMcpServers([])

    expect(requests[0].payload).toMatchObject({ defaultProviderId: 'gemini-cli', scopeExecutionHostId: 'box-1' })
    expect(requests[1].payload).not.toHaveProperty('scopeExecutionHostId')
    expect(requests[2].payload).toEqual({ servers: [], scopeExecutionHostId: 'box-1' })
    expect(requests[3].payload).toEqual({ servers: [] })
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

  it.each(['callback', 'exception', 'newer-title'])('keeps newer workspace state when a rename rolls back through %s', async (failureMode) => {
    const now = new Date('2026-01-01T00:00:00.000Z')
    let rejectRename: () => void = () => {
      throw new Error('rename request was not sent')
    }
    window.cefQuery = ({ onFailure }) => {
      rejectRename = () => onFailure(500, 'Rename failed')
      if (failureMode === 'exception') throw new Error('CEF context unavailable')
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
            name: failureMode === 'newer-title' ? 'Confirmed' : session.name,
            workspaceDirectory: '/tmp/source/.uam-worktrees/chat-1',
            workspaceIsolationKind: 'gitWorktree',
            workspaceSourceDirectory: '/tmp/source',
            workspaceWorktreeDirectory: '/tmp/source/.uam-worktrees/chat-1',
          }
        : session),
    }))
    if (failureMode !== 'exception') rejectRename()
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(useAppStore.getState().sessions[0]).toMatchObject({
      name: failureMode === 'newer-title' ? 'Confirmed' : 'Original',
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
      historyStartIndexBySessionId: { 'chat-a': 10, 'chat-b': 20, 'chat-c': 0 },
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
    expect(useAppStore.getState().historyStartIndexBySessionId).toEqual({ 'chat-c': 0 })
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

  it.each([false, true])('preserves newer folder state when rename rolls back (backend update: %s)', async (backendUpdate) => {
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
    if (backendUpdate) {
      useAppStore.getState().loadFromCef({ ...makeCppState(10), folders: [
        { id: 'one', title: 'Confirmed', directory: '/tmp/confirmed', collapsed: true },
      ] })
    }
    rejectRename()
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(useAppStore.getState().folders[0]).toMatchObject({
      name: backendUpdate ? 'Confirmed' : 'Original',
      directory: backendUpdate ? '/tmp/confirmed' : '/tmp/one',
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
    await expect(useAppStore.getState().setSessionUamControlEnabled('chat-1', true, true)).resolves.toBe(true)
    expect(requests[3].payload).toEqual({ chatId: 'chat-1', enabled: true, applyOnNextLaunch: true })
    expect(useAppStore.getState().sessions[0].uamControlEnabled).toBe(true)
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

  it('saves and rolls back the explorer path with editor preferences', async () => {
    const previous = useAppStore.getState().fileExplorerApplication
    const requests: Array<{ payload: Record<string, unknown> }> = []
    window.cefQuery = ({ request, onFailure }) => {
      requests.push(JSON.parse(request))
      onFailure(400, 'Invalid application')
    }
    await expect(useAppStore.getState().setEditorSettings({
      defaultEditorPresetId: 'vscode', editorFileAssociations: [], fileExplorerApplication: '/Applications/Alternative.app',
    })).resolves.toBe(false)
    expect(requests[0].payload.fileExplorerApplication).toBe('/Applications/Alternative.app')
    expect(useAppStore.getState().fileExplorerApplication).toBe(previous)
  })

  it('hydrates central resources and restores them after a rejected save', async () => {
    const configuration = {
      enabled: true, instructions: 'Shared context', instructionFiles: ['/instructions/AGENTS.md'],
      skillDirectories: ['/skills'], defaultAgentId: 'plan', uamControlEnabled: true,
      defaultSkills: ['review'], defaultAgents: ['build'],
      mcpOverrides: [{ providerId: 'codex-cli', executionHostId: 'local', serverId: 'uam-mcp', enabled: false }],
    }
    const state = makeCppState(1)
    state.settings.centralProviderConfiguration = configuration
    useAppStore.getState().loadFromCef(state)
    expect(useAppStore.getState().centralProviderConfiguration).toEqual(configuration)
    const requests: Array<{ action: string; payload: unknown }> = []
    window.cefQuery = ({ request, onFailure }) => {
      requests.push(JSON.parse(request))
      onFailure(500, 'Storage unavailable')
    }
    const result = await useAppStore.getState().setCentralProviderConfiguration({ ...configuration, instructions: 'Updated context' })
    expect(result).toEqual({ ok: false, error: 'Storage unavailable' })
    expect(requests[0]).toMatchObject({ action: 'setCentralProviderConfiguration', payload: { instructions: 'Updated context' } })
    expect(useAppStore.getState().centralProviderConfiguration).toEqual(configuration)
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

  it('hydrates persisted VCS switches and enables them for older settings files', () => {
    const cpp = makeCppState(1)
    cpp.settings.gitEnabled = false
    cpp.settings.svnEnabled = true
    useAppStore.getState().loadFromCef(cpp)
    expect(useAppStore.getState()).toMatchObject({ gitEnabled: false, svnEnabled: true })
    useAppStore.getState().loadFromCef(makeCppState(2))
    expect(useAppStore.getState()).toMatchObject({ gitEnabled: true, svnEnabled: true })
  })

  it('keeps VCS switches absent from unrelated or malformed settings patches', async () => {
    const unrelated = sanitizeCppStatePatch({ stateRevision: 2, settings: { theme: 'focus' } })!
    const valid = sanitizeCppStatePatch({ stateRevision: 3, settings: { gitEnabled: true, svnEnabled: false } })!
    const malformed = sanitizeCppStatePatch({ stateRevision: 4, settings: { gitEnabled: 'false', svnEnabled: null } })!
    expect(unrelated.settings).toEqual({ theme: 'focus' })
    expect(valid.settings).toEqual({ gitEnabled: true, svnEnabled: false })
    expect(malformed.settings).toEqual({})
    for (const patch of [unrelated, malformed]) {
      expect(Object.prototype.hasOwnProperty.call(patch.settings, 'gitEnabled')).toBe(false)
      expect(Object.prototype.hasOwnProperty.call(patch.settings, 'svnEnabled')).toBe(false)
    }

    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.settings.gitEnabled = false
    initial.settings.svnEnabled = true
    testWindow.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(initial))
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    expect(testWindow.uamPush).toBeTypeOf('function')
    testWindow.uamPush!({ type: 'statePatch', data: unrelated })
    expect(cefStore.getState()).toMatchObject({ gitEnabled: false, svnEnabled: true })
    testWindow.uamPush!({ type: 'statePatch', data: valid })
    expect(cefStore.getState()).toMatchObject({ gitEnabled: true, svnEnabled: false })
    testWindow.uamPush!({ type: 'statePatch', data: malformed })
    expect(cefStore.getState()).toMatchObject({ gitEnabled: true, svnEnabled: false })
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

  function memorySettingsCanonicalFixture() {
    const state = useAppStore.getState()
    return {
      memoryEnabledDefault: state.memoryEnabledDefault,
      memoryLevelDefault: state.memoryLevelDefault,
      memoryIdleDelaySeconds: state.memoryIdleDelaySeconds,
      memoryRecallBudgetBytes: state.memoryRecallBudgetBytes,
      goalMaxLoopIterations: state.goalMaxLoopIterations,
      acpSetupInactivityTimeoutSeconds: state.acpSetupInactivityTimeoutSeconds,
      acpTurnOutputLimitMiB: state.acpTurnOutputLimitMiB,
      memoryWorkerBindings: state.memoryWorkerBindings,
      permissionReviewerProviderId: state.permissionReviewerProviderId,
      permissionReviewerModelId: state.permissionReviewerModelId,
    }
  }

  it('clamps memory settings before optimistic updates and CEF persistence', async () => {
    const cppState = makeCppState(1)
    useAppStore.getState().loadFromCef(cppState)

    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    const scope = '11111111-1111-4111-8111-111111111111'
    let canonical = memorySettingsCanonicalFixture()
    window.cefQuery = ({ request, onSuccess }) => {
      const envelope = JSON.parse(request)
      requests.push(envelope)
      if (envelope.action === 'getMemorySettings') {
        onSuccess(JSON.stringify({ scope, revision: 1, primaryValidated: true, values: canonical }))
        return
      }
      const payload = envelope.payload
      canonical = { ...canonical, memoryIdleDelaySeconds: payload.idleDelaySeconds, memoryRecallBudgetBytes: payload.recallBudgetBytes,
        goalMaxLoopIterations: payload.goalMaxLoopIterations, acpSetupInactivityTimeoutSeconds: payload.acpSetupInactivityTimeoutSeconds,
        acpTurnOutputLimitMiB: payload.acpTurnOutputLimitMiB }
      onSuccess(JSON.stringify({ scope, revision: 2, primaryValidated: true, values: canonical,
        memoryRequestId: payload.memoryRequestId, outcome: 'committed' }))
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
    expect(requests[0]).toMatchObject({ action: 'getMemorySettings', payload: {} })
    expect(requests[1].action).toBe('setMemorySettings')
    expect(requests[1].payload?.idleDelaySeconds).toBe(30)
    expect(requests[1].payload?.recallBudgetBytes).toBe(8192)
    expect(requests[1].payload?.goalMaxLoopIterations).toBe(200)
    expect(requests[1].payload?.acpSetupInactivityTimeoutSeconds).toBe(3600)
    expect(requests[1].payload?.acpTurnOutputLimitMiB).toBe(4096)
  })

  it('keeps a later memory setting when an older typed rejection settles', async () => {
    const scope = '11111111-1111-4111-8111-111111111111'
    const canonical = memorySettingsCanonicalFixture()
    let revision = 0
    const available: Array<{ payload: Record<string, unknown>; success: (body: string) => void }> = []
    let waiting: ((item: typeof available[number]) => void) | undefined
    window.cefQuery = ({ request, onSuccess }) => {
      const envelope = JSON.parse(request)
      if (envelope.action === 'getMemorySettings') {
        onSuccess(JSON.stringify({ scope, revision: ++revision, primaryValidated: true, values: canonical }))
        return
      }
      const item = { payload: envelope.payload, success: onSuccess }
      if (waiting) { const deliver = waiting; waiting = undefined; deliver(item) } else available.push(item)
    }
    const nextSet = () => available.length ? Promise.resolve(available.shift()!)
      : new Promise<typeof available[number]>((resolve) => { waiting = resolve })
    const older = useAppStore.getState().setMemorySettings({ memoryIdleDelaySeconds: 90 })
    const newer = useAppStore.getState().setMemorySettings({ memoryRecallBudgetBytes: 4096 })
    const first = await nextSet()
    first.success(JSON.stringify({ scope, revision: ++revision, primaryValidated: true, values: canonical,
      memoryRequestId: first.payload.memoryRequestId, outcome: 'rejected' }))
    await expect(older).resolves.toBe(false)
    const second = await nextSet()
    expect(second.payload).not.toHaveProperty('idleDelaySeconds')
    second.success(JSON.stringify({ scope, revision: ++revision, primaryValidated: true, values: { ...canonical, memoryRecallBudgetBytes: 4096 },
      memoryRequestId: second.payload.memoryRequestId, outcome: 'committed' }))
    await expect(newer).resolves.toBe(true)

    expect(useAppStore.getState().memoryIdleDelaySeconds).toBe(60)
    expect(useAppStore.getState().memoryRecallBudgetBytes).toBe(4096)
  })

  it('preserves an unconfirmed memory draft through real full-state and patch ingestion', async () => {
    const testWindow = ensureTestWindow()
    const previousPush = testWindow.uamPush
    const previousQuery = testWindow.cefQuery
    vi.resetModules()
    const scope = '11111111-1111-4111-8111-111111111111'
    let canonical = { memoryEnabledDefault: true, memoryLevelDefault: 'strict', memoryIdleDelaySeconds: 60,
      memoryRecallBudgetBytes: 2048, goalMaxLoopIterations: 200, acpSetupInactivityTimeoutSeconds: 600,
      acpTurnOutputLimitMiB: 1024, memoryWorkerBindings: {}, permissionReviewerProviderId: '', permissionReviewerModelId: '' }
    const sent: string[] = []
    let settingsRevision = 0
    let writes = 0
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const envelope = JSON.parse(request)
      sent.push(envelope.action)
      if (envelope.action === 'getInitialState') onSuccess(JSON.stringify(makeCppState(1)))
      else if (envelope.action === 'getMemorySettings') onSuccess(JSON.stringify({ scope, revision: ++settingsRevision, primaryValidated: true, values: canonical }))
      else if (envelope.action === 'setMemorySettings') {
        expect(envelope.payload.expectedSettingsRevision).toBe(settingsRevision)
        writes += 1
        if (writes === 1) onSuccess(JSON.stringify({ scope, revision: ++settingsRevision, primaryValidated: false,
          memoryRequestId: envelope.payload.memoryRequestId, outcome: 'unknown' }))
        else {
          canonical = { ...canonical, memoryRecallBudgetBytes: envelope.payload.recallBudgetBytes }
          onSuccess(JSON.stringify({ scope, revision: ++settingsRevision, primaryValidated: true, values: canonical,
            memoryRequestId: envelope.payload.memoryRequestId, outcome: 'committed' }))
        }
      }
    }
    try {
      const { useAppStore: store } = await import('./useAppStore')
      store.getState().loadFromCef(makeCppState(2))
      await expect(store.getState().setMemorySettings({ memoryIdleDelaySeconds: 90 })).resolves.toBe(false)
      expect(store.getState().memoryIdleDelaySeconds).toBe(90)
      expect(store.getState().memorySettingsUnconfirmedFields).toEqual(['memoryIdleDelaySeconds'])
      store.getState().loadFromCef(makeCppState(3))
      expect(store.getState().memoryIdleDelaySeconds).toBe(90)
      expect(store.getState().memorySettingsUnconfirmedFields).toEqual(['memoryIdleDelaySeconds'])
      expect(testWindow.uamPush).toBeTypeOf('function')
      testWindow.uamPush!({ type: 'statePatch', data: { stateRevision: 4,
        settings: { memoryIdleDelaySeconds: 60, memoryRecallBudgetBytes: 4096 } } })
      expect(store.getState().memoryIdleDelaySeconds).toBe(90)
      expect(store.getState().memorySettingsUnconfirmedFields).toEqual(['memoryIdleDelaySeconds'])
      expect(store.getState().memoryRecallBudgetBytes).toBe(4096)
      expect(sent.filter((action) => action === 'setMemorySettings')).toHaveLength(1)
      await expect(store.getState().setMemorySettings({ memoryRecallBudgetBytes: 8192 })).resolves.toBe(true)
      expect(store.getState().memoryIdleDelaySeconds).toBe(60)
      expect(store.getState().memoryRecallBudgetBytes).toBe(8192)
      expect(store.getState().memorySettingsUnconfirmedFields).toEqual([])
      expect(sent.filter((action) => action === 'setMemorySettings')).toHaveLength(2)
    } finally {
      testWindow.uamPush = previousPush
      testWindow.cefQuery = previousQuery
    }
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

  it.each(['success', 'failure'] as const)('ignores stale refresh %s after selecting another memory scope', async (result) => {
    const callbacks: Array<{ succeed: (response: string) => void; fail: (code: number, message: string) => void }> = []
    window.cefQuery = ({ onSuccess, onFailure }) => { callbacks.push({ succeed: onSuccess, fail: onFailure }) }
    const globalScope = { scopeType: 'global' as const, folderId: '', label: 'Global', rootPath: '/global' }
    const folderScope = { scopeType: 'folder' as const, folderId: 'project', label: 'Project', rootPath: '/project/.UAM' }
    useAppStore.setState({ memoryLibraryScope: globalScope, memoryLibraryEntries: [] })
    const refreshing = useAppStore.getState().refreshMemoryLibrary()
    const opening = useAppStore.getState().openFolderMemoryLibrary('project')
    if (result === 'success') callbacks[0].succeed(JSON.stringify({ scope: globalScope, entries: [{ id: 'stale' }] }))
    else callbacks[0].fail(500, 'Old scope failed')
    await expect(refreshing).resolves.toBe(false)
    expect(useAppStore.getState()).toMatchObject({
      memoryLibraryScope: { scopeType: 'folder', folderId: 'project' }, memoryLibraryLoading: true,
      memoryLibraryEntries: [], memoryLibraryError: '',
    })
    callbacks[1].succeed(JSON.stringify({ scope: folderScope, entries: [] }))
    await expect(opening).resolves.toBe(true)
    expect(useAppStore.getState().memoryLibraryScope).toEqual(folderScope)
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

  it.each(['all', 'global', 'folder'] as const)('keeps a failed initial %s memory load visible and retryable', async (scopeType) => {
    const requests: Array<{ payload: { scopeType: string; folderId?: string }; succeed: (response: string) => void; fail: (code: number, message: string) => void }> = []
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      requests.push({ payload: JSON.parse(request).payload, succeed: onSuccess, fail: onFailure })
    }
    useAppStore.setState({
      folders: [{ id: 'project', name: 'Project', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: new Date() }],
    })
    const store = useAppStore.getState()
    const opening = scopeType === 'all' ? store.openAllMemoryLibrary()
      : scopeType === 'global' ? store.openGlobalMemoryLibrary()
      : store.openFolderMemoryLibrary('project')
    const scope = useAppStore.getState().memoryLibraryScope
    expect(scope).toMatchObject({ scopeType, folderId: scopeType === 'folder' ? 'project' : '' })
    expect(useAppStore.getState().memoryLibraryLoading).toBe(true)

    requests[0].fail(500, 'Memory root unavailable')
    await expect(opening).resolves.toBe(false)
    expect(useAppStore.getState()).toMatchObject({
      memoryLibraryScope: scope, memoryLibraryLoading: false, memoryLibraryError: 'Memory root unavailable', memoryLibraryEntries: [],
    })

    const retrying = useAppStore.getState().refreshMemoryLibrary()
    expect(requests[1].payload).toMatchObject({ scopeType, folderId: scope?.folderId })
    requests[1].succeed(JSON.stringify({ scope, entries: [] }))
    await expect(retrying).resolves.toBe(true)
    expect(useAppStore.getState().memoryLibraryError).toBe('')
    useAppStore.getState().closeMemoryLibrary()
    expect(useAppStore.getState().memoryLibraryScope).toBeNull()
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

  it.each(['open', 'refresh'])('cancels a late embedded memory %s response before settings close', async (operation) => {
    let finish: ((response: string) => void) | undefined
    ensureTestWindow().cefQuery = vi.fn(({ onSuccess }) => { finish = onSuccess }) as TestWindow['cefQuery']
    useAppStore.setState({ isSettingsOpen: true, memoryLibraryScope: { scopeType: 'all', folderId: '', label: 'All memory', rootPath: '' } })
    const pending = operation === 'open' ? useAppStore.getState().openAllMemoryLibrary() : useAppStore.getState().refreshMemoryLibrary()
    const closeSnapshots: Array<{ open: boolean; hasMemory: boolean }> = []
    const unsubscribe = useAppStore.subscribe(state => closeSnapshots.push({ open: state.isSettingsOpen, hasMemory: Boolean(state.memoryLibraryScope) }))
    try { useAppStore.getState().setSettingsOpen(false) } finally { unsubscribe() }
    expect(closeSnapshots.some(state => !state.open && state.hasMemory)).toBe(false)
    expect(useAppStore.getState().isSettingsOpen).toBe(false)
    expect(useAppStore.getState().memoryLibraryScope).toBeNull()
    finish?.(JSON.stringify({ scope: { scopeType: 'all', folderId: '', label: 'All memory', rootPath: '' }, entries: [] }))
    await expect(pending).resolves.toBe(false)
    expect(useAppStore.getState().memoryLibraryScope).toBeNull()
    expect(useAppStore.getState().memoryLibraryLoading).toBe(false)
  })

  it('preserves explicit standalone memory when settings are already closed', () => {
    const scope = { scopeType: 'all' as const, folderId: '', label: 'All memory', rootPath: '' }
    useAppStore.setState({ isSettingsOpen: false, memoryLibraryScope: scope })
    useAppStore.getState().setSettingsOpen(false)
    expect(useAppStore.getState().memoryLibraryScope).toEqual(scope)
    useAppStore.getState().closeMemoryLibrary()
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

  it('edits queued attachments by retained IDs and keeps omitted selections backward compatible', async () => {
    const first = { id: 'first', name: 'first.png', type: 'image' as const, mimeType: 'image/png', size: 10, path: '/workspace/first.png' }
    const second = { id: 'second', name: 'second.png', type: 'image' as const, mimeType: 'image/png', size: 20, path: '/workspace/second.png' }
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          queuedPrompts: [{ id: 'queued', revision: 1, text: 'Prompt', uamAgentId: '', markdownStoreFiles: [], attachments: [first, second], goalMode: false, goalId: '' }],
        },
      },
    }))

    await expect(useAppStore.getState().editQueuedAcpPrompt('chat-1', 'queued', 1, 'Legacy text')).resolves.toBe(true)
    expect(useAppStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts?.[0].attachments).toEqual([first, second])
    await expect(useAppStore.getState().editQueuedAcpPrompt('chat-1', 'queued', 2, '')).resolves.toBe(true)
    expect(useAppStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts?.[0]).toMatchObject({ revision: 3, text: '', attachments: [first, second] })
    await expect(useAppStore.getState().editQueuedAcpPrompt('chat-1', 'queued', 3, 'Keep one', ['second'])).resolves.toBe(true)
    expect(useAppStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts?.[0]).toMatchObject({ revision: 4, attachments: [second] })
    await expect(useAppStore.getState().editQueuedAcpPrompt('chat-1', 'queued', 4, 'Remove all', [])).resolves.toBe(true)
    expect(useAppStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts?.[0].attachments).toEqual([])
    await expect(useAppStore.getState().editQueuedAcpPrompt('chat-1', 'queued', 5, 'Invalid', ['missing'])).resolves.toBe(false)
    await expect(useAppStore.getState().editQueuedAcpPrompt('chat-1', 'queued', 5, 'Duplicate', ['x', 'x'])).resolves.toBe(false)
  })

  it('sends optional retained attachment IDs only when supplied to native queue edits', async () => {
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    ensureTestWindow().cefQuery = ((params: { request: string; onSuccess?: (response: string) => void }) => {
      requests.push(JSON.parse(params.request))
      params.onSuccess?.('{}')
    }) as unknown as TestWindow['cefQuery']
    await expect(useAppStore.getState().editQueuedAcpPrompt('chat-1', 'queued', 1, 'Legacy')).resolves.toBe(true)
    await expect(useAppStore.getState().editQueuedAcpPrompt('chat-1', 'queued', 2, 'Retain', ['image-id'])).resolves.toBe(true)
    expect(requests.map(({ action, payload }) => ({ action, payload }))).toEqual([
      { action: 'manageQueuedAcpPrompt', payload: { chatId: 'chat-1', operation: 'edit', promptId: 'queued', expectedRevision: 1, text: 'Legacy' } },
      { action: 'manageQueuedAcpPrompt', payload: { chatId: 'chat-1', operation: 'edit', promptId: 'queued', expectedRevision: 2, text: 'Retain', retainedAttachmentIds: ['image-id'] } },
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

  it('routes restart and timeout intent explicitly while ordinary stops remain interrupts', async () => {
    const requests: { action: string; payload?: any }[] = []
    ensureTestWindow().cefQuery = ((params: { request: string; onSuccess?: (response: string) => void }) => {
      requests.push(JSON.parse(params.request)); params.onSuccess?.('{}')
    }) as unknown as TestWindow['cefQuery']
    await useAppStore.getState().resumeGoal('chat-1', 'goal-1', true)
    await useAppStore.getState().stopAcpSession('chat-1', 'timeout')
    await useAppStore.getState().stopAcpSession('chat-1')
    expect(requests.map(({ action, payload }) => ({ action, payload }))).toEqual([
      { action: 'resumeGoal', payload: { chatId: 'chat-1', goalId: 'goal-1', restart: true } },
      { action: 'stopAcpSession', payload: { chatId: 'chat-1', purpose: 'timeout' } },
      { action: 'stopAcpSession', payload: { chatId: 'chat-1' } },
    ])
  })

  it('acknowledges only the observed attention revision on deliberate interaction', async () => {
    const requests: { action: string; payload?: any }[] = []
    ensureTestWindow().cefQuery = ((params: { request: string; onSuccess?: (response: string) => void }) => {
      requests.push(JSON.parse(params.request)); params.onSuccess?.('{}')
    }) as unknown as TestWindow['cefQuery']
    const state = makeCppState(1)
    state.chats[0].attentionRevision = 'completion-one'
    useAppStore.getState().loadFromCef(state)
    expect(useAppStore.getState().sessions[0].attentionRevision).toBe('completion-one')
    expect(requests).toEqual([])
    await useAppStore.getState().acknowledgeChatAttention('chat-1', useAppStore.getState().sessions[0].attentionRevision!)
    expect(requests).toEqual([expect.objectContaining({ action: 'acknowledgeChatAttention', payload: { chatId: 'chat-1', attentionRevision: 'completion-one' } })])
  })

  it('retains completion revisions through full and patch pushes and clears acknowledgements', async () => {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(makeCppState(1)))
    const { useAppStore: cefStore } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    const completed = makeCppState(2)
    completed.chats[0].attentionRevision = 'completion-one'
    testWindow.uamPush?.({ type: 'stateUpdate', data: completed })
    expect(cefStore.getState().sessions[0].attentionRevision).toBe('completion-one')
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 3, chats: [{ ...completed.chats[0], attentionRevision: 'completion-two' }] } })
    expect(cefStore.getState().sessions[0].attentionRevision).toBe('completion-two')
    testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 4, chats: [{ ...completed.chats[0], attentionRevision: '' }] } })
    expect(cefStore.getState().sessions[0].attentionRevision).toBe('')
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

	it('allows UAM computer use to be enabled before target approval', async () => {
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

	  await expect(useAppStore.getState().setSessionComputerUseEnabled('chat-1', true)).resolves.toEqual({ ok: true })
	  expect(requests).toEqual([expect.objectContaining({ action: 'setChatComputerUseEnabled' })])
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

  it('loads a recent companion page, prepends older messages, and resets on a changed digest', async () => {
    window.history.replaceState(null, '', '/companion')
    const originalStorage = Object.getOwnPropertyDescriptor(window, 'localStorage')
    const stored = new Map<string, string>()
    Object.defineProperty(window, 'localStorage', { configurable: true, value: {
      getItem: (key: string) => stored.get(key) ?? null,
      setItem: (key: string, value: string) => { stored.set(key, value) },
      removeItem: (key: string) => { stored.delete(key) },
      clear: () => { stored.clear() },
    } })
    window.localStorage.setItem('uam-companion-token', 'test-token')
    vi.resetModules()
    const pages = [
      { chatId: 'chat-1', messagesDigest: 'first', startIndex: 2, totalCount: 4,
        messages: [
          { role: 'user', content: 'third', createdAt: '2026-01-01T00:00:02Z' },
          { role: 'assistant', content: 'fourth', createdAt: '2026-01-01T00:00:03Z' },
        ] },
      { chatId: 'chat-1', messagesDigest: 'first', startIndex: 0, totalCount: 4,
        messages: [
          { role: 'user', content: 'first', createdAt: '2026-01-01T00:00:00Z' },
          { role: 'assistant', content: 'second', createdAt: '2026-01-01T00:00:01Z' },
        ] },
      { chatId: 'chat-1', messagesDigest: 'second', startIndex: 3, totalCount: 5,
        messages: [
          { role: 'assistant', content: 'fourth', createdAt: '2026-01-01T00:00:03Z' },
          { role: 'user', content: 'fifth', createdAt: '2026-01-01T00:00:04Z' },
        ] },
    ]
    let companionStore: typeof useAppStore = useAppStore
    const requests: Array<{ action: string; payload: Record<string, unknown> }> = []
    vi.stubGlobal('fetch', vi.fn(async (_url: string, init: RequestInit) => {
      const request = JSON.parse(String(init.body))
      requests.push(request)
      if (request.action === 'sendAcpPrompt') {
        companionStore.setState((state) => {
          const fifth = state.messages['chat-1'][1]
          return {
            messages: { ...state.messages, 'chat-1': [fifth, { ...fifth, id: 'sixth', content: 'sixth' }] },
            historyStartIndexBySessionId: { ...state.historyStartIndexBySessionId, 'chat-1': 4 },
            sessions: state.sessions.map((session) => ({ ...session, messageCount: 6 })),
          }
        })
        return { ok: true, json: async () => ({ ok: true, data: {} }) }
      }
      return { ok: true, json: async () => ({ ok: true, data: pages.shift() }) }
    }))
    try {
      companionStore = (await import('./useAppStore')).useAppStore
      const now = new Date()
      companionStore.setState({ sessions: [{ id: 'chat-1', name: 'Chat', viewMode: 'chat',
        folderId: 'default', createdAt: now, updatedAt: now }] })

      await companionStore.getState().loadSessionMessages('chat-1')
      expect(companionStore.getState().messages['chat-1'].map((message) => message.content)).toEqual(['third', 'fourth'])
      expect(companionStore.getState().historyStartIndexBySessionId['chat-1']).toBe(2)
      expect(await companionStore.getState().loadOlderSessionMessages('chat-1')).toBe(true)
      expect(companionStore.getState().messages['chat-1'].map((message) => message.content)).toEqual(['first', 'second', 'third', 'fourth'])
      expect(companionStore.getState().historyStartIndexBySessionId['chat-1']).toBe(0)

      await companionStore.getState().loadSessionMessages('chat-1', true)
      expect(companionStore.getState().messages['chat-1'].map((message) => message.content)).toEqual(['fourth', 'fifth'])
      expect(companionStore.getState().historyStartIndexBySessionId['chat-1']).toBe(3)
      expect(companionStore.getState().sessions[0].messageCount).toBe(5)
      expect(await companionStore.getState().sendAcpPrompt('chat-1', 'sixth')).toBe(true)
      expect(companionStore.getState().messages['chat-1'].map((message) => message.content)).toEqual(['fifth', 'sixth'])
      expect(requests.slice(0, 3).map((request) => request.payload)).toEqual([
        expect.objectContaining({ chatId: 'chat-1', limit: 50, deferToolCallContent: true }),
        { chatId: 'chat-1', limit: 100, before: 2, deferToolCallContent: true },
        expect.objectContaining({ chatId: 'chat-1', limit: 50, deferToolCallContent: true }),
      ])
    } finally {
      vi.unstubAllGlobals()
      if (originalStorage) Object.defineProperty(window, 'localStorage', originalStorage)
      else delete (window as Window & { localStorage?: Storage }).localStorage
      window.history.replaceState(null, '', '/')
    }
  })
})


describe('bulk workspace deletion', () => {
  it('sends one batch, keeps state on failure, and removes native-reported dependent chats on success', async () => {
    const now = new Date()
    const testWindow = ensureTestWindow()
    const originalQuery = testWindow.cefQuery
    const requests: { action: string; payload: unknown }[] = []
    let succeed = false
    testWindow.cefQuery = ({ request, onSuccess, onFailure }) => {
      requests.push(JSON.parse(request))
      if (succeed) onSuccess(JSON.stringify({ deletedChatIds: ['hidden'], selectedChatId: 'keep' }))
      else onFailure(409, 'A runtime is running.')
    }
    useAppStore.setState({
      folders: ['a', 'b', 'keep'].map((id) => ({ id, name: id, parentId: null, directory: `/tmp/${id}`, isExpanded: true, createdAt: now })),
      sessions: ['a', 'b', 'hidden', 'keep'].map((id) => ({ id, name: id, folderId: id === 'hidden' ? null : id, viewMode: 'chat', createdAt: now, updatedAt: now })),
      activeSessionId: 'a', messages: {},
    })
    try {
      await expect(useAppStore.getState().deleteFolders(['a', 'b', 'a'])).resolves.toBe(false)
      expect(useAppStore.getState().folders).toHaveLength(3)
      expect(useAppStore.getState().sessions).toHaveLength(4)
      expect(useAppStore.getState().workspaceDeletionError).toBe('A runtime is running.')
      succeed = true
      await expect(useAppStore.getState().deleteFolders(['a', 'b', 'a'])).resolves.toBe(true)
      expect(requests).toHaveLength(2)
      expect(useAppStore.getState().workspaceDeletionError).toBe('')
      expect(requests[1]).toMatchObject({ action: 'deleteFolders', payload: { folderIds: ['a', 'b'] } })
      expect(useAppStore.getState().folders.map((folder) => folder.id)).toEqual(['keep'])
      expect(useAppStore.getState().sessions.map((session) => session.id)).toEqual(['keep'])
      expect(useAppStore.getState().activeSessionId).toBe('keep')
      await expect(useAppStore.getState().deleteFolders([])).resolves.toBe(false)
      await expect(useAppStore.getState().deleteFolders(['missing'])).resolves.toBe(false)
      expect(requests).toHaveLength(2)
    } finally {
      testWindow.cefQuery = originalQuery
    }
  })
})


describe('provider model catalogue transport capacity', () => {
  const catalogs = Array.from({ length: 513 }, (_, index) => ({
    providerId: 'claude-cli',
    executionHostId: 'local',
    workspaceDirectory: `/tmp/private-worktree-${index}`,
    availableModels: [{ id: `cached-model-${index}`, name: `Cached ${index}`, description: '' }],
    currentModelId: '',
    modelsLoading: false,
    modelRefreshError: '',
  }))

  it('retains the 513th persisted worktree catalogue during startup hydration', () => {
    const state = sanitizeCppAppState({ ...makeCppState(1), providerModelCatalogs: catalogs })
    expect(state).not.toBeNull()
    useAppStore.getState().loadFromCef(state!)
    expect(useAppStore.getState().providerModelCatalogs).toHaveLength(513)
    expect(useAppStore.getState().providerModelCatalogs[512]).toMatchObject(catalogs[512])
  })

  it('retains the 513th persisted worktree catalogue in a state patch', () => {
    const patch = sanitizeCppStatePatch({ stateRevision: 2, providerModelCatalogs: catalogs })
    expect(patch?.providerModelCatalogs).toHaveLength(513)
    expect(patch?.providerModelCatalogs?.[512]).toMatchObject(catalogs[512])
  })
})


describe('SVN update settings transport', () => {
  it('loads explicit SVN update values and preserves them through partial patches', () => {
    const full = sanitizeCppAppState({ ...makeCppState(1), settings: { ...makeCppState(1).settings, svnUpdateBeforeChat: true } })
    expect(full?.settings.svnUpdateBeforeChat).toBe(true)
    const enabled = sanitizeCppStatePatch({ stateRevision: 2, settings: { svnUpdateBeforeChat: false } })
    expect(enabled?.settings?.svnUpdateBeforeChat).toBe(false)
    const unrelated = sanitizeCppStatePatch({ stateRevision: 3, settings: { gitEnabled: false } })
    expect(unrelated?.settings).not.toHaveProperty('svnUpdateBeforeChat')
    const malformed = sanitizeCppStatePatch({ stateRevision: 4, settings: { svnUpdateBeforeChat: 'true' } })
    expect(malformed?.settings).not.toHaveProperty('svnUpdateBeforeChat')
  })
})


describe('chat memory inheritance parity', () => {
  it('creates mock chats with inherited memory and preserves explicit memory across provider switches', async () => {
    delete window.cefQuery
    useAppStore.setState({
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#fff', description: '' },
        { id: 'codex-cli', name: 'Codex', shortName: 'Codex', color: '#fff', description: '' },
      ],
      executionHosts: [{ id: 'local', label: 'Local', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: '', lastSeenAt: '' }],
      sessions: [],
      folders: [{ id: 'memory-workspace', name: 'Private', parentId: null, directory: '/tmp/private-memory-workspace', isExpanded: true, createdAt: new Date() }],
      providerChatDefaults: {
        'gemini-cli': { modelId: '', approvalMode: 'default', commandSafetyTier: 'off', memoryEnabled: false, memoryLevel: 'off', reasoningEffort: '', serviceTier: '' },
        'codex-cli': { modelId: '', approvalMode: 'default', commandSafetyTier: 'off', memoryEnabled: true, memoryLevel: 'strict', reasoningEffort: '', serviceTier: '' },
      },
      acpBindingBySessionId: {},
      cliBindingBySessionId: {},
    })
    expect(await useAppStore.getState().addSession('Inherited', 'memory-workspace', 'gemini-cli')).toBe(true)
    const id = useAppStore.getState().sessions[0].id
    expect(useAppStore.getState().sessions[0]).toMatchObject({ memoryLevel: 'default', memoryEnabled: true })
    expect(await useAppStore.getState().setSessionProvider(id, 'codex-cli')).toBe(true)
    expect(useAppStore.getState().sessions[0]).toMatchObject({ memoryLevel: 'default', memoryEnabled: true })
    useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => ({ ...session, memoryLevel: 'off' as const, memoryEnabled: false })) }))
    expect(await useAppStore.getState().setSessionProvider(id, 'gemini-cli')).toBe(true)
    expect(useAppStore.getState().sessions[0]).toMatchObject({ memoryLevel: 'off', memoryEnabled: false })
  })
})


describe('computer use stop retry transport', () => {
  it('retries Off through CEF after native revocation failed while redundant On remains a no-op', async () => {
    const now = new Date()
    useAppStore.setState({ sessions: [{ id: 'stop-retry', name: 'Private', viewMode: 'chat', folderId: 'private', computerUseEnabled: false, createdAt: now, updatedAt: now }] })
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onFailure }) => { requests.push(JSON.parse(request)); onFailure(500, 'Control revocation is still pending.') }
    const result = await useAppStore.getState().setSessionComputerUseEnabled('stop-retry', false)
    expect(requests).toEqual([expect.objectContaining({ action: 'setChatComputerUseEnabled', payload: { chatId: 'stop-retry', enabled: false } })])
    expect(result).toEqual({ ok: false, error: 'Control revocation is still pending.' })
    expect(useAppStore.getState().sessions[0].computerUseEnabled).toBe(false)
    useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => ({ ...session, computerUseEnabled: true })) }))
    expect(await useAppStore.getState().setSessionComputerUseEnabled('stop-retry', true)).toEqual({ ok: true })
    expect(requests).toHaveLength(1)
  })
})


describe('managed initial receipt R83', () => {
  const prompt = '@reviewer inspect this change'
  const parentMessage = { role: 'user' as const, content: prompt, createdAt: '2026-10-17T13:00:00.000Z', markdownStoreFiles: ['/skills/review.md'] }
  const skill = { id: 'managed-skill', title: 'Review rules', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', filePath: '/skills/review.md' }

  async function begin(patchWithoutSession = false) {
    const testWindow = ensureTestWindow()
    vi.resetModules()
    testWindow.dispatchEvent = vi.fn(() => true)
    const initial = makeCppState(1)
    initial.chats[0] = { ...initial.chats[0], providerId: 'codex-cli', messages: [], messageCount: 0,
      acpSession: { sessionId: '', running: false, processing: false, turnSerial: 0 } }
    const patchChat = initial.chats[0]
    if (patchWithoutSession) { delete patchChat.acpSession; initial.chats = []; initial.selectedChatId = null }
    let finish: (body: unknown) => void = () => { throw new Error('Managed request not started') }
    let sends = 0
    testWindow.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      if (parsed.action === 'sendAcpPrompt') { sends += 1; finish = (body) => onSuccess(JSON.stringify(body)) }
      else if (parsed.action === 'getChatMessages') onSuccess(JSON.stringify({ chatId: 'chat-1', messages: [], startIndex: 0, totalCount: 0 }))
      else onSuccess(JSON.stringify(initial))
    }
    const { useAppStore: store } = await import('./useAppStore')
    await new Promise((resolve) => setTimeout(resolve, 0))
    if (patchWithoutSession) testWindow.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, chats: [patchChat], selectedChatId: patchChat.id } })
    store.getState().attachMarkdownStoreEntry('chat-1', skill)
    const sending = store.getState().sendAcpPrompt('chat-1', prompt)
    return { store, sending, initial, testWindow, finish: (body: unknown) => finish(body), sends: () => sends }
  }

  it('keeps a recorded delegation error separate from runtime state after a session-less public chat patch', async () => {
    const task = await begin(true)
    expect(task.store.getState().sessions.some((session) => session.id === 'chat-1')).toBe(true)
    expect(task.store.getState().acpBindingBySessionId['chat-1']).toBeUndefined()
    task.finish({ managedMention: { inputRecorded: true, delegationState: 'uncertain', parentMessageIndex: 0, parentMessage,
      error: 'The request was saved, but child creation was not confirmed.' } })
    await expect(task.sending).resolves.toBe(true)
    const binding = task.store.getState().acpBindingBySessionId['chat-1']
    expect(binding?.promptActionError?.message).toBe('The request was saved, but child creation was not confirmed.')
    expect(binding?.lifecycleState).toBe('stopped')
    expect(binding?.protocolKind).toBe('')
    expect(binding?.providerId).toBe('codex-cli')
    expect(binding?.processing).toBe(false)
    expect(task.store.getState().messages['chat-1']).toHaveLength(1)
    expect(task.sends()).toBe(1)
  })

  it.each(['rejected', 'uncertain'] as const)('retains resources when parent publication is %s', async (delegationState) => {
    const task = await begin()
    task.finish({ managedMention: { inputRecorded: false, delegationState, error: 'Parent publication was not confirmed.' } })
    await expect(task.sending).resolves.toBe(false)
    expect(task.store.getState().messages['chat-1']).toEqual([])
    expect(task.store.getState().markdownStoreAttachedBySessionId['chat-1'].map((entry) => entry.filePath)).toEqual([skill.filePath])
    expect(task.store.getState().acpBindingBySessionId['chat-1'].promptActionError?.message).toBe('Parent publication was not confirmed.')
    expect(task.sends()).toBe(1)
  })

  it.each(['committed', 'rejected', 'uncertain'] as const)('records authoritative saved input when delegation is %s', async (delegationState) => {
    const task = await begin()
    const added = { ...skill, id: 'later', filePath: '/skills/later.md' }
    task.store.getState().attachMarkdownStoreEntry('chat-1', added)
    task.finish({ managedMention: { inputRecorded: true, delegationState, parentMessageIndex: 0, parentMessage,
      ...(delegationState === 'committed' ? { runId: 'run-actual', transcriptChatId: 'transcript-actual' } : { error: 'Child creation was not confirmed.' }) } })
    await expect(task.sending).resolves.toBe(true)
    expect(task.store.getState().messages['chat-1']).toEqual([expect.objectContaining({
      id: `cef-m-chat-1-${Date.parse(parentMessage.createdAt)}-0-user`, nativeMessageIndex: 0, content: prompt,
      createdAt: new Date(parentMessage.createdAt),
    })])
    expect(task.store.getState().markdownStoreAttachedBySessionId['chat-1'].map((entry) => entry.filePath)).toEqual([added.filePath])
    expect(task.store.getState().acpBindingBySessionId['chat-1'].processing).toBe(false)
    expect(task.store.getState().acpBindingBySessionId['chat-1'].promptActionError?.message).toBe(
      delegationState === 'committed' ? undefined : 'Child creation was not confirmed.')
    expect(task.sends()).toBe(1)
  })

  it.each(['push-before-ack', 'ack-before-push'] as const)('deduplicates the exact parent row across %s', async (order) => {
    const task = await begin()
    const push = () => task.testWindow.uamPush?.({ type: 'stateUpdate', data: { ...task.initial, stateRevision: 2,
      chats: [{ ...task.initial.chats[0], messageCount: 1, messages: [parentMessage] }] } })
    if (order === 'push-before-ack') push()
    task.finish({ managedMention: { inputRecorded: true, delegationState: 'committed', parentMessageIndex: 0, parentMessage,
      runId: 'run-actual', transcriptChatId: 'transcript-actual' } })
    await expect(task.sending).resolves.toBe(true)
    expect(task.store.getState().messages['chat-1']).toHaveLength(1)
    expect(task.store.getState().messages['chat-1'][0].nativeMessageIndex).toBe(0)
    if (order === 'ack-before-push') push()
    expect(task.store.getState().messages['chat-1']).toHaveLength(1)
    expect(task.store.getState().messages['chat-1'][0].nativeMessageIndex).toBe(0)
  })

  it('uses the authoritative receipt index when the renderer count was stale without inventing missing history', async () => {
    const task = await begin()
    task.finish({ managedMention: { inputRecorded: true, delegationState: 'committed', parentMessageIndex: 2, parentMessage,
      runId: 'run-actual', transcriptChatId: 'transcript-actual' } })
    await expect(task.sending).resolves.toBe(true)
    expect(task.store.getState().messages['chat-1']).toEqual([])
    expect(task.store.getState().sessions.find((session) => session.id === 'chat-1')?.messageCount).toBe(3)
    expect(task.store.getState().markdownStoreAttachedBySessionId['chat-1']).toEqual([])
    expect(task.sends()).toBe(1)
  })

  it.each(['folder', 'workspace', 'provider', 'host'] as const)('acknowledges recorded input after a %s change without projecting stale history', async (field) => {
    const task = await begin()
    task.store.setState((state) => ({ sessions: state.sessions.map((session) => session.id !== 'chat-1' ? session : {
      ...session, ...(field === 'folder' ? { folderId: 'moved-folder' } : field === 'workspace' ? { workspaceDirectory: '/new/workspace' }
        : field === 'provider' ? { providerId: 'claude-cli' } : { executionHostId: 'different-host' }),
    }) }))
    const currentRows = task.store.getState().messages['chat-1']
    task.finish({ managedMention: { inputRecorded: true, delegationState: 'committed', parentMessageIndex: 0, parentMessage,
      runId: 'run-actual', transcriptChatId: 'transcript-actual' } })
    await expect(task.sending).resolves.toBe(true)
    expect(task.store.getState().messages['chat-1']).toBe(currentRows)
    expect(task.store.getState().markdownStoreAttachedBySessionId['chat-1']).toEqual([])
    expect(task.sends()).toBe(1)
  })

  it.each(['deleted', 'recreated'] as const)('does not resurrect a %s root on a late receipt', async (scenario) => {
    const task = await begin()
    task.store.setState((state) => ({ sessions: scenario === 'deleted' ? [] : state.sessions.map((session) =>
      session.id === 'chat-1' ? { ...session, createdAt: new Date('2026-10-17T13:30:00.000Z') } : session) }))
    task.finish({ managedMention: { inputRecorded: true, delegationState: 'committed', parentMessageIndex: 0, parentMessage,
      runId: 'run-actual', transcriptChatId: 'transcript-actual' } })
    await expect(task.sending).resolves.toBe(false)
    expect(task.store.getState().messages['chat-1']).toEqual([])
    expect(task.store.getState().markdownStoreAttachedBySessionId['chat-1']).toHaveLength(1)
  })

  it('preserves a reattached skill and a newer authoritative tail on late acknowledgement', async () => {
    const task = await begin()
    task.store.getState().detachMarkdownStoreEntry('chat-1', skill.filePath)
    task.store.getState().attachMarkdownStoreEntry('chat-1', skill)
    const assistant = { role: 'assistant' as const, content: 'Newer parent output', createdAt: '2026-10-17T13:00:01.000Z' }
    task.testWindow.uamPush?.({ type: 'stateUpdate', data: { ...task.initial, stateRevision: 2,
      chats: [{ ...task.initial.chats[0], messageCount: 2, messages: [parentMessage, assistant] }] } })
    const rows = task.store.getState().messages['chat-1']
    task.finish({ managedMention: { inputRecorded: true, delegationState: 'committed', parentMessageIndex: 0, parentMessage,
      runId: 'run-actual', transcriptChatId: 'transcript-actual' } })
    await expect(task.sending).resolves.toBe(true)
    expect(task.store.getState().messages['chat-1']).toBe(rows)
    expect(task.store.getState().messages['chat-1'].map((message) => message.content)).toEqual([prompt, assistant.content])
    expect(task.store.getState().markdownStoreAttachedBySessionId['chat-1'].map((entry) => entry.filePath)).toEqual([skill.filePath])
  })

  it.each([
    null,
    { inputRecorded: false, delegationState: 'committed', runId: 'impossible' },
    { inputRecorded: true, delegationState: 'committed', parentMessageIndex: -1, parentMessage, runId: 'run', transcriptChatId: 'child' },
    { inputRecorded: true, delegationState: 'committed', parentMessageIndex: 0, parentMessage: { ...parentMessage, content: 'different request' }, runId: 'run', transcriptChatId: 'child' },
  ])('refuses a malformed present receipt without ordinary-send fallback: %j', async (managedMention) => {
    const task = await begin()
    task.finish({ managedMention })
    await expect(task.sending).resolves.toBe(false)
    expect(task.store.getState().messages['chat-1']).toEqual([])
    expect(task.store.getState().markdownStoreAttachedBySessionId['chat-1']).toHaveLength(1)
    expect(task.store.getState().acpBindingBySessionId['chat-1'].promptActionError?.message).toBeTruthy()
    expect(task.sends()).toBe(1)
  })
})


// Additive TESTONLY controls: observed publication is fallback, never save authority.
describe('observed memory fallback through real store ingestion, authored UNRUN', () => {
  async function fixture(firstUnknown = false) {
    const testWindow = ensureTestWindow()
    const previousQuery = testWindow.cefQuery, previousPush = testWindow.uamPush
    vi.resetModules()
    const scope = '11111111-1111-4111-8111-111111111111'
    const values = { memoryEnabledDefault: true, memoryLevelDefault: 'strict', memoryIdleDelaySeconds: 60,
      memoryRecallBudgetBytes: 2048, goalMaxLoopIterations: 200, acpSetupInactivityTimeoutSeconds: 600,
      acpTurnOutputLimitMiB: 1024, memoryWorkerBindings: {}, permissionReviewerProviderId: '', permissionReviewerModelId: '' }
    const actions: string[] = []
    const gets: { done: boolean; fail: () => void }[] = []
    const saves: Promise<boolean>[] = []
    let closing = false, getCount = 0
    testWindow.cefQuery = ({ request, onSuccess, onFailure }) => {
      const envelope = JSON.parse(request)
      actions.push(envelope.action)
      if (envelope.action === 'getInitialState') { onSuccess(JSON.stringify(makeCppState(1))); return }
      if (envelope.action === 'getMemorySettings') {
        ++getCount
        if (firstUnknown && getCount === 1) {
          onSuccess(JSON.stringify({ scope, revision: 1, primaryValidated: true, values })); return
        }
        const held = { done: false, fail: () => {
          if (held.done) return
          held.done = true; onFailure(500, 'Private prerequisite GET failed before dispatch')
        } }
        gets.push(held)
        if (closing) held.fail()
        return
      }
      if (envelope.action === 'setMemorySettings') {
        onSuccess(JSON.stringify({ scope, revision: 2, primaryValidated: false,
          memoryRequestId: envelope.payload.memoryRequestId, outcome: 'unknown' })); return
      }
      onSuccess('{}')
    }
    try {
      const { useAppStore: store } = await import('./useAppStore')
      store.getState().loadFromCef(makeCppState(10))
      expect(store.getState().lastAppliedStateRevision).toBe(10)
      expect(store.getState().memoryIdleDelaySeconds).toBe(60)
      expect(testWindow.uamPush).toBeTypeOf('function')
      const start = (seconds: number) => {
        const promise = store.getState().setMemorySettings({ memoryIdleDelaySeconds: seconds })
        saves.push(promise)
        expect(store.getState().memoryIdleDelaySeconds).toBe(seconds)
        return promise
      }
      const push = (kind: 'statePatch' | 'stateUpdate', revision: number, seconds?: number) => {
        if (kind === 'statePatch') testWindow.uamPush!({ type: kind, data: { stateRevision: revision,
          settings: seconds === undefined ? { theme: 'light' } : { memoryIdleDelaySeconds: seconds } } })
        else {
          const full = makeCppState(revision)
          testWindow.uamPush!({ type: kind, data: { ...full, settings: { ...full.settings, memoryIdleDelaySeconds: seconds } } })
        }
      }
      const counts = (get: number, set = 0) => {
        expect(actions.filter(action => action === 'getMemorySettings')).toHaveLength(get)
        expect(actions.filter(action => action === 'setMemorySettings')).toHaveLength(set)
      }
      return { store, start, push, gets, counts, async close() {
        try {
          closing = true
          gets.forEach(get => get.fail())
          await Promise.allSettled(saves)
          expect(gets.every(get => get.done)).toBe(true)
        } finally {
          testWindow.cefQuery = previousQuery; testWindow.uamPush = previousPush
        }
      } }
    } catch (error) {
      closing = true; gets.forEach(get => get.fail()); await Promise.allSettled(saves)
      testWindow.cefQuery = previousQuery; testWindow.uamPush = previousPush
      throw error
    }
  }

  it.each(['statePatch', 'stateUpdate'] as const)('restores accepted observed75 after held prerequisite GET fails following %s', async kind => {
    const f = await fixture()
    try {
      const pending = f.start(90)
      expect(f.gets).toHaveLength(1); expect(f.gets[0].done).toBe(false); f.counts(1)
      f.push(kind, 11, 75)
      expect(f.store.getState().lastAppliedStateRevision).toBe(11)
      expect(f.store.getState().memoryIdleDelaySeconds).toBe(90)
      expect(f.store.getState().memorySettingsUnconfirmedFields).toEqual([])
      expect(f.gets[0].done).toBe(false); f.counts(1)
      f.gets[0].fail()
      await expect(pending).resolves.toBe(false)
      expect(f.store.getState().memoryIdleDelaySeconds).toBe(75)
      expect(f.store.getState().memorySettingsUnconfirmedFields).toEqual([])
      f.counts(1)
    } finally { await f.close() }
  })

  it.each(['unrelated theme patch', 'stale memory revision'] as const)('does not adopt optimistic90 as fallback from %s', async change => {
    const f = await fixture()
    try {
      const pending = f.start(90)
      expect(f.gets).toHaveLength(1); expect(f.gets[0].done).toBe(false)
      if (change === 'unrelated theme patch') {
        f.push('statePatch', 11)
        expect(f.store.getState().lastAppliedStateRevision).toBe(11)
        expect(f.store.getState().theme).toBe('light')
      } else {
        f.push('statePatch', 9, 75)
        expect(f.store.getState().lastAppliedStateRevision).toBe(10)
      }
      expect(f.store.getState().memoryIdleDelaySeconds).toBe(90)
      f.gets[0].fail()
      await expect(pending).resolves.toBe(false)
      expect(f.store.getState().memoryIdleDelaySeconds).toBe(60)
      f.counts(1)
    } finally { await f.close() }
  })

  it('keeps newer120 while older90 is held then reveals observed75 after both prerequisite GET failures', async () => {
    const f = await fixture()
    try {
      const older = f.start(90)
      expect(f.gets).toHaveLength(1)
      f.push('statePatch', 11, 75)
      expect(f.store.getState().lastAppliedStateRevision).toBe(11)
      expect(f.store.getState().memoryIdleDelaySeconds).toBe(90)
      const newer = f.start(120)
      expect(f.gets).toHaveLength(1); f.counts(1)
      f.gets[0].fail()
      await expect(older).resolves.toBe(false)
      expect(f.store.getState().memoryIdleDelaySeconds).toBe(120)
      expect(f.gets).toHaveLength(2); expect(f.gets[1].done).toBe(false)
      f.gets[1].fail()
      await expect(newer).resolves.toBe(false)
      expect(f.store.getState().memoryIdleDelaySeconds).toBe(75)
      expect(f.store.getState().memorySettingsUnconfirmedFields).toEqual([])
      f.counts(2)
    } finally { await f.close() }
  })

  it('does not settle older Unknown90 from observed75 and reveals it again after newer120 is NotSent', async () => {
    const f = await fixture(true)
    try {
      await expect(f.start(90)).resolves.toBe(false)
      expect(f.store.getState().memoryIdleDelaySeconds).toBe(90)
      expect(f.store.getState().memorySettingsUnconfirmedFields).toEqual(['memoryIdleDelaySeconds'])
      f.counts(1, 1)
      f.push('statePatch', 11, 75)
      expect(f.store.getState().lastAppliedStateRevision).toBe(11)
      expect(f.store.getState().memoryIdleDelaySeconds).toBe(90)
      expect(f.store.getState().memorySettingsUnconfirmedFields).toEqual(['memoryIdleDelaySeconds'])
      const newer = f.start(120)
      expect(f.store.getState().memorySettingsUnconfirmedFields).toEqual([])
      expect(f.gets).toHaveLength(1); expect(f.gets[0].done).toBe(false)
      f.gets[0].fail()
      await expect(newer).resolves.toBe(false)
      expect(f.store.getState().memoryIdleDelaySeconds).toBe(90)
      expect(f.store.getState().memorySettingsUnconfirmedFields).toEqual(['memoryIdleDelaySeconds'])
      f.counts(2, 1)
    } finally { await f.close() }
  })
})
