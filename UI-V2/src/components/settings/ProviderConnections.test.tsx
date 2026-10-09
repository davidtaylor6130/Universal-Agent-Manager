import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { ProviderConnections } from './ProviderConnections'
import { useAppStore } from '../../store/useAppStore'
import { sendToCEF } from '../../ipc/cefBridge'
import { SUPPORTED_CLI_PROVIDER_IDS } from '../../utils/providerMetadata'

vi.mock('../../ipc/cefBridge', async importOriginal => ({ ...await importOriginal<typeof import('../../ipc/cefBridge')>(), sendToCEF: vi.fn() }));
(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
let host: HTMLDivElement
let root: Root
const server = { id: 'docs', name: 'Docs', executionHostId: '', workspaceDirectory: '/workspace', transport: 'http' as const, command: '', args: [], url: 'https://example.test/mcp', environment: [], headers: [], enabled: true }
const status = (hostId = 'local') => ({ executionHostId: hostId, connectedProofAvailable: false, handshakeProofAvailable: false, computerUseBuiltInAvailable: hostId === 'local', providers: SUPPORTED_CLI_PROVIDER_IDS.map(providerId => ({ providerId, status: 'configured-unverified', detail: '', activeSessionCount: 0, configuredSessionCount: 0, uamControlEnabled: true, sharedSetupEnabled: true, mcpServers: [{ id: 'docs', name: 'Docs', enabled: true }] })) })
beforeEach(() => {
  vi.mocked(sendToCEF).mockReset().mockImplementation(async request => ({ ok: true, data: status(request.payload?.executionHostId as string) }))
  useAppStore.setState(state => ({ sessions: [], mcpServers: [server], providers: [], executionHosts: [{ id: 'local', label: 'Local', transport: 'local' }, { id: 'ssh-lab', label: 'Lab', transport: 'ssh', sshAlias: 'lab' }], centralProviderConfiguration: { ...state.centralProviderConfiguration, enabled: true, uamControlEnabled: true }, cliVersionManager: { providers: [], remoteProviders: [] } }))
  host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host)
})
afterEach(() => { act(() => root.unmount()); host.remove(); vi.useRealTimers() })
async function render(providerId?: string) { await act(async () => root.render(<ProviderConnections providerId={providerId} onMcpSelect={vi.fn()} onPreferencesSelect={vi.fn()} />)) }
async function click(label: string) { await act(async () => host.querySelector<HTMLButtonElement>(`[aria-label="${label}"]`)!.click()) }

it('renders six compact provider rows with all server columns and no guessed connection success', async () => {
  await render()
  expect(host.querySelectorAll('tbody tr')).toHaveLength(6)
  expect(host.textContent).toContain('Docs')
  expect(host.textContent).not.toContain('0/0')
  expect(host.textContent).not.toContain('enabled server definitions')
  expect(host.textContent).not.toContain('Connected')
  expect(host.querySelector('[role="checkbox"][aria-label="Codex Docs"]')?.getAttribute('aria-checked')).toBe('true')
  expect(host.querySelector('[role="tablist"][aria-label="Provider settings host"]')).not.toBeNull()
})

it('shows installations and Computer Use availability for the selected host', async () => {
  useAppStore.setState({ cliVersionManager: { providers: [], remoteProviders: [{ providerId: 'codex-cli', executionHostId: 'ssh-lab', installedVersion: '9.1', installMethod: 'npm', selectedVersion: '', availableVersions: [], preferredVersion: '', status: 'verified', message: '', running: false, lastCommand: '', lastOutput: '' }] } })
  await render('codex-cli')
  expect(host.textContent).not.toContain('9.1')
  await click('Lab')
  expect(host.textContent).toContain('9.1')
  expect(host.textContent).not.toContain('npm')
  expect(host.querySelector('[aria-label="Codex Computer Use"]')?.getAttribute('aria-checked')).toBe('false')
})

