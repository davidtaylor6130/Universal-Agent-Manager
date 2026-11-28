import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import type { CppAppState } from './cpp/types'
import type { AppState } from './storeTypes'
import type { ExecutionRoutingSettings } from '../types/executionRouting'

// Builds without SSH: routing state and actions must not exist anywhere in the store, and automatic routing stays unavailable.
vi.mock('../config/buildFeatures', async (importOriginal) => ({ ...await importOriginal<typeof import('../config/buildFeatures')>(), SSH_ENABLED: false }))

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

type TestWindow = Window & typeof globalThis & { cefQuery?: Window['cefQuery']; uamPush?: (payload: unknown) => void }

const routing: ExecutionRoutingSettings = {
  enabled: true,
  hosts: [{ hostId: 'ssh-a', enabled: true, maxActiveTurns: 2, providers: [{ providerId: 'codex-cli', connectionIdentity: 'host-v1', providerConfigurationIdentity: 'provider-v1' }], workspaceMappings: [{ sourceFolderId: 'source', sourceDirectory: '/source', destinationFolderId: 'target' }] }],
}
const chat = (id: string, title: string) => ({ id, title, folderId: '', providerId: 'codex-cli', createdAt: '2026-01-01T00:00:00Z', updatedAt: '2026-01-01T00:00:00Z' })
// Native in a build without SSH no longer sends routing, but an older or fuller payload must still be ignored.
const fullState = (revision: number, title: string): CppAppState => ({
  stateRevision: revision, selectedChatId: 'chat-1', folders: [], resourceCollections: [], providers: [],
  chats: [chat('chat-1', title)], settings: { executionRouting: routing }, statusLine: '',
}) as unknown as CppAppState

const ROUTING_MEMBERS = ['executionRouting', 'setExecutionRoutingSettings', 'releaseExecutionRoutingReservation']
const expectNoRouting = (state: object) => { for (const key of ROUTING_MEMBERS) expect(state).not.toHaveProperty(key) }

async function startStore() {
  const win = window as TestWindow
  vi.resetModules()
  vi.spyOn(window, 'dispatchEvent').mockImplementation(() => true)
  win.cefQuery = ({ request, onSuccess }) => {
    if (JSON.parse(request).action === 'getInitialState') onSuccess(JSON.stringify(fullState(5, 'Initial')))
    else onSuccess('{}')
  }
  const { useAppStore } = await import('./useAppStore')
  await vi.waitFor(() => expect(useAppStore.getState().sessions[0]?.name).toBe('Initial'))
  return { store: useAppStore, push: (payload: unknown) => win.uamPush?.(payload) }
}

describe('builds without SSH', () => {
  beforeEach(() => { vi.spyOn(console, 'error').mockImplementation(() => undefined); vi.spyOn(console, 'warn').mockImplementation(() => undefined) })
  afterEach(() => { delete (window as TestWindow).cefQuery; vi.restoreAllMocks() })

  it('keeps routing out of the store through the initial state, patches and full-state snapshots', async () => {
    const { store, push } = await startStore()
    expect(store.getState().sessions[0]?.name).toBe('Initial')
    expectNoRouting(store.getState())
    push({ type: 'statePatch', data: { stateRevision: 6, settings: { executionRouting: routing }, chats: [chat('chat-1', 'Patched')] } })
    expect(store.getState().sessions[0]?.name).toBe('Patched')
    expectNoRouting(store.getState())
    // A full state is projected onto a snapshot of the current settings, which must not reintroduce routing either.
    push({ type: 'stateUpdate', data: fullState(7, 'Replaced') })
    expect(store.getState().sessions[0]?.name).toBe('Replaced')
    expectNoRouting(store.getState())
  })

  it('refuses automatic routing in session creation without asking native', async () => {
    vi.resetModules()
    const bridge = await import('../ipc/cefBridge')
    const send = vi.spyOn(bridge, 'sendToCEF').mockResolvedValue({ ok: true })
    vi.spyOn(bridge, 'isCefContext').mockReturnValue(true)
    const { createSessionsSlice } = await import('./slices/sessionsSlice')
    // Even a stray routing value that would allow automatic routing in an SSH build is ignored.
    const state = {
      executionRouting: routing, executionHosts: [], sessions: [], providerChatDefaults: {}, defaultNewChatProviderId: 'codex-cli',
      folders: [{ id: 'source', directory: '/source' }], providers: [{ id: 'codex-cli', name: 'Codex', shortName: 'Codex', color: '', description: '' }],
    } as unknown as AppState
    const actions = createSessionsSlice(vi.fn(), () => state, true)
    await expect(actions.addSession('Automatic', 'source', 'codex-cli', '', '', 'chat', 'local', '/source', 'automatic')).resolves.toBe(false)
    expect(send).not.toHaveBeenCalledWith(expect.objectContaining({ action: 'createSession' }))
  })

  it('renders the new-chat dialog without an automatic routing choice', async () => {
    vi.resetModules()
    const { useAppStore } = await import('./useAppStore')
    const { NewChatModal } = await import('../components/sidebar/NewChatModal')
    expectNoRouting(useAppStore.getState())
    useAppStore.setState({
      folders: [{ id: 'project', name: 'Project', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: new Date() }],
      providers: [{ id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', color: '#22c55e', description: '' }],
      defaultNewChatProviderId: 'codex-cli', newChatFolderId: 'project', newChatWorktreeContext: null,
      executionHosts: [{ id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' }],
    })
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    try {
      act(() => root.render(<NewChatModal />))
      expect(document.body.querySelector('button[aria-label="This computer"]')).not.toBeNull()
      expect(document.body.querySelector('button[aria-label="Automatic"]')).toBeNull()
      expect(document.body.textContent).not.toMatch(/routing/i)
    } finally { act(() => root.unmount()); host.remove() }
  })
})
