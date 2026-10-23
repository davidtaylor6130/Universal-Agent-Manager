import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ModelPickerContent } from './ModelPickerContent'
import { buildModelOptions } from './modelOptions'
import { useAppStore } from '../../store/useAppStore'
import { useModelPickerStore } from '../../store/modelPickerStore'
import type { ProviderModelCatalog } from '../../store/cpp/types'
import type { Provider } from '../../types/provider'

// Private public-boundary fixture: real picker, real public store action and real
// loadFromCef projection. Only the CEF transport is deferred. No discovery action,
// native ACK, successful process, selected model, provider start or Send is mocked.
const ID = 'private-public-model-refresh'
const WORKSPACE = '/private/model-refresh-fixture'
const BIRTH = '2026-10-22T01:00:00.000Z'
const SPECIFIC = 'Provider model catalog is unavailable.'
const GENERIC = 'Could not refresh models. Try again.'
const providers: Provider[] = ['codex-cli', 'claude-cli'].map(id => ({
  id, name: id, shortName: id === 'codex-cli' ? 'Codex' : 'Claude', color: '', description: '',
  supportsStructured: true, outputMode: 'cli',
}))
type Query = {
  envelope: { action: string; payload?: Record<string, unknown> }
  settled: boolean
  fail: () => void
  close: () => void
}
const refreshOwningChat = () => useAppStore.getState().discoverProviderModels(ID)
let queries: Query[] = []
let root: Root
let host: HTMLDivElement
let previousStore: ReturnType<typeof useAppStore.getState>
let previousPicker: ReturnType<typeof useModelPickerStore.getState>
let previousCef: PropertyDescriptor | undefined
let previousActFlag: PropertyDescriptor | undefined
let previousUrl: string
let previousHistory: unknown
let select: ReturnType<typeof vi.fn>
let close: ReturnType<typeof vi.fn>

function model(id: string) { return { id, name: id, description: '', supportedReasoningEfforts: [] } }
function catalogue(loading = false, fresh = false, hostId = 'local'): ProviderModelCatalog {
  return {
    providerId: 'claude-cli', executionHostId: hostId, workspaceDirectory: WORKSPACE,
    availableModels: [model('cached-claude-model'), ...(fresh ? [model('new-claude-model')] : [])],
    configOptions: [], currentModelId: '', modelsLoading: loading, modelRefreshError: '',
  }
}

function project(revision: number, activeLoading = false, activeFresh = false, catalogs: ProviderModelCatalog[] = [catalogue()]) {
  // These are source-shaped CEF full-state snapshots consumed by the actual
  // sanitizer/deserializer, not direct assignment to a binding or picker prop.
  // Success oracle requires observed matching loading -> settled + changed model
  // contents + cleared error. No catalogue timestamp/token is invented.
  useAppStore.getState().loadFromCef({
    stateRevision: revision,
    folders: [{ id: 'private-model-folder', title: 'Fixture', directory: WORKSPACE, collapsed: false }],
    resourceCollections: [], selectedChatId: ID, providers,
    chats: [{
      id: ID, title: 'Private model owner', folderId: 'private-model-folder', providerId: 'codex-cli',
      executionHostId: 'local', workspaceDirectory: WORKSPACE, modelId: 'selected-model',
      createdAt: BIRTH, updatedAt: BIRTH, messages: [],
      acpSession: {
        sessionId: 'private-cached-native', threadId: 'private-cached-native', providerId: 'codex-cli',
        protocolKind: 'codex-app-server', running: false, processing: false, lifecycleState: 'stopped',
        currentModelId: 'selected-model', modelsLoading: activeLoading, modelRefreshError: '',
        availableModels: [model('selected-model'), ...(activeFresh ? [model('new-active-model')] : [])],
        queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [],
        pendingPermission: null, pendingUserInput: null,
      },
    }],
    providerModelCatalogs: catalogs,
    settings: {
      activeProviderId: 'codex-cli', theme: 'dark', memoryEnabledDefault: true,
      memoryIdleDelaySeconds: 60, memoryRecallBudgetBytes: 2048, goalMaxLoopIterations: 200,
      updateChecksEnabled: false, automaticProviderUpdates: false, updateLastCheckedAt: '',
      dismissedUpdateVersions: {}, memoryLastStatus: '', memoryWorkerBindings: {},
    },
    memoryActivity: { entryCount: 0, lastCreatedAt: '', lastCreatedCount: 0, runningCount: 0, lastStatus: '' },
    statusLine: '',
  })
  expect(useAppStore.getState().lastAppliedStateRevision).toBe(revision)
}