it('updates a provider server override only after successful verified backend reply', async () => {
  await render('codex-cli')
  let finish!: (value: unknown) => void
  vi.mocked(sendToCEF).mockImplementationOnce(() => new Promise(resolve => { finish = resolve as typeof finish }))
  await click('Codex Docs')
  expect(sendToCEF).toHaveBeenLastCalledWith({ action: 'setProviderMcpConfiguration', payload: { executionHostId: 'local', providerId: 'codex-cli', serverId: 'docs', enabled: false } })
  expect(host.querySelector('[aria-label="Codex Docs"]')?.getAttribute('aria-checked')).toBe('true')
  const reply = status(); reply.providers.find(row => row.providerId === 'codex-cli')!.mcpServers[0].enabled = false
  await act(async () => finish({ ok: true, data: reply }))
  expect(host.querySelector('[aria-label="Codex Docs"]')?.getAttribute('aria-checked')).toBe('false')
})

it('keeps failed changes visible and reports their error beside controls', async () => {
  await render('codex-cli')
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: false, error: 'Cannot save provider MCP configuration.' })
  await click('Codex UAM MCP')
  expect(host.querySelector('[aria-label="Codex UAM MCP"]')?.getAttribute('aria-checked')).toBe('true')
  expect(host.querySelector('[role="alert"]')?.textContent).toContain('Cannot save provider MCP configuration.')
})

it('refreshes manually, after configuration changes and on a modest mounted interval', async () => {
  vi.useFakeTimers()
  await render('codex-cli')
  const initial = vi.mocked(sendToCEF).mock.calls.length
  await act(async () => useAppStore.setState(state => ({ sessions: [...state.sessions] })))
  expect(vi.mocked(sendToCEF).mock.calls.length).toBe(initial)
  await click('Refresh provider connections')
  expect(vi.mocked(sendToCEF).mock.calls.length).toBe(initial + 1)
  await act(async () => useAppStore.setState({ mcpServers: [{ ...server, enabled: false }] }))
  expect(vi.mocked(sendToCEF).mock.calls.length).toBe(initial + 2)
  await act(async () => vi.advanceTimersByTime(30_000))
  expect(vi.mocked(sendToCEF).mock.calls.length).toBe(initial + 3)
})

it('rejects stale host replies and wrong-host mutation replies', async () => {
  let finish!: (value: unknown) => void
  vi.mocked(sendToCEF).mockImplementationOnce(() => new Promise(resolve => { finish = resolve as typeof finish }))
  await render('codex-cli'); await click('Lab')
  const old = status(); old.providers.find(row => row.providerId === 'codex-cli')!.uamControlEnabled = false
  await act(async () => finish({ ok: true, data: old }))
  expect(host.querySelector('[aria-label="Codex UAM MCP"]')?.getAttribute('aria-checked')).toBe('true')
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: old })
  await click('Codex UAM MCP')
  expect(host.querySelector('[aria-label="Codex UAM MCP"]')?.getAttribute('aria-checked')).toBe('true')
  expect(host.querySelector('[role="alert"]')).not.toBeNull()
})

it('treats runtime structured support as authoritative for Antigravity and never chat permission as build availability', async () => {
  useAppStore.setState({ providers: [{ id: 'antigravity-cli', supportsStructured: true }] as ReturnType<typeof useAppStore.getState>['providers'] })
  await render('antigravity-cli')
  expect(host.querySelector('[aria-label="Antigravity UAM MCP"]')).not.toHaveProperty('disabled', true)
  expect(host.querySelector('[aria-label="Antigravity Computer Use"]')?.getAttribute('aria-checked')).toBe('true')
  expect(host.querySelector('[aria-label="Antigravity Computer Use"]')).toHaveProperty('disabled', true)
})

it('requires authoritative live evidence before displaying Connected', async () => {
  const historical = status(); historical.handshakeProofAvailable = true; historical.providers[0].status = 'handshake-confirmed'
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: historical })
  await render('gemini-cli')
  expect(host.textContent).not.toContain('Connected')
  const live = status(); live.connectedProofAvailable = true; live.providers[0].status = 'connected'
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: live })
  await click('Refresh provider connections')
  expect(host.textContent).toContain('Connected')
})

