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
const status = (hostId = 'local') => ({ executionHostId: hostId, connectedProofAvailable: false, handshakeProofAvailable: false, computerUseBuiltInAvailable: hostId === 'local', providers: SUPPORTED_CLI_PROVIDER_IDS.map(providerId => ({ providerId, status: 'configured-unverified', detail: '', activeSessionCount: 0, configuredSessionCount: 0, uamControlEnabled: true, sharedSetupEnabled: true, progressiveMcpDiscoveryEnabled: providerId === 'opencode-cli' ? true : undefined, mcpServers: [{ id: 'docs', name: 'Docs', enabled: true }] })) })
beforeEach(() => {
  vi.mocked(sendToCEF).mockReset().mockImplementation(async request => ({ ok: true, data: status(request.payload?.executionHostId as string) }))
  useAppStore.setState(state => ({ settingsScopeHostId: '*', hostScopedSettings: true, hostScopedSettingsValues: {}, sessions: [], mcpServers: [server], providers: [], executionHosts: [{ id: 'local', label: 'Local', transport: 'local' }, { id: 'ssh-lab', label: 'Lab', transport: 'ssh', sshAlias: 'lab' }], centralProviderConfiguration: { ...state.centralProviderConfiguration, enabled: true, uamControlEnabled: true }, cliVersionManager: { providers: [], remoteProviders: [] } }))
  host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host)
})
afterEach(() => { act(() => root.unmount()); host.remove(); vi.useRealTimers() })
async function render(providerId?: string, selectedHostId?: string) { await act(async () => root.render(<ProviderConnections providerId={providerId} selectedHostId={selectedHostId} onMcpSelect={vi.fn()} onPreferencesSelect={vi.fn()} />)) }
async function click(label: string) { await act(async () => host.querySelector<HTMLButtonElement>(`[aria-label="${label}"]`)!.click()) }

it.each(['local', 'ssh-lab'])('saves OpenCode progressive discovery in the exact %s scope after acknowledgement', async hostId => {
  await render('opencode-cli', hostId)
  const control = () => host.querySelector<HTMLInputElement>('[aria-label="OpenCode progressive MCP discovery"]')!
  expect(control().checked).toBe(true)
  expect(host.textContent).toContain('the next time OpenCode starts')
  for (const enabled of [false, true]) {
    let finish!: (value: unknown) => void
    vi.mocked(sendToCEF).mockImplementationOnce(() => new Promise(resolve => { finish = resolve as typeof finish }))
    await click('OpenCode progressive MCP discovery')
    expect(sendToCEF).toHaveBeenLastCalledWith({ action: 'setProviderMcpConfiguration', payload: { executionHostId: hostId, providerId: 'opencode-cli', serverId: 'opencode-progressive-mcp', enabled, kind: 'preference' } })
    expect(control().disabled).toBe(true)
    expect(host.textContent).toContain('Saving discovery preference')
    expect(control().checked).toBe(enabled)
    const reply = status(hostId)
    reply.providers.find(row => row.providerId === 'opencode-cli')!.progressiveMcpDiscoveryEnabled = enabled
    await act(async () => finish({ ok: true, data: reply }))
    expect(control().checked).toBe(enabled)
    expect(control().disabled).toBe(false)
  }
})