function RealStorePicker({ active = true }: { active?: boolean }) {
  const binding = useAppStore(state => state.acpBindingBySessionId[ID])
  return <ModelPickerContent providers={providers} providerId="codex-cli" currentModelId="selected-model"
    activeOptions={buildModelOptions(binding, 'selected-model', providers[0], 'codex-cli', true)}
    activeLoading={binding?.modelsLoading} activeError={binding?.modelRefreshError}
    modelDisabled={false} canChangeProvider workspaceDirectory={WORKSPACE} executionHostId="local"
    onRefreshActive={active ? refreshOwningChat : undefined} onSelect={select} onClose={close} />
}

beforeEach(() => {
  previousStore = useAppStore.getState(); previousPicker = useModelPickerStore.getState()
  previousCef = Object.getOwnPropertyDescriptor(window, 'cefQuery')
  previousActFlag = Object.getOwnPropertyDescriptor(globalThis, 'IS_REACT_ACT_ENVIRONMENT')
  previousUrl = window.location.href; previousHistory = window.history.state
  window.history.replaceState({}, '', '/')
  Object.defineProperty(globalThis, 'IS_REACT_ACT_ENVIRONMENT', { configurable: true, writable: true, value: true })
  queries = []; select = vi.fn(); close = vi.fn()
  useAppStore.setState(useAppStore.getInitialState(), true)
  useModelPickerStore.setState({ favorites: [] })
  vi.spyOn(console, 'error') // Retain the actual bridge diagnostic; do not suppress it.
  Object.defineProperty(window, 'cefQuery', { configurable: true, writable: true, value: ((parameters) => {
    const query: Query = {
      envelope: JSON.parse(parameters.request), settled: false,
      fail: () => {
        expect(query.settled).toBe(false); query.settled = true
        parameters.onFailure(500, SPECIFIC)
      },
      close: () => {
        if (query.settled) return
        query.settled = true; parameters.onSuccess(JSON.stringify({ started: false, pending: false }))
      },
    }
    queries.push(query)
  }) satisfies NonNullable<Window['cefQuery']> })
  project(1)
  expect(useAppStore.getState().discoverProviderModels).toBe(useAppStore.getInitialState().discoverProviderModels)
  host = document.createElement('div'); document.body.append(host); root = createRoot(host)
})

afterEach(async () => {
  await act(async () => { queries.forEach(query => query.close()); await Promise.resolve() })
  if (root) act(() => root.unmount())
  host?.remove()
  useAppStore.setState(previousStore, true); useModelPickerStore.setState(previousPicker, true)
  if (previousCef) Object.defineProperty(window, 'cefQuery', previousCef)
  else delete window.cefQuery
  window.history.replaceState(previousHistory, '', previousUrl)
  if (previousActFlag) Object.defineProperty(globalThis, 'IS_REACT_ACT_ENVIRONMENT', previousActFlag)
  else Reflect.deleteProperty(globalThis, 'IS_REACT_ACT_ENVIRONMENT')
  vi.restoreAllMocks()
})

function button(label: string) {
  const found = host.querySelector<HTMLButtonElement>(`button[aria-label="${label}"]`)
  expect(found, `Missing picker control ${label}`).not.toBeNull()
  return found!
}
function expectOnlyDiscovery(expectedChatId: string) {
  expect(queries).toHaveLength(1)
  expect(queries[0].envelope.action).toBe('discoverProviderModels')
  expect(queries[0].envelope.payload?.chatId).toBe(expectedChatId)
  expect(queries.every(query => query.settled)).toBe(true)
  expect(select).not.toHaveBeenCalled(); expect(close).not.toHaveBeenCalled()
  expect(useAppStore.getState().sessions.find(session => session.id === ID)?.modelId).toBe('selected-model')
}
async function failActiveRequest() {
  act(() => root.render(<RealStorePicker />))
  const mounted = host.querySelector('.uam-model-picker')
  act(() => button('Refresh models for Codex').click())
  expect(queries).toHaveLength(1)
  expect(queries[0].envelope.payload).toEqual({ chatId: ID })
  await act(async () => queries[0].fail())
  expect(useAppStore.getState().acpBindingBySessionId[ID].modelRefreshError).toBe(SPECIFIC)
  expect(button('Retry models for Codex')).not.toBeNull()
  return mounted
}

