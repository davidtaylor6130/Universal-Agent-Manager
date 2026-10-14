/**
 * Review-only fake CEF backend. Served by `npm run dev` at /mock.html; never part of the production bundle.
 * Answers the bridge actions the UI needs to render the V5 wave 5 surfaces with sample data.
 * ponytail: deliberately partial; unknown actions resolve as {} and are logged so gaps are easy to spot.
 */
type Json = Record<string, unknown>

const DAY = 86400000
const now = Date.now()
const iso = (offsetMs: number) => new Date(now - offsetMs).toISOString()
let revision = 1

const providers = [
  { id: 'claude-cli', name: 'Claude Code', shortName: 'Claude', outputMode: 'structured', supportsCli: true, supportsStructured: true, structuredProtocol: 'claude-stream-json' },
  { id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', outputMode: 'structured', supportsCli: true, supportsStructured: true, structuredProtocol: 'codex-app-server' },
  { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', outputMode: 'structured', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' },
  { id: 'opencode-cli', name: 'OpenCode', shortName: 'OpenCode', outputMode: 'structured', supportsCli: true, supportsStructured: true, structuredProtocol: 'acp' },
  { id: 'copilot-cli', name: 'GitHub Copilot CLI', shortName: 'Copilot', outputMode: 'structured', supportsCli: true, supportsStructured: true, structuredProtocol: 'acp' },
]

const models: Record<string, { id: string; name: string; description: string }[]> = {
  'claude-cli': [
    { id: 'claude-opus-5-5', name: 'Opus 5.5', description: 'Most capable for complex work' },
    { id: 'claude-sonnet-5-5', name: 'Sonnet 5.5', description: 'Balanced speed and capability' },
    { id: 'claude-haiku-5-5', name: 'Haiku 5.5', description: 'Fastest for simple tasks' },
  ],
  'codex-cli': [
    { id: 'gpt-6.1-sol', name: 'GPT-6.1 Sol', description: 'Default Codex model' },
    { id: 'gpt-6.1-mini', name: 'GPT-6.1 Mini', description: 'Cheaper and faster' },
  ],
  'gemini-cli': [
    { id: 'gemini-3-pro', name: 'Gemini 3 Pro', description: 'Long context' },
    { id: 'gemini-3-flash', name: 'Gemini 3 Flash', description: 'Fast responses' },
  ],
}

const folders = [
  { id: 'f-uam', title: 'Universal Agent Manager', directory: '/Users/david/GitHub/UAM', collapsed: false, executionHostId: 'local' },
  { id: 'f-site', title: 'Company website', directory: '/Users/david/GitHub/site', collapsed: false, executionHostId: 'local' },
]

type Msg = { role: 'user' | 'assistant'; content: string; createdAt: string; attachments?: Json[]; providerId?: string }
const messagesByChat: Record<string, Msg[]> = {}

function longChat(): Msg[] {
  const out: Msg[] = []
  for (let i = 1; i <= 24; i += 1) {
    out.push({ role: 'user', content: `Step ${i}: ${['tidy the sidebar spacing', 'check the settings nav', 'fix the rail notches', 'review the usage colours'][i % 4]}.`, createdAt: iso((30 - i) * 60000) })
    out.push({ role: 'assistant', providerId: 'claude-cli', content: `Done with step ${i}. I updated the relevant component and re-ran the focused tests; everything passes.\n\nNext I will look at the following item.`, createdAt: iso((30 - i) * 60000 - 20000) })
  }
  return out
}
messagesByChat['c-long'] = longChat()
messagesByChat['c-attach'] = [
  { role: 'user', content: 'Can you review these files and the screenshot?', createdAt: iso(3600000), attachments: [
    { id: 'a1', name: 'ChatView.tsx', kind: 'file', path: '/Users/david/GitHub/UAM/UI-V2/src/components/views/ChatView.tsx', size: 120000, mimeType: 'text/plain' },
    { id: 'a2', name: 'components', kind: 'directory', path: '/Users/david/GitHub/UAM/UI-V2/src/components' },
  ] },
  { role: 'assistant', providerId: 'codex-cli', content: 'I reviewed both. In `UI-V2/src/components/views/ChatView.tsx` the composer row needs its controls aligned to the right; the styles live in `UI-V2/src/index.css` and the docs in `docs/ui/`. I made the change on `feature/composer-row`, branched from `main`. Run `npm test` afterwards.', createdAt: iso(3500000) },
]
messagesByChat['c-stopped'] = [
  { role: 'user', content: 'Run the migration and report back.', createdAt: iso(7200000) },
  { role: 'assistant', providerId: 'gemini-cli', content: 'The migration finished. Waiting for your next instruction.', createdAt: iso(7100000) },
]
messagesByChat['c-side'] = [
  { role: 'user', content: 'Quick side question: what does the pill colour mean?', createdAt: iso(600000) },
  { role: 'assistant', providerId: 'claude-cli', content: 'It shows this is a temporary side chat; the main chat is reference only.', createdAt: iso(590000) },
]
messagesByChat['c-new'] = []

const chat = (id: string, title: string, folderId: string, providerId: string, extra: Json = {}) => ({
  id, title, folderId, providerId, modelId: models[providerId]?.[0]?.id, createdAt: iso(DAY), updatedAt: iso(60000),
  workspaceDirectory: folders.find(f => f.id === folderId)?.directory, executionHostId: 'local',
  messageCount: messagesByChat[id]?.length ?? 0, messagesDigest: `d-${id}-${messagesByChat[id]?.length ?? 0}`, ...extra,
})

let chats: Json[] = [
  chat('c-new', 'New chat', 'f-uam', 'claude-cli'),
  chat('c-long', 'Long review chat (message rail)', 'f-uam', 'claude-cli', { pinned: true, workspaceIsolationKind: 'gitWorktree', workspaceBranchName: 'feature/composer-row', workspaceWorktreeDirectory: '/Users/david/GitHub/UAM-worktrees/composer-row', workspaceBaseRef: 'main' }),
  chat('c-attach', 'Attachments and chips', 'f-uam', 'codex-cli'),
  chat('c-stopped', 'Stopped provider session', 'f-site', 'gemini-cli', { acpSession: { running: false, lifecycleState: 'stopped', lastStopReason: 'idle-timeout', idleShutdownAtMs: now - 900000 } }),
  chat('c-side', 'Side chat example', 'f-uam', 'claude-cli', { temporaryParentChatId: 'c-long' }),
]
let selectedChatId: string | null = 'c-new'
let computerUseEnabled = true
let deletionProgress: Json | undefined

const executionHosts = [
  { id: 'local', label: 'This Mac', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '5.0.0', platform: 'macos', architecture: 'arm64', lastSeenAt: iso(0) },
  { id: 'h-build', label: 'Build server', transport: 'ssh', sshAlias: 'build-01', runnerStatus: 'ready', runnerVersion: '5.0.0', platform: 'linux', architecture: 'x86_64', lastSeenAt: iso(30000), runnerDirectory: '/opt/uam-runner', startupEnabled: true, runnerStartupApplied: 'failed', runnerStartupError: 'systemctl enable returned exit code 1', instructionFile: '/home/david/AGENTS.md' },
  { id: 'h-gpu', label: 'GPU box', transport: 'ssh', sshAlias: 'gpu-02', runnerStatus: 'offline', runnerVersion: '4.9.2', platform: 'linux', architecture: 'x86_64', lastSeenAt: iso(DAY), startupEnabled: false, runnerStartupApplied: 'pending' },
]

function state(): Json {
  revision += 1
  return {
    stateRevision: revision, bootId: 'mock-boot', appVersion: '5.0.0-mock', folders, chats: chats.map(c => c.id === selectedChatId ? { ...c, messages: messagesByChat[c.id as string] ?? [] } : c),
    selectedChatId, providers,
    providerModelCatalogs: Object.entries(models).map(([providerId, list]) => ({ providerId, workspaceDirectory: '', executionHostId: 'local', availableModels: list, currentModelId: list[0].id, modelsLoading: false, modelRefreshError: '' })),
    settings: {
      activeProviderId: 'claude-cli', theme: localStorage.getItem('uam-theme') ?? 'dark', memoryEnabledDefault: true, memoryIdleDelaySeconds: 60, memoryRecallBudgetBytes: 8192,
      goalMaxLoopIterations: 10, updateChecksEnabled: true, automaticProviderUpdates: false, updateLastCheckedAt: iso(3600000), dismissedUpdateVersions: {},
      memoryLastStatus: '', memoryWorkerBindings: {}, gitEnabled: true, svnEnabled: false, executionHosts, computerUseEnabled, providerAgentImport: true, branchSwitching: true, hostScopedSettings: true,
      defaultNewChatProviderId: 'claude-cli',
    },
    cliVersionManager: { providers: [
      { providerId: 'claude-cli', installedVersion: '2.4.0', selectedVersion: '2.4.0', availableVersions: [{ version: '2.5.0', preferred: true }, { version: '2.4.0', preferred: false }], preferredVersion: '2.5.0', status: 'untested-newer', message: 'Version 2.5.0 is available.', running: false, lastCommand: '', lastOutput: '' },
    ] },
    createSessionFromSourceChat: true,
    ...(deletionProgress ? { deletionProgress } : {}),
  }
}

const push = () => window.uamPush?.({ type: 'stateUpdate', data: state() })

function totals(input: number, cacheRead: number, cacheWrite: number, output: number, reasoning: number, records: number) {
  const total = input + cacheRead + cacheWrite + output
  return { inputTokens: input, cachedInputTokens: cacheRead, cacheWriteTokens: cacheWrite, outputTokens: output, reasoningTokens: reasoning, totalTokens: total,
    reportedCostUsd: 0, estimatedCostUsd: Math.round(total * 0.000004 * 100) / 100, estimatedCacheSavingsUsd: 0, reportedCostTokens: 0, estimatedCostTokens: total, unpricedTokens: 0, records, sessions: Math.max(1, Math.floor(records / 4)) }
}
type T = ReturnType<typeof totals>
const add = (a: T, b: T): T => Object.fromEntries(Object.keys(a).map(k => [k, (a as Json)[k] as number + ((b as Json)[k] as number)])) as T
const USAGE_PROVIDERS = ['codex-cli', 'claude-cli', 'gemini-cli', 'opencode-cli', 'copilot-cli', 'antigravity-cli']

function usage(payload: Json): Json {
  const startMs = payload.startMs as number, endMs = payload.endMs as number, executionHostId = payload.executionHostId as string
  const zero = totals(0, 0, 0, 0, 0, 0)
  const providerDays: Json[] = [], days: Json[] = []
  const perProvider: Record<string, T> = Object.fromEntries(USAGE_PROVIDERS.map(p => [p, zero]))
  for (let day = Math.floor(startMs / DAY) * DAY; day < endMs; day += DAY) {
    let dayTotal = zero
    USAGE_PROVIDERS.slice(0, 3).forEach((p, i) => {
      const seed = ((day / DAY) * (i + 3)) % 7 + 1
      const t = totals(40000 * seed, 120000 * seed, 9000 * seed, 15000 * seed, 6000 * seed, 8 * seed)
      providerDays.push({ ...t, startMs: day, providerId: p })
      perProvider[p] = add(perProvider[p], t); dayTotal = add(dayTotal, t)
    })
    days.push({ ...dayTotal, startMs: day })
  }
  const providerRows = USAGE_PROVIDERS.map(p => ({ ...perProvider[p], providerId: p, available: true, files: 3, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 }))
  const all = Object.values(perProvider).reduce(add, zero)
  return { version: 2, executionHostId, source: 'native-transcripts', startMs, endMs, totals: all, complete: true, filesChecked: 9, filesTotal: 9, bytesRead: 123456, duplicates: 0, unidentifiedRecords: 0, listingLimited: false,
    providers: providerRows, models: USAGE_PROVIDERS.slice(0, 3).map(p => ({ ...perProvider[p], providerId: p, modelId: models[p][0].id, speed: 'standard' })), days, hours: [], providerDays, providerHours: [] }
}

function messagesResponse(chatId: string): Json {
  const list = messagesByChat[chatId] ?? []
  return { chatId, messages: list, startIndex: 0, totalCount: list.length, messagesDigest: `d-${chatId}-${list.length}`, userMessageIndices: list.flatMap((m, i) => m.role === 'user' ? [i] : []) }
}

function setChat(id: string, patch: Json) { chats = chats.map(c => c.id === id ? { ...c, ...patch } : c) }
// Branch switching: per-chat checked-out branch overrides and a shared fake branch list.
const chatBranchOverrides: Record<string, string> = {}
let fakeBranches: { name: string; remote?: boolean }[] = [{ name: 'main' }, { name: 'claude/v5ui-integration' }, { name: 'feature/composer' }, { name: 'build/fix-login' }, { name: 'release/2.0', remote: true }]
const chatCurrentBranch = (id: string) => chatBranchOverrides[id] ?? ((chats.find(x => x.id === id)?.workspaceBranchName as string) || (chats.find(x => x.id === id)?.folderId === 'f-site' ? 'main' : 'claude/v5ui-integration'))

async function handle(action: string, payload: Json): Promise<unknown> {
  switch (action) {
    case 'getInitialState': return state()
    case 'getChatMessages': return messagesResponse(payload.chatId as string)
    case 'selectSession': selectedChatId = payload.chatId as string; setTimeout(push, 0); return {}
    case 'getUsageHistory': return usage(payload)
    case 'setComputerUseEnabled': await sleep(500); computerUseEnabled = Boolean(payload.enabled); setTimeout(push, 0); return {}
    case 'listProviderAgents': await sleep(700); return { agents: [
      { id: 'reviewer', name: 'Code reviewer', description: 'Reviews diffs for bugs and style', sourcePath: `~/.config/${payload.providerId}/agents/reviewer.md` },
      { id: 'planner', name: 'Planner', description: 'Breaks work into steps', sourcePath: `~/.config/${payload.providerId}/agents/planner.md`, alreadyImported: true },
      { id: 'docs', name: 'Docs writer', sourcePath: `~/.config/${payload.providerId}/agents/docs.md` },
    ] }
    case 'importProviderAgents': await sleep(600); return { imported: (payload.agentIds as string[]).length - 1, failed: [{ id: (payload.agentIds as string[])[0], error: 'File is not valid Markdown front matter.' }] }
    case 'createSession': {
      const id = `c-${Date.now()}`
      messagesByChat[id] = []
      chats = [chat(id, 'New chat', (payload.folderId as string) || 'f-uam', (payload.providerId as string) || 'claude-cli'), ...chats]
      selectedChatId = id; setTimeout(push, 0); return { chatId: id }
    }
    case 'sendAcpPrompt': {
      const id = payload.chatId as string
      const list = messagesByChat[id] ??= []
      list.push({ role: 'user', content: payload.text as string, createdAt: new Date().toISOString() })
      setChat(id, { messageCount: list.length, messagesDigest: `d-${id}-${list.length}`, acpSession: { running: true, processing: true, lifecycleState: 'busy' } })
      setTimeout(push, 0)
      setTimeout(() => {
        list.push({ role: 'assistant', providerId: 'claude-cli', content: 'This is a mock reply. The composer should now sit at the bottom.', createdAt: new Date().toISOString() })
        setChat(id, { messageCount: list.length, messagesDigest: `d-${id}-${list.length}`, acpSession: { running: true, processing: false, lifecycleState: 'idle' } })
        push()
      }, 1200)
      return {}
    }
    case 'compactAcpSession': await sleep(300); throw Object.assign(new Error("Start this chat's provider session before compacting its context."), { code: 409 })
    case 'deleteSessions': case 'deleteFolder': case 'deleteFolders': {
      const ids: string[] = (payload.chatIds as string[]) ?? (payload.chatId ? [payload.chatId as string] : [])
      const total = 12
      for (let done = 0; done <= total; done += 1) {
        deletionProgress = { completed: done, total, currentLabel: `chat-${done + 1}.json` }
        push(); await sleep(250)
      }
      deletionProgress = undefined
      chats = chats.filter(c => !ids.includes(c.id as string))
      if (selectedChatId && ids.includes(selectedChatId)) selectedChatId = (chats[0]?.id as string) ?? null
      push(); return { selectedChatId }
    }
    case 'setChatPinned': setChat(payload.chatId as string, { pinned: payload.pinned }); setTimeout(push, 0); return {}
    case 'renameSession': setChat(payload.chatId as string, { title: payload.title }); setTimeout(push, 0); return {}
    case 'listMarkdownStoreEntries': return { entries: [] }
    case 'listThemes': return { themes: [] }
    case 'listUamAgents': return { agents: [] }
    case 'listChatBranches': {
      const current = chatCurrentBranch(payload.chatId as string)
      return { vcsType: 'git', branches: fakeBranches.map(b => ({ ...b, current: b.name === current })) }
    }
    case 'switchChatBranch': {
      const id = payload.chatId as string
      const name = (payload.branch as string).trim()
      if (payload.create) {
        if (fakeBranches.some(b => b.name === name)) throw Object.assign(new Error(`A branch named ${name} already exists.`), { code: 400 })
        fakeBranches = [...fakeBranches, { name }]
      } else if (name === 'release/2.0') {
        throw Object.assign(new Error('Commit or discard 2 changed files before switching branches.'), { code: 409 })
      }
      if (!fakeBranches.some(b => b.name === name)) throw Object.assign(new Error(`Unknown branch ${name}.`), { code: 404 })
      chatBranchOverrides[id] = name
      push()
      return { branch: name }
    }
    case 'getVcsCommitStatus': {
      const c = chats.find(x => x.id === payload.chatId)
      const branch = chatCurrentBranch(payload.chatId as string)
      return { available: true, vcsTypes: ['git'], activeVcsType: 'git', workspaceDirectory: (c?.workspaceDirectory as string) ?? '', branchOrRevision: branch, lineStatsReady: true, warning: '', error: '',
        changedFiles: [{ path: 'UI-V2/src/components/views/ChatView.tsx', status: 'M', additions: 42, deletions: 7, staged: false }] }
    }
    default:
      console.info('[fake CEF] unhandled action', action, payload)
      return {}
  }
}

const sleep = (ms: number) => new Promise(resolve => setTimeout(resolve, ms))

window.cefQuery = ({ request, onSuccess, onFailure }) => {
  const { action, payload } = JSON.parse(request) as { action: string; payload?: Json }
  handle(action, payload ?? {}).then(
    result => setTimeout(() => onSuccess(JSON.stringify(result ?? {})), 40),
    (error: Error & { code?: number }) => onFailure(error.code ?? 500, error.message),
  )
}