it('keeps a same-named MCP server independent from the discovery preference', async () => {
  const collision = { ...server, id: 'opencode-progressive-mcp', name: 'Separate server' }
  useAppStore.setState({ mcpServers: [collision] })
  const reply = status()
  for (const row of reply.providers) row.mcpServers = [{ id: collision.id, name: collision.name, enabled: false }]
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: reply })
  await render('opencode-cli')
  let finish!: (value: unknown) => void
  vi.mocked(sendToCEF).mockImplementationOnce(() => new Promise(resolve => { finish = resolve as typeof finish }))
  await click('OpenCode progressive MCP discovery')
  expect(host.querySelector('[aria-label="OpenCode Separate server"]')?.getAttribute('data-pending')).toBeNull()
  const discoveryReply = structuredClone(reply)
  discoveryReply.providers.find(row => row.providerId === 'opencode-cli')!.progressiveMcpDiscoveryEnabled = false
  await act(async () => finish({ ok: true, data: discoveryReply }))
  expect(host.querySelector('[aria-label="OpenCode Separate server"]')?.getAttribute('aria-checked')).toBe('false')
  const serverReply = structuredClone(discoveryReply)
  serverReply.providers.find(row => row.providerId === 'opencode-cli')!.mcpServers[0].enabled = true
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: serverReply })
  await click('OpenCode Separate server')
  expect(sendToCEF).toHaveBeenLastCalledWith({ action: 'setProviderMcpConfiguration', payload: { executionHostId: 'local', providerId: 'opencode-cli', serverId: collision.id, enabled: true } })
  expect(host.querySelector('[aria-label="OpenCode Separate server"]')?.getAttribute('aria-checked')).toBe('true')
  expect(host.querySelector<HTMLInputElement>('[aria-label="OpenCode progressive MCP discovery"]')?.checked).toBe(false)
  expect(host.querySelector('[role="alert"]')).toBeNull()
})

it.each(['server', 'discovery'])('keeps pending controls separate for a prefixed server ID when saving %s', async saving => {
  const collision = { ...server, id: 'preference:opencode-progressive-mcp', name: 'Prefixed server' }
  useAppStore.setState({ mcpServers: [collision] })
  const reply = status()
  for (const row of reply.providers) row.mcpServers = [{ id: collision.id, name: collision.name, enabled: true }]
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: reply })
  await render('opencode-cli')
  const serverControl = () => host.querySelector<HTMLButtonElement>('[aria-label="OpenCode Prefixed server"]')!
  const discoveryControl = () => host.querySelector<HTMLInputElement>('[aria-label="OpenCode progressive MCP discovery"]')!
  let finish!: (value: unknown) => void
  vi.mocked(sendToCEF).mockImplementationOnce(() => new Promise(resolve => { finish = resolve as typeof finish }))
  const discovery = saving === 'discovery'
  await click(discovery ? 'OpenCode progressive MCP discovery' : 'OpenCode Prefixed server')
  expect(sendToCEF).toHaveBeenLastCalledWith({ action: 'setProviderMcpConfiguration', payload: { executionHostId: 'local', providerId: 'opencode-cli', serverId: discovery ? 'opencode-progressive-mcp' : collision.id, enabled: false, ...(discovery ? { kind: 'preference' } : {}) } })
  expect(serverControl().disabled).toBe(!discovery)
  expect(serverControl().getAttribute('data-pending')).toBe(discovery ? null : 'true')
  expect(discoveryControl().disabled).toBe(discovery)
  expect(host.textContent?.includes('Saving discovery preference')).toBe(discovery)
  expect(serverControl().getAttribute('aria-checked')).toBe('true')
  expect(discoveryControl().checked).toBe(!discovery)
  const saved = structuredClone(reply)
  const row = saved.providers.find(item => item.providerId === 'opencode-cli')!
  if (discovery) row.progressiveMcpDiscoveryEnabled = false
  else row.mcpServers[0].enabled = false
  await act(async () => finish({ ok: true, data: saved }))
  expect(serverControl().disabled).toBe(false)
  expect(discoveryControl().disabled).toBe(false)
  expect(serverControl().getAttribute('data-pending')).toBeNull()
  expect(serverControl().getAttribute('aria-checked')).toBe(discovery ? 'true' : 'false')
  expect(discoveryControl().checked).toBe(!discovery)
  expect(host.querySelector('[role="alert"]')).toBeNull()
})

it('keeps the progressive setting unknown when an older backend omits it', async () => {
  const reply = status()
  reply.providers.find(row => row.providerId === 'opencode-cli')!.progressiveMcpDiscoveryEnabled = undefined
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: reply })
  await render('opencode-cli')
  const control = host.querySelector<HTMLInputElement>('[aria-label="OpenCode progressive MCP discovery"]')!
  expect(control.disabled).toBe(true)
  expect(host.textContent).toContain('Discovery setting unavailable')
  await click('OpenCode progressive MCP discovery')
  expect(vi.mocked(sendToCEF).mock.calls).toHaveLength(1)
})