it('retains the global UAM warning and preserves instructions during its repair', async () => {
  const setter = vi.fn(async () => ({ ok: false, error: 'Cannot write provider configuration.' }))
  useAppStore.setState(state => ({ centralProviderConfiguration: { ...state.centralProviderConfiguration, uamControlEnabled: false, instructions: 'Keep instructions' }, setCentralProviderConfiguration: setter }))
  await render()
  expect(host.querySelector('[role="alert"]')?.textContent).toContain('WARNING: UAM MCP is disabled')
  await act(async () => [...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === 'Enable UAM MCP')!.click())
  expect(setter).toHaveBeenCalledWith(expect.objectContaining({ uamControlEnabled: true, instructions: 'Keep instructions' }))
  expect(host.textContent).toContain('Cannot write provider configuration.')
})


it('clears prior live connection evidence when refresh fails', async () => {
  const live = status(); live.connectedProofAvailable = true; live.providers[0].status = 'connected'
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: live })
  await render('gemini-cli')
  expect(host.textContent).toContain('Connected')
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: false, error: 'Host check failed.' })
  await click('Refresh provider connections')
  expect(host.textContent).not.toContain('Connected')
  expect(host.querySelector('[role="status"]')?.textContent).toContain('Host check failed.')
  expect(host.querySelector('[aria-label="Gemini UAM MCP (unknown)"]')).toHaveProperty('disabled', true)
})

it.each(['loading', 'failed lookup'])('shows unknown controls instead of disabled red crosses during %s', async phase => {
  vi.mocked(sendToCEF).mockImplementationOnce(() => phase === 'loading'
    ? new Promise(() => {})
    : Promise.resolve({ ok: false, error: 'Status unavailable' }))
  await render('codex-cli')
  for (const label of ['UAM MCP', 'Computer Use', 'Shared setup', 'Docs']) {
    const control = host.querySelector<HTMLButtonElement>(`[aria-label="Codex ${label} (unknown)"]`)
    expect(control).not.toBeNull()
    expect(control?.getAttribute('aria-checked')).toBe('mixed')
    expect(control?.style.color).toBe('var(--text-3)')
    expect(control?.disabled).toBe(true)
    expect(control?.querySelector('.lucide-x')).toBeNull()
  }
})

it('retains disabled red controls when their false value is verified', async () => {
  const disabled = status()
  disabled.providers.find(row => row.providerId === 'codex-cli')!.uamControlEnabled = false
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: disabled })
  await render('codex-cli')
  const control = host.querySelector<HTMLButtonElement>('[aria-label="Codex UAM MCP"]')!
  expect(control.getAttribute('aria-checked')).toBe('false')
  expect(control.querySelector('.lucide-x')).not.toBeNull()
  expect(control.disabled).toBe(false)
})

it('ignores structurally identical pushes and unrelated settings while retaining action errors', async () => {
  useAppStore.setState({ providers: [{ id: 'codex-cli', supportsStructured: true }] as ReturnType<typeof useAppStore.getState>['providers'] })
  await render('codex-cli')
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: false, error: 'Save failed, retry this change.' })
  await click('Codex UAM MCP')
  const requests = vi.mocked(sendToCEF).mock.calls.length
  await act(async () => useAppStore.setState(state => ({
    centralProviderConfiguration: { ...state.centralProviderConfiguration, instructions: 'Updated unrelated instructions',
      mcpOverrides: state.centralProviderConfiguration.mcpOverrides?.map(override => ({ ...override })) },
    mcpServers: state.mcpServers.map(server => ({ ...server, environment: [...server.environment], headers: [...server.headers] })),
    providers: state.providers.map(provider => ({ ...provider })),
  })))
  expect(vi.mocked(sendToCEF).mock.calls.length).toBe(requests)
  expect(host.querySelector('[role="alert"]')?.textContent).toContain('Save failed, retry this change.')
  await click('Refresh provider connections')
  expect(host.querySelector('[role="alert"]')?.textContent).toContain('Save failed, retry this change.')
})