describe('model picker feedback through the real public store boundary', () => {
  it('shows the owning native response error instead of masking it with the local generic failure', async () => {
    const mounted = await failActiveRequest()
    expect(host.textContent).toContain(SPECIFIC)
    expect(host.textContent).not.toContain(GENERIC)
    expect(host.querySelector('.uam-model-picker')).toBe(mounted)
    expectOnlyDiscovery(ID)
  })

  it('retires active Retry only after matching projected loading and a completed changed-model result without remount', async () => {
    const mounted = await failActiveRequest()
    act(() => project(2, true, false))
    expect(host.textContent).toContain('Discovering models')
    expect(useAppStore.getState().acpBindingBySessionId[ID].modelsLoading).toBe(true)
    act(() => project(3, false, true))
    expect(useAppStore.getState().acpBindingBySessionId[ID]).toMatchObject({ modelsLoading: false, modelRefreshError: '' })
    expect(host.textContent).toContain('new-active-model')
    expect(host.textContent).not.toContain(GENERIC)
    expect(host.textContent).not.toContain(SPECIFIC)
    expect(host.querySelector('[aria-label="Retry models for Codex"]')).toBeNull()
    expect(button('Refresh models for Codex')).not.toBeNull()
    expect(host.querySelector('.uam-model-picker')).toBe(mounted)
    expectOnlyDiscovery(ID)
  })

  it('keeps an inactive error through unrelated scope success and retires it only after matching catalogue progress and changed models', async () => {
    act(() => root.render(<RealStorePicker active={false} />))
    const mounted = host.querySelector('.uam-model-picker')
    act(() => button('Claude').click())
    expect(queries).toHaveLength(0) // A populated cached catalogue must not auto-refresh on browse.
    act(() => button('Refresh models for Claude').click())
    expect(queries).toHaveLength(1)
    expect(queries[0].envelope.payload).toEqual({ chatId: '', providerId: 'claude-cli', workspaceDirectory: WORKSPACE, executionHostId: 'local' })
    await act(async () => queries[0].fail())
    expect(host.textContent).toContain(GENERIC)
    act(() => project(2, false, false, [catalogue(), catalogue(true, false, 'unrelated-host')]))
    act(() => project(3, false, false, [catalogue(), catalogue(false, true, 'unrelated-host')]))
    expect(host.textContent).toContain(GENERIC)
    expect(button('Retry models for Claude')).not.toBeNull()
    expect(host.textContent).not.toContain('new-claude-model')
    act(() => project(4, false, false, [catalogue(true, false)]))
    expect(host.textContent).toContain('Discovering models')
    act(() => project(5, false, false, [catalogue(false, true)]))
    expect(host.textContent).toContain('new-claude-model')
    expect(host.textContent).not.toContain(GENERIC)
    expect(host.querySelector('[aria-label="Retry models for Claude"]')).toBeNull()
    expect(button('Refresh models for Claude')).not.toBeNull()
    expect(host.querySelector('.uam-model-picker')).toBe(mounted)
    expectOnlyDiscovery('')
  })
})


it('keeps local failure when a native error clears without an observed matching refresh', async () => {
  const mounted = await failActiveRequest()
  act(() => project(2, false, false))
  expect(useAppStore.getState().acpBindingBySessionId[ID]).toMatchObject({ modelsLoading: false, modelRefreshError: '' })
  expect(host.textContent).toContain(GENERIC)
  expect(button('Retry models for Codex')).not.toBeNull()
  expect(host.textContent).not.toContain('new-active-model')
  expect(host.querySelector('.uam-model-picker')).toBe(mounted)
  expectOnlyDiscovery(ID)
})