it.each(['failure', 'mismatched acknowledgement', 'wrong host'])('keeps the unsaved progressive choice for retry after %s', async outcome => {
  await render('opencode-cli', 'ssh-lab')
  const control = () => host.querySelector<HTMLInputElement>('[aria-label="OpenCode progressive MCP discovery"]')!
  const button = (text: string) => [...host.querySelectorAll('button')].find(item => item.textContent === text)
  vi.mocked(sendToCEF).mockResolvedValueOnce(outcome === 'failure' ? { ok: false, error: 'Could not persist discovery.' } : { ok: true, data: status(outcome === 'wrong host' ? 'local' : 'ssh-lab') })
  await click('OpenCode progressive MCP discovery')
  expect(control().checked).toBe(false)
  expect(host.textContent).toContain('Not saved:')
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: false, error: 'Still failing.' })
  await act(async () => button('Retry')!.click())
  expect(sendToCEF).toHaveBeenLastCalledWith({ action: 'setProviderMcpConfiguration', payload: { executionHostId: 'ssh-lab', providerId: 'opencode-cli', serverId: 'opencode-progressive-mcp', enabled: false, kind: 'preference' } })
  expect(control().checked).toBe(false)
  await act(async () => button('Discard')!.click())
  expect(control().checked).toBe(true)
  expect(host.textContent).not.toContain('Not saved:')
})

it('shows no discovery row for other providers', async () => {
  await render('codex-cli')
  expect(host.querySelector('[aria-label="OpenCode progressive MCP discovery"]')).toBeNull()
})

it('ignores an old-host progressive save reply after switching computers', async () => {
  await render('opencode-cli')
  let finish!: (value: unknown) => void
  vi.mocked(sendToCEF).mockImplementationOnce(() => new Promise(resolve => { finish = resolve as typeof finish }))
  await click('OpenCode progressive MCP discovery')
  await render('opencode-cli', 'ssh-lab')
  const reply = status()
  reply.providers.find(row => row.providerId === 'opencode-cli')!.progressiveMcpDiscoveryEnabled = false
  await act(async () => finish({ ok: true, data: reply }))
  expect(host.querySelector<HTMLInputElement>('[aria-label="OpenCode progressive MCP discovery"]')?.checked).toBe(true)
  expect(host.querySelector('[role="alert"]')).toBeNull()
})

it('binds pending discovery saves to their host and keeps a newer save pending', async () => {
  await render('opencode-cli', 'local')
  let finishLocal!: (value: unknown) => void
  vi.mocked(sendToCEF).mockImplementationOnce(() => new Promise(resolve => { finishLocal = resolve as typeof finishLocal }))
  await click('OpenCode progressive MCP discovery')
  await render('opencode-cli', 'ssh-lab')
  const control = () => host.querySelector<HTMLInputElement>('[aria-label="OpenCode progressive MCP discovery"]')!
  expect(control().disabled).toBe(false)
  let finishRemote!: (value: unknown) => void
  vi.mocked(sendToCEF).mockImplementationOnce(() => new Promise(resolve => { finishRemote = resolve as typeof finishRemote }))
  await click('OpenCode progressive MCP discovery')
  expect(control().disabled).toBe(true)
  const localReply = status('local')
  localReply.providers.find(row => row.providerId === 'opencode-cli')!.progressiveMcpDiscoveryEnabled = false
  await act(async () => finishLocal({ ok: true, data: localReply }))
  expect(control().disabled).toBe(true)
  const remoteReply = status('ssh-lab')
  remoteReply.providers.find(row => row.providerId === 'opencode-cli')!.progressiveMcpDiscoveryEnabled = false
  await act(async () => finishRemote({ ok: true, data: remoteReply }))
  expect(control().disabled).toBe(false)
  expect(control().checked).toBe(false)
})

