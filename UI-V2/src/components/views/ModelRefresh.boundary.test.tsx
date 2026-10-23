// Picker interaction controls use private state and deferred transports.
import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ChatView } from './ChatView'
import { ModelPickerContent } from '../chat/ModelPickerContent'
import { useAppStore } from '../../store/useAppStore'
import { useModelPickerStore } from '../../store/modelPickerStore'
import { acpBindingFromCppChat, reconcileCppMessages } from '../../store/cpp/reconcile'
import { sanitizeCppAcpSession } from '../../store/cpp/sanitizers'
import { readChatComposerDraft, removeComposerDrafts, writeChatComposerDraft } from '../../utils/composerDraftStorage'
import type { ProviderModelCatalog, CppChat } from '../../store/cpp/types'
import type { Provider } from '../../types/provider'

vi.mock('../chat/ManagedSubagentPanel', () => ({ ManagedSubagentPanel: () => null }))
;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const ERROR = 'Could not refresh models. Try again.'
const TEXT = 'First draft line\nSecond draft line with  two spaces\nFinal line'
const FILE = { id: 'model-refresh-file', name: 'refresh-context.txt', type: 'file', size: 7, path: '/private-fixture/refresh-context.txt' }
const SKILL = { id: 'model-refresh-skill', title: 'Refresh retained skill', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', filePath: '/private-fixture/refresh-skill.md' }
const ID = 'model-refresh-displayed'
const OTHER = 'model-refresh-other'
const providers: Provider[] = ['codex-cli', 'claude-cli', 'gemini-cli'].map(id => ({
  id, name: id, shortName: id === 'codex-cli' ? 'Codex' : id === 'claude-cli' ? 'Claude' : 'Gemini', color: '', description: '',
  outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: id === 'gemini-cli' ? 'gemini-acp' : 'codex-app-server',
}))
const models = Array.from({ length: 3 }, (_, index) => ({ id: `model-${index}`, label: `Model ${index}`, shortLabel: `Model ${index}`, detail: '', disabled: index === 1 }))
const catalog = (host = 'ssh-lab', workspace = '/project', model = 'remote-only'): ProviderModelCatalog => ({
  providerId: 'claude-cli', executionHostId: host, workspaceDirectory: workspace,
  availableModels: [{ id: model, name: model, description: '', supportedReasoningEfforts: [] }],
  currentModelId: '', modelsLoading: false, modelRefreshError: '',
})
const HISTORY = [
  { role: 'user' as const, content: 'Earlier independent request', createdAt: '2026-01-01T00:00:00Z' },
  { role: 'assistant' as const, content: 'Earlier independent answer', createdAt: '2026-01-01T00:00:01Z' },
]

describe('post143 scoped model refresh boundary', () => {
  let previous: ReturnType<typeof useAppStore.getState>
  let previousPicker: ReturnType<typeof useModelPickerStore.getState>
  let previousCef: Window['cefQuery']
  let previousUrl: string
  let previousHistory: unknown
  let storageDescriptor: PropertyDescriptor | undefined
  let roots: Root[]
  let hosts: HTMLElement[]
  let pendingCleanup: Array<() => void>

  beforeEach(() => {
    previous = useAppStore.getState(); previousPicker = useModelPickerStore.getState()
    previousCef = window.cefQuery; previousUrl = window.location.href; previousHistory = window.history.state
    roots = []; hosts = []; pendingCleanup = []
    vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
    const saved = new Map<string, string>()
    const storage: Storage = { get length() { return saved.size }, clear: () => saved.clear(),
      getItem: key => saved.get(key) ?? null, setItem: (key, value) => { saved.set(key, String(value)) },
      removeItem: key => { saved.delete(key) }, key: index => [...saved.keys()][index] ?? null }
    storageDescriptor = Object.getOwnPropertyDescriptor(window, 'localStorage')
    vi.stubGlobal('localStorage', storage)
    Object.defineProperty(window, 'localStorage', { configurable: true, value: storage })
    delete window.cefQuery
    window.history.replaceState({}, '', '/')
    useAppStore.setState({ ...useAppStore.getInitialState(), providers, providerModelCatalogs: [], providerChatDefaults: {},
      discoverProviderModels: vi.fn().mockResolvedValue(true), refreshUamAgents: vi.fn(async () => true), getVcsCommitStatus: vi.fn(async () => null),
    }, true)
    useModelPickerStore.setState({ favorites: [] })
  })
  afterEach(async () => {
    await act(async () => pendingCleanup.forEach(finish => finish()))
    roots.forEach(root => act(() => root.unmount())); hosts.forEach(host => host.remove())
    removeComposerDrafts([ID, OTHER])
    useAppStore.setState(previous, true); useModelPickerStore.setState(previousPicker, true)
    window.cefQuery = previousCef; window.history.replaceState(previousHistory, '', previousUrl)
    vi.restoreAllMocks(); vi.unstubAllGlobals()
    if (storageDescriptor) Object.defineProperty(window, 'localStorage', storageDescriptor)
    else Reflect.deleteProperty(window, 'localStorage')
  })
  const deferred = () => {
    let resolve!: (value: boolean) => void; let reject!: (error: Error) => void
    const promise = new Promise<boolean>((yes, no) => { resolve = yes; reject = no })
    // Observe rejection on a side branch to isolate the feedback assertion from the test runner's global
    // unhandled-rejection channel; the consumer still receives the ORIGINAL rejecting Promise.
    void promise.catch(() => {})
    pendingCleanup.push(() => resolve(false))
    return { promise, resolve, reject }
  }
  const surface = () => {
    const host = document.createElement('div'); document.body.append(host); hosts.push(host)
    const root = createRoot(host); roots.push(root); return { host, root }
  }
  const button = (scope: ParentNode, label: string) => {
    const found = scope.querySelector<HTMLButtonElement>(`button[aria-label="${label}"]`)
    expect(found, `Required model refresh control is absent: ${label}`).not.toBeNull()
    return found!
  }
  const picker = (cached = true) => {
    const { host, root } = surface()
    const select = vi.fn(); const close = vi.fn()
    useAppStore.setState({ providerModelCatalogs: cached ? [catalog()] : [] })
    const render = (executionHostId = 'ssh-lab', workspaceDirectory = '/project', locked = false) => act(() => root.render(
      <ModelPickerContent providers={providers.slice(0, 2)} providerId="codex-cli" currentModelId="model-0" activeOptions={models}
        modelDisabled={locked} canChangeProvider={!locked} workspaceDirectory={workspaceDirectory} executionHostId={executionHostId}
        onSelect={select} onClose={close} />))
    render(); act(() => button(host, 'Claude').click())
    return { host, render, select, close }
  }
  const chat = (loading = false) => {
    const birth = new Date('2026-01-01T00:00:00Z')
    const displayed = { id: ID, name: 'Displayed pane', providerId: 'gemini-cli', modelId: 'auto-gemini-3',
      folderId: 'model-fixture-folder', viewMode: 'chat' as const, workspaceDirectory: '/private-fixture', executionHostId: 'local',
      createdAt: birth, updatedAt: birth, messageCount: 2, uamAgentId: 'plan', memoryLevel: 'balanced' as const, temporaryParentChatId: 'private-parent' }
    const other = { ...displayed, id: OTHER, name: 'Globally selected other pane', temporaryParentChatId: undefined }
    const cpp: CppChat = { id: ID, title: displayed.name, providerId: displayed.providerId, createdAt: birth.toISOString(), updatedAt: birth.toISOString(),
      acpSession: sanitizeCppAcpSession({ providerId: 'gemini-cli', protocolKind: 'gemini-acp', sessionId: 'private-runtime', running: true,
        processing: false, lifecycleState: 'ready', currentModeId: 'default', currentModelId: 'auto-gemini-3', modelsLoading: loading,
        modelRefreshError: '', availableModels: [{ id: 'auto-gemini-3', name: 'Gemini fixture', description: '', supportedReasoningEfforts: [] }],
        queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [], pendingPermission: null, pendingUserInput: null }) }
    const setProvider = vi.fn().mockResolvedValue(true); const setModel = vi.fn().mockResolvedValue(true); const send = vi.fn().mockResolvedValue(true)
    writeChatComposerDraft(ID, { text: '', attachments: [FILE] })
    writeChatComposerDraft(OTHER, { text: 'Other pane draft must stay separate', attachments: [] })
    useAppStore.setState({ sessions: [displayed, other], activeSessionId: OTHER,
      folders: [{ id: displayed.folderId, name: 'Fixture', directory: '/private-fixture', parentId: null, isExpanded: true, createdAt: birth }],
      messages: { [ID]: reconcileCppMessages(ID, undefined, HISTORY, true, 0), [OTHER]: [] },
      historyStartIndexBySessionId: { [ID]: 0 }, chatHistoryErrorBySessionId: {},
      acpBindingBySessionId: { [ID]: acpBindingFromCppChat(cpp, undefined) }, cliBindingBySessionId: {},
      markdownStoreAttachedBySessionId: { [ID]: [SKILL] }, uamAgentsBySessionId: { [ID]: [{ id: 'plan', description: 'Plan', builtIn: true }] },
      setSessionProvider: setProvider, setSessionModel: setModel, sendAcpPrompt: send,
    })
    const { host, root } = surface(); act(() => root.render(<ChatView session={displayed} />))
    const textarea = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
    expect(textarea).not.toBeNull()
    act(() => { Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(textarea, TEXT); textarea.dispatchEvent(new Event('input', { bubbles: true })) })
    act(() => window.dispatchEvent(new Event('pagehide')))
    const draft = readChatComposerDraft(ID)
    expect(draft).toEqual({ text: TEXT, attachments: [FILE] })
    act(() => button(host, 'Select provider and model').click())
    const menu = document.body.querySelector<HTMLElement>('.uam-model-picker')!
    expect(menu).not.toBeNull()
    const selected = menu.querySelector('[role="menuitemradio"][aria-checked="true"]')
    expect(selected).not.toBeNull()
    const retainedMessages = useAppStore.getState().messages
    const assertRetained = () => {
      expect(document.body.querySelector('.uam-model-picker')).toBe(menu)
      expect(menu.querySelector('[role="menuitemradio"][aria-checked="true"]')).toBe(selected)
      expect(host.querySelector('.uam-composer-textarea')).toBe(textarea); expect(textarea.value).toBe(TEXT)
      act(() => window.dispatchEvent(new Event('pagehide')))
      expect(readChatComposerDraft(ID)).toEqual(draft)
      expect(readChatComposerDraft(OTHER).text).toBe('Other pane draft must stay separate')
      expect(useAppStore.getState().activeSessionId).toBe(OTHER)
      expect(useAppStore.getState().sessions).toEqual([displayed, other])
      expect(useAppStore.getState().messages).toBe(retainedMessages)
      expect(useAppStore.getState().markdownStoreAttachedBySessionId[ID]).toEqual([SKILL])
      expect(host.textContent).toContain(FILE.name); expect(host.textContent).toContain(SKILL.title)
      expect(setProvider).not.toHaveBeenCalled(); expect(setModel).not.toHaveBeenCalled(); expect(send).not.toHaveBeenCalled()
      expect(window.cefQuery).toBeUndefined()
    }
    return { host, menu, textarea, selected, assertRetained }
  }

  it.each(['false', 'throw'] as const)('reports failed automatic scoped discovery and permits only deliberate retry (%s)', async outcome => {
    const first = deferred()
    const discover = vi.fn().mockReturnValueOnce(first.promise).mockResolvedValue(true)
    useAppStore.setState({ discoverProviderModels: discover })
    const f = picker(false)
    expect(discover).toHaveBeenCalledTimes(1)
    expect(discover).toHaveBeenCalledWith('', 'claude-cli', '/project', 'ssh-lab')
    await act(async () => outcome === 'throw' ? first.reject(new Error('Private discovery failure')) : first.resolve(false))
    expect(f.host.textContent).toContain(ERROR)
    act(() => button(f.host, 'Codex').click()); act(() => button(f.host, 'Claude').click())
    expect(discover).toHaveBeenCalledTimes(1)
    await act(async () => button(f.host, 'Retry models for Claude').click())
    expect(discover).toHaveBeenCalledTimes(2); expect(discover).toHaveBeenLastCalledWith('', 'claude-cli', '/project', 'ssh-lab')
    expect(f.select).not.toHaveBeenCalled(); expect(f.close).not.toHaveBeenCalled()
  })

  it('refreshes a populated catalogue once while preserving cached choices and query', async () => {
    const pending = deferred(); const discover = vi.fn(() => pending.promise)
    useAppStore.setState({ discoverProviderModels: discover }); const f = picker()
    const search = f.host.querySelector<HTMLInputElement>('[aria-label="Search models"]')!
    act(() => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(search, 'remote'); search.dispatchEvent(new Event('input', { bubbles: true })) })
    const refresh = button(f.host, 'Refresh models for Claude')
    act(() => { refresh.click(); refresh.click() })
    expect(discover).toHaveBeenCalledTimes(1); expect(refresh.disabled).toBe(true); expect(refresh.getAttribute('aria-busy')).toBe('true')
    expect(f.host.textContent).toContain('remote-only'); expect(search.value).toBe('remote')
    await act(async () => pending.resolve(true))
    expect(refresh.disabled).toBe(false); expect(f.host.textContent).toContain('remote-only'); expect(search.value).toBe('remote')
    expect(f.select).not.toHaveBeenCalled(); expect(f.close).not.toHaveBeenCalled()
  })

  it('uses the new exact host/workspace for deliberate discovery after a scope change', async () => {
    const discover = vi.fn().mockResolvedValue(true); useAppStore.setState({ discoverProviderModels: discover })
    const f = picker(); f.render('ssh-other', '/Other Project')
    await act(async () => button(f.host, 'Refresh models for Claude').click())
    expect(discover).toHaveBeenCalledTimes(1); expect(discover).toHaveBeenCalledWith('', 'claude-cli', '/Other Project', 'ssh-other')
    expect(f.host.textContent).not.toContain('remote-only'); expect(f.select).not.toHaveBeenCalled(); expect(f.close).not.toHaveBeenCalled()
  })

  it('refreshes the displayed active runtime rather than the globally selected chat', async () => {
    const pending = deferred(); const discover = vi.fn(() => pending.promise); useAppStore.setState({ discoverProviderModels: discover })
    const f = chat(); act(() => button(f.menu, 'Refresh models for Gemini').click())
    expect(discover).toHaveBeenCalledTimes(1); expect(discover).toHaveBeenCalledWith(ID)
    expect(button(f.menu, 'Refresh models for Gemini').disabled).toBe(true); f.assertRetained()
    await act(async () => pending.resolve(true)); f.assertRetained()
  })

  it('keeps active refresh disabled while its own runtime reports pushed discovery', () => {
    const discover = vi.fn().mockResolvedValue(true); useAppStore.setState({ discoverProviderModels: discover })
    const f = chat(true); const refresh = button(f.menu, 'Refresh models for Gemini')
    expect(refresh.disabled).toBe(true); expect(refresh.getAttribute('aria-busy')).toBe('true')
    act(() => refresh.click()); expect(discover).not.toHaveBeenCalled(); f.assertRetained()
  })

  it('preserves displayed draft and context through deferred false, thrown and successful active refresh', async () => {
    const pending: ReturnType<typeof deferred>[] = []
    const discover = vi.fn(() => { const next = deferred(); pending.push(next); return next.promise })
    useAppStore.setState({ discoverProviderModels: discover }); const f = chat()
    for (const [index, outcome] of (['false', 'throw', 'success'] as const).entries()) {
      act(() => button(f.menu, `${index === 0 ? 'Refresh' : 'Retry'} models for Gemini`).click())
      expect(discover).toHaveBeenCalledTimes(index + 1); expect(discover).toHaveBeenNthCalledWith(index + 1, ID)
      expect(button(f.menu, 'Refresh models for Gemini').disabled).toBe(true); f.assertRetained()
      await act(async () => outcome === 'throw' ? pending[index].reject(new Error('Private refresh failed')) : pending[index].resolve(outcome === 'success'))
      f.assertRetained()
      if (outcome === 'success') { expect(button(f.menu, 'Refresh models for Gemini').disabled).toBe(false); expect(f.menu.textContent).not.toContain(ERROR) }
      else { expect(button(f.menu, 'Retry models for Gemini').disabled).toBe(false); expect(f.menu.textContent).toContain(ERROR) }
    }
  })

  it('honours pushed nonactive loading without dropping cached choices or query', async () => {
    const discover = vi.fn().mockResolvedValue(true); useAppStore.setState({ discoverProviderModels: discover }); const f = picker()
    const search = f.host.querySelector<HTMLInputElement>('[aria-label="Search models"]')!
    act(() => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(search, 'remote'); search.dispatchEvent(new Event('input', { bubbles: true })) })
    act(() => useAppStore.setState({ providerModelCatalogs: [{ ...catalog(), modelsLoading: true }] }))
    const refresh = button(f.host, 'Refresh models for Claude')
    expect(refresh.disabled).toBe(true); expect(refresh.getAttribute('aria-busy')).toBe('true')
    expect(f.host.textContent).toContain('Discovering models'); expect(f.host.textContent).toContain('remote-only'); expect(search.value).toBe('remote')
    act(() => refresh.click()); expect(discover).not.toHaveBeenCalled()
    act(() => useAppStore.setState({ providerModelCatalogs: [catalog()] }))
    expect(refresh.disabled).toBe(false); await act(async () => refresh.click())
    expect(discover).toHaveBeenCalledTimes(1); expect(discover).toHaveBeenCalledWith('', 'claude-cli', '/project', 'ssh-lab')
    expect(f.select).not.toHaveBeenCalled(); expect(f.close).not.toHaveBeenCalled()
  })

  it('isolates a late old-scope failure from a different pending scope and retains that error on return', async () => {
    const old = deferred(); const current = deferred(); const discover = vi.fn().mockReturnValueOnce(old.promise).mockReturnValueOnce(current.promise)
    useAppStore.setState({ discoverProviderModels: discover }); const f = picker()
    act(() => button(f.host, 'Refresh models for Claude').click())
    expect(discover).toHaveBeenNthCalledWith(1, '', 'claude-cli', '/project', 'ssh-lab')
    act(() => useAppStore.setState({ providerModelCatalogs: [catalog(), catalog('ssh-other', '/Other Project', 'current-only')] }))
    f.render('ssh-other', '/Other Project'); const refresh = button(f.host, 'Refresh models for Claude')
    expect(refresh.disabled).toBe(false); expect(f.host.textContent).toContain('current-only'); expect(f.host.textContent).not.toContain('remote-only')
    act(() => refresh.click()); expect(discover).toHaveBeenNthCalledWith(2, '', 'claude-cli', '/Other Project', 'ssh-other')
    await act(async () => old.resolve(false))
    expect(refresh.disabled).toBe(true); expect(refresh.getAttribute('aria-busy')).toBe('true'); expect(f.host.textContent).not.toContain(ERROR)
    await act(async () => current.resolve(true)); expect(refresh.disabled).toBe(false)
    f.render(); expect(button(f.host, 'Retry models for Claude')).not.toBeNull(); expect(f.host.textContent).toContain(ERROR)
    expect(discover).toHaveBeenCalledTimes(2); expect(f.select).not.toHaveBeenCalled(); expect(f.close).not.toHaveBeenCalled()
  })

  it('keeps Favorites free of refresh and respects disabled model/provider gates', () => {
    const discover = vi.fn().mockResolvedValue(true); useAppStore.setState({ discoverProviderModels: discover })
    useModelPickerStore.setState({ favorites: [{ providerId: 'codex-cli', modelId: 'model-0' }, { providerId: 'claude-cli', modelId: 'remote-only' }] })
    const f = picker(); f.render('ssh-lab', '/project', true)
    const refresh = button(f.host, 'Refresh models for Claude'); const provider = button(f.host, 'Claude')
    expect(refresh.disabled).toBe(true); expect(provider.disabled).toBe(true)
    act(() => { refresh.click(); provider.click() }); expect(discover).not.toHaveBeenCalled()
    act(() => button(f.host, 'Favorites').click())
    expect(f.host.querySelector('[aria-label^="Refresh models for"],[aria-label^="Retry models for"]')).toBeNull()
    const choices = [...f.host.querySelectorAll<HTMLButtonElement>('[role="menuitemradio"]')]
    expect(choices).toHaveLength(2); expect(choices.every(choice => choice.disabled)).toBe(true)
    f.render(); expect(f.host.querySelector('[aria-label^="Refresh models for"],[aria-label^="Retry models for"]')).toBeNull()
    expect(f.host.textContent).toContain('remote-only'); expect(discover).not.toHaveBeenCalled()
    expect(f.select).not.toHaveBeenCalled(); expect(f.close).not.toHaveBeenCalled()
  })
  it.each(['false', 'throw'] as const)('isolates a mounted active owner at the same catalogue identity from late previous-owner completion (%s)', async outcome => {
    const ownerA = deferred(); const ownerB = deferred()
    const discover = vi.fn((chatId: string) => chatId === 'owner-A' ? ownerA.promise : ownerB.promise)
    useAppStore.setState({ discoverProviderModels: discover })
    // Stable per-owner callbacks represent the existing ChatView active-runtime callback seam. No new
    // component prop, native identity or delivery fact is invented; the picker instance is deliberately reused.
    const refreshA = () => discover('owner-A')
    const refreshB = () => discover('owner-B')
    const select = vi.fn(); const close = vi.fn(); const { host, root } = surface()
    const render = (owner: 'A' | 'B') => act(() => root.render(<ModelPickerContent providers={providers.slice(0, 2)}
      providerId="codex-cli" currentModelId="model-0" activeOptions={models}
      modelDisabled={false} canChangeProvider workspaceDirectory="/same-project" executionHostId="ssh-same"
      onRefreshActive={owner === 'A' ? refreshA : refreshB} onSelect={select} onClose={close} />))
    render('A')
    const pickerNode = host.querySelector('.uam-model-picker')!
    const selected = host.querySelector('[role="menuitemradio"][aria-checked="true"]')
    const refresh = button(host, 'Refresh models for Codex')
    act(() => refresh.click())
    expect(discover).toHaveBeenCalledTimes(1); expect(discover).toHaveBeenNthCalledWith(1, 'owner-A')
    expect(refresh.disabled).toBe(true)
    render('A') // Same owner rerender must not shed pending state.
    expect(button(host, 'Refresh models for Codex').disabled).toBe(true)
    render('B') // Only chat owner/callback changed: catalogue identity and mounted picker stay exact.
    expect(host.querySelector('.uam-model-picker')).toBe(pickerNode)
    expect(host.querySelector('[role="menuitemradio"][aria-checked="true"]')).toBe(selected)
    const currentRefresh = button(host, 'Refresh models for Codex')
    expect(currentRefresh).toBe(refresh)
    expect(currentRefresh.disabled).toBe(false)
    expect(host.textContent).not.toContain(ERROR)
    act(() => { currentRefresh.click(); currentRefresh.click() })
    expect(discover).toHaveBeenCalledTimes(2); expect(discover).toHaveBeenNthCalledWith(2, 'owner-B')
    expect(currentRefresh.disabled).toBe(true); expect(currentRefresh.getAttribute('aria-busy')).toBe('true')
    await act(async () => outcome === 'throw' ? ownerA.reject(new Error('Previous active owner failed')) : ownerA.resolve(false))
    expect(currentRefresh.disabled).toBe(true); expect(currentRefresh.getAttribute('aria-busy')).toBe('true')
    expect(host.textContent).not.toContain(ERROR)
    expect(host.querySelector('[aria-label="Retry models for Codex"]')).toBeNull()
    expect(host.querySelector('[role="menuitemradio"][aria-checked="true"]')).toBe(selected)
    await act(async () => ownerB.resolve(true))
    expect(currentRefresh.disabled).toBe(false); expect(host.textContent).not.toContain(ERROR)
    expect(discover).toHaveBeenCalledTimes(2)
    expect(select).not.toHaveBeenCalled(); expect(close).not.toHaveBeenCalled()
    expect(window.cefQuery).toBeUndefined()
  })

})