it('offers progressive discovery only on the OpenCode provider page', async () => {
  for (const provider of [undefined, ...SUPPORTED_CLI_PROVIDER_IDS.filter(id => id !== 'opencode-cli')]) {
    await render(provider)
    expect(host.querySelector('[aria-label="OpenCode progressive MCP discovery"]')).toBeNull()
  }
})

it('renders six compact provider rows with all server columns and no guessed connection success', async () => {
  await render()
  expect(host.querySelectorAll('tbody tr')).toHaveLength(6)
  expect(host.textContent).toContain('Docs')
  expect(host.textContent).not.toContain('0/0')
  expect(host.textContent).not.toContain('enabled server definitions')
  expect(host.textContent).not.toContain('Connected')
  expect(host.querySelector('[role="checkbox"][aria-label="Codex Docs"]')?.getAttribute('aria-checked')).toBe('true')
  expect(host.querySelector('[role="tablist"][aria-label="Provider settings host"]')).toBeNull()
})

it('shows installations and Computer Use availability for the selected host', async () => {
  useAppStore.setState({ cliVersionManager: { providers: [], remoteProviders: [{ providerId: 'codex-cli', executionHostId: 'ssh-lab', installedVersion: '9.1', installMethod: 'npm', selectedVersion: '', availableVersions: [], preferredVersion: '', status: 'verified', message: '', running: false, lastCommand: '', lastOutput: '' }] } })
  await render('codex-cli')
  expect(host.textContent).not.toContain('9.1')
  await render('codex-cli', 'ssh-lab')
  expect(host.textContent).toContain('9.1')
  expect(host.textContent).not.toContain('npm')
  expect(host.querySelector('[aria-label="Codex Computer Use"]')?.getAttribute('aria-checked')).toBe('false')
})

it('reads Off in the Computer Use column when the built-in setting is disabled', async () => {
  useAppStore.setState({ computerUseEnabled: false })
  await render('codex-cli')
  const cell = host.querySelector('[aria-label="Codex Computer Use"]')
  expect(cell?.getAttribute('aria-checked')).toBe('false')
  expect(cell?.getAttribute('title')).toContain('Off')
  useAppStore.setState({ computerUseEnabled: undefined })
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
  await click('Codex Shared setup')
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
  await render('codex-cli')
  await render('codex-cli', 'ssh-lab')
  const old = status(); old.providers.find(row => row.providerId === 'codex-cli')!.uamControlEnabled = false
  await act(async () => finish({ ok: true, data: old }))
  expect(host.querySelector('[aria-label="Codex UAM MCP"]')?.getAttribute('aria-checked')).toBe('true')
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: old })
  await click('Codex Shared setup')
  expect(host.querySelector('[aria-label="Codex UAM MCP"]')?.getAttribute('aria-checked')).toBe('true')
  expect(host.querySelector('[role="alert"]')).not.toBeNull()
})

it('treats runtime structured support as authoritative for Antigravity and never chat permission as build availability', async () => {
  useAppStore.setState({ providers: [{ id: 'antigravity-cli', supportsStructured: true }] as ReturnType<typeof useAppStore.getState>['providers'] })
  await render('antigravity-cli')
  expect(host.querySelector('[aria-label="Antigravity UAM MCP"]')).toHaveProperty('disabled', true)
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
  expect(host.querySelector('[role="alert"]')?.textContent).toContain('UAM MCP is off')
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
    expect(control?.classList.contains(label === 'Computer Use' || label === 'UAM MCP' ? 'uam-prov-avail' : 'uam-prov-switch')).toBe(true)
    expect(control?.disabled).toBe(true)
    expect(control?.title).not.toContain('Off')
  }
})

it('retains disabled red controls when their false value is verified', async () => {
  const disabled = status()
  disabled.providers.find(row => row.providerId === 'codex-cli')!.sharedSetupEnabled = false
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: disabled })
  await render('codex-cli')
  const control = host.querySelector<HTMLButtonElement>('[aria-label="Codex Shared setup"]')!
  expect(control.getAttribute('aria-checked')).toBe('false')
  expect(control.title).toBe('Off. Click to turn on.')
  expect(control.disabled).toBe(false)
})

it('ignores structurally identical pushes and unrelated settings while retaining action errors', async () => {
  useAppStore.setState({ providers: [{ id: 'codex-cli', supportsStructured: true }] as ReturnType<typeof useAppStore.getState>['providers'] })
  await render('codex-cli')
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: false, error: 'Save failed, retry this change.' })
  await click('Codex Shared setup')
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

it('collapses many server columns into one count chip whose popover holds each switch', async () => {
  const extra = ['Search', 'Files', 'Git'].map((name, index) => ({ ...server, id: `s${index}`, name }))
  useAppStore.setState({ mcpServers: [server, ...extra] })
  await render('codex-cli')
  expect(host.querySelector('[aria-label="Codex Docs"]')).toBeNull()
  expect(host.textContent).toContain('MCP servers')
  const chip = host.querySelector<HTMLButtonElement>('button[aria-haspopup="dialog"]')!
  expect(chip.textContent).toContain('1 of 4 on')
  await act(async () => chip.click())
  expect(document.body.querySelector('[aria-label="Codex Docs"]')?.getAttribute('aria-checked')).toBe('true')
  expect(document.body.querySelector('[aria-label="Codex Git (unknown)"]')).not.toBeNull()
  const popover = document.body.querySelector('[aria-label="Codex MCP servers"]')!
  expect(popover.textContent).not.toContain(server.workspaceDirectory)
  expect(popover.textContent).not.toContain(server.url)
})


it.each(['local', 'ssh-lab'])('shows known MCP setup and handshake states for %s without claiming live connectivity', async hostId => {
  const configured = status(hostId)
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: configured })
  await render('gemini-cli', hostId)
  expect(sendToCEF).toHaveBeenCalledWith(expect.objectContaining({ action: 'getProviderMcpStatus', payload: { executionHostId: hostId } }))
  expect(host.querySelector('.uam-prov-status')?.textContent).toBe('Configured, unverified')
  expect(host.textContent).not.toContain('Connected')
  const confirmed = status(hostId)
  confirmed.handshakeProofAvailable = true
  confirmed.providers[0].status = 'handshake-confirmed'
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: confirmed })
  await click('Refresh provider connections')
  expect(host.querySelector('.uam-prov-status')?.textContent).toBe('Handshake confirmed')
  expect(host.textContent).not.toContain('Connected')
  confirmed.handshakeProofAvailable = false
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: confirmed })
  await click('Refresh provider connections')
  expect(host.querySelector('.uam-prov-status')?.textContent).toBe('Unknown')
})

it('keeps endpoint and workspace paths out of MCP server overview summaries', async () => {
  await render('gemini-cli')
  expect(host.textContent).not.toContain(server.url)
  expect(host.textContent).not.toContain(server.workspaceDirectory)
  const headers = [...host.querySelectorAll('th')]
  expect(headers.find(header => header.textContent === server.name)?.hasAttribute('title')).toBe(false)
})

it('shows only the selected host effective MCP servers in the popup without guessing connectivity', async () => {
  const remoteServers = Array.from({ length: 4 }, (_, index) => ({ ...server, id: `remote-${index}`, name: `Remote ${index}`, executionHostId: 'ssh-lab' }))
  useAppStore.setState({ mcpServers: Array.from({ length: 4 }, (_, index) => ({ ...server, id: `global-${index}`, name: `Global ${index}` })), hostScopedSettingsValues: { 'ssh-lab': { defaultNewChatProviderId: '', providerChatDefaults: {}, mcpServers: remoteServers } } })
  vi.mocked(sendToCEF).mockImplementation(async request => {
    const reply = status(request.payload?.executionHostId as string)
    for (const row of reply.providers) row.mcpServers = remoteServers.map(item => ({ id: item.id, name: item.name, enabled: true }))
    if (request.action === 'setProviderMcpConfiguration') reply.providers.find(row => row.providerId === 'codex-cli')!.mcpServers[0].enabled = false
    return { ok: true, data: reply }
  })
  await render('codex-cli', 'ssh-lab')
  await click('Codex MCP servers, 4 of 4 on')
  const popup = document.querySelector('[role="dialog"][aria-label="Codex MCP servers"]')!
  expect(popup.textContent).toContain('Remote 0')
  expect(popup.textContent).not.toContain('Global')
  expect(popup.querySelector('[aria-label="Codex Remote 0"]')?.getAttribute('aria-checked')).toBe('true')
  expect(host.querySelector('.uam-prov-status')?.textContent).toBe('Configured, unverified')
  expect(host.textContent).not.toContain('Connected')
  await act(async () => popup.querySelector<HTMLButtonElement>('[aria-label="Codex Remote 0"]')!.click())
  expect(sendToCEF).toHaveBeenLastCalledWith({ action: 'setProviderMcpConfiguration', payload: { executionHostId: 'ssh-lab', providerId: 'codex-cli', serverId: 'remote-0', enabled: false } })
  expect(popup.querySelector('[aria-label="Codex Remote 0"]')?.getAttribute('aria-checked')).toBe('false')
})

it('refreshes when scoped MCP identities change and respects an explicitly empty host list', async () => {
  const remote = { ...server, id: 'remote', name: 'Remote', executionHostId: 'ssh-lab' }
  useAppStore.setState({ hostScopedSettingsValues: { 'ssh-lab': { defaultNewChatProviderId: '', providerChatDefaults: {}, mcpServers: [remote] } } })
  vi.mocked(sendToCEF).mockImplementation(async () => {
    const reply = status('ssh-lab')
    for (const row of reply.providers) row.mcpServers = useAppStore.getState().hostScopedSettingsValues['ssh-lab'].mcpServers.map(item => ({ id: item.id, name: item.name, enabled: true }))
    return { ok: true, data: reply }
  })
  await render('codex-cli', 'ssh-lab')
  const initial = vi.mocked(sendToCEF).mock.calls.length
  await act(async () => useAppStore.setState({ hostScopedSettingsValues: { 'ssh-lab': { defaultNewChatProviderId: '', providerChatDefaults: {}, mcpServers: [{ ...remote, id: 'replacement', name: 'Replacement' }] } } }))
  expect(vi.mocked(sendToCEF).mock.calls.length).toBe(initial + 1)
  expect(host.querySelector('thead')?.textContent).toContain('Replacement')
  expect(host.querySelector('thead')?.textContent).not.toContain('Docs')
  expect(host.querySelector('[aria-label="Codex Replacement"]')?.getAttribute('aria-checked')).toBe('true')
  await act(async () => useAppStore.setState({ hostScopedSettingsValues: { 'ssh-lab': { defaultNewChatProviderId: '', providerChatDefaults: {}, mcpServers: [] } } }))
  expect(host.querySelectorAll('th')).toHaveLength(5)
  expect(host.textContent).toContain('Configured, unverified')
})

it('filters foreign-host MCP definitions from inherited global columns', async () => {
  useAppStore.setState({ mcpServers: [server, { ...server, id: 'foreign', name: 'Foreign', executionHostId: 'ssh-other' }] })
  await render('codex-cli', 'ssh-lab')
  expect(host.querySelector('thead')?.textContent).toContain('Docs')
  expect(host.querySelector('thead')?.textContent).not.toContain('Foreign')
})

it('describes UAM MCP as on or off without implying opt-outs are forbidden', async () => {
  await render('codex-cli')
  expect(host.querySelector('[aria-label="Codex UAM MCP"]')?.getAttribute('title')).toBe('On. Managed in the UAM MCP service section.')
  const disabled = status()
  disabled.providers.find(row => row.providerId === 'codex-cli')!.uamControlEnabled = false
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: disabled })
  await click('Refresh provider connections')
  expect(host.querySelector('[aria-label="Codex UAM MCP"]')?.getAttribute('aria-checked')).toBe('false')
  expect(host.querySelector('[aria-label="Codex UAM MCP"]')?.getAttribute('title')).toBe('Off for this provider.')
})

it('closes an open MCP popup when its scoped list no longer has a chip', async () => {
  vi.useFakeTimers()
  const remoteServers = Array.from({ length: 4 }, (_, index) => ({ ...server, id: `remote-${index}`, name: `Remote ${index}`, executionHostId: 'ssh-lab' }))
  useAppStore.setState({ hostScopedSettingsValues: { 'ssh-lab': { defaultNewChatProviderId: '', providerChatDefaults: {}, mcpServers: remoteServers } } })
  await render('codex-cli', 'ssh-lab')
  await act(async () => host.querySelector<HTMLButtonElement>('button[aria-haspopup="dialog"]')!.click())
  expect(document.querySelector('[role="dialog"][aria-label="Codex MCP servers"]')).not.toBeNull()
  await act(async () => useAppStore.setState({ hostScopedSettingsValues: { 'ssh-lab': { defaultNewChatProviderId: '', providerChatDefaults: {}, mcpServers: [] } } }))
  expect(host.querySelector('button[aria-haspopup="dialog"]')).toBeNull()
  await act(async () => vi.advanceTimersByTime(500))
  expect(document.querySelector('[role="dialog"][aria-label="Codex MCP servers"]')).toBeNull()
})

it('keeps the discovery draft locked and unconfirmed when another switch save supersedes it', async () => {
  await render('opencode-cli')
  const control = () => host.querySelector<HTMLInputElement>('[aria-label="OpenCode progressive MCP discovery"]')!
  let finish!: (value: unknown) => void
  vi.mocked(sendToCEF).mockImplementationOnce(() => new Promise(resolve => { finish = resolve as typeof finish }))
  await click('OpenCode progressive MCP discovery')
  vi.mocked(sendToCEF).mockImplementationOnce(() => new Promise(() => {}))
  await click('OpenCode Shared setup')
  expect(control().disabled).toBe(true)
  const reply = status()
  reply.providers.find(row => row.providerId === 'opencode-cli')!.progressiveMcpDiscoveryEnabled = false
  await act(async () => finish({ ok: true, data: reply }))
  expect(control().checked).toBe(false)
  expect(host.textContent).toContain('Result not confirmed')
})

it('clears a failed discovery draft when switching computers and refocuses the switch on Discard', async () => {
  await render('opencode-cli', 'ssh-lab')
  const control = () => host.querySelector<HTMLInputElement>('[aria-label="OpenCode progressive MCP discovery"]')!
  const button = (text: string) => [...host.querySelectorAll('button')].find(item => item.textContent === text)
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: false, error: 'Could not persist discovery.' })
  await click('OpenCode progressive MCP discovery')
  await act(async () => button('Discard')!.click())
  expect(document.activeElement).toBe(control())
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: false, error: 'Could not persist discovery.' })
  await click('OpenCode progressive MCP discovery')
  expect(host.textContent).toContain('Not saved:')
  await render('opencode-cli', 'local')
  expect(host.textContent).not.toContain('Not saved:')
  expect(control().checked).toBe(true)
})

it('resolves an unconfirmed discovery draft when a refresh reports the drafted value', async () => {
  await render('opencode-cli')
  vi.mocked(sendToCEF).mockImplementationOnce(() => new Promise(resolve => resolve({ ok: false, error: 'Could not persist discovery.' })))
  await click('OpenCode progressive MCP discovery')
  expect(host.textContent).toContain('Not saved:')
  const reply = status()
  reply.providers.find(row => row.providerId === 'opencode-cli')!.progressiveMcpDiscoveryEnabled = false
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: reply })
  await click('Refresh provider connections')
  expect(host.textContent).not.toContain('Not saved:')
  expect(host.querySelector<HTMLInputElement>('[aria-label="OpenCode progressive MCP discovery"]')!.checked).toBe(false)
})
