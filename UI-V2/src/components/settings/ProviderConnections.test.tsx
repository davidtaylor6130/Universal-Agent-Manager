import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, expect, it, vi } from 'vitest'
import { ProviderConnections } from './ProviderConnections'
import { useAppStore } from '../../store/useAppStore'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
beforeEach(() => {
  useAppStore.setState(state => ({
    sessions: [], mcpServers: [],
    executionHosts: [{ id: 'local', label: 'Local', transport: 'local' }, { id: 'ssh-lab', label: 'Lab', transport: 'ssh', sshAlias: 'lab' }],
    centralProviderConfiguration: { ...state.centralProviderConfiguration, enabled: true, uamControlEnabled: true },
    cliVersionManager: { providers: [], remoteProviders: [] },
  }))
})
function render(providerId?: string) {
  const host = document.createElement('div'); document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(<ProviderConnections providerId={providerId} onMcpSelect={vi.fn()} onPreferencesSelect={vi.fn()} />))
  return { host, dispose: () => { act(() => root.unmount()); host.remove() } }
}
it('does not present requested configuration as a successful connection', () => {
  const { host, dispose } = render()
  expect(host.querySelectorAll('tbody tr')).toHaveLength(6)
  expect(host.textContent).toContain('Enabled, unverified')
  expect(host.textContent).not.toContain('Connected')
  expect(host.textContent).toContain('Antigravity')
  dispose()
})
it('shows a remote installation only under its actual host', () => {
  useAppStore.setState({ cliVersionManager: { providers: [], remoteProviders: [{ providerId: 'codex-cli', executionHostId: 'ssh-lab', installedVersion: '9.1', selectedVersion: '', availableVersions: [], preferredVersion: '', status: 'verified', message: '', running: false, lastCommand: '', lastOutput: '' }] } })
  const { host, dispose } = render('codex-cli')
  expect(host.textContent).not.toContain('9.1')
  act(() => host.querySelector<HTMLButtonElement>('[aria-label="Provider settings host"]')!.click())
  act(() => [...document.querySelectorAll<HTMLButtonElement>('[role="option"]')].find(option => option.textContent === 'Lab')!.click())
  expect(host.textContent).toContain('9.1')
  expect(host.textContent).toContain('Unavailable over SSH')
  dispose()
})
it('repairs disabled UAM setup without discarding shared preferences and displays failure', async () => {
  const setter = vi.fn(async () => ({ ok: false, error: 'Cannot write provider configuration.' }))
  useAppStore.setState(state => ({ centralProviderConfiguration: { ...state.centralProviderConfiguration, uamControlEnabled: false, instructions: 'Keep instructions' }, setCentralProviderConfiguration: setter }))
  const { host, dispose } = render()
  expect(host.querySelector('[role="alert"]')?.textContent).toContain('WARNING: UAM MCP is disabled')
  await act(async () => [...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === 'Enable UAM MCP')!.click())
  expect(setter).toHaveBeenCalledWith(expect.objectContaining({ uamControlEnabled: true, instructions: 'Keep instructions' }))
  expect(host.textContent).toContain('Cannot write provider configuration.')
  dispose()
})
it('shows handshake evidence without claiming socket liveness and rejects stale host results', async () => {
  const replies: Array<{ hostId: string; reply: (text: string) => void }> = []
  window.cefQuery = ({ request, onSuccess }) => {
    const parsed = JSON.parse(request)
    replies.push({ hostId: parsed.payload.executionHostId, reply: onSuccess })
  }
  const { host, dispose } = render('codex-cli')
  await act(async () => replies[0].reply(JSON.stringify({ executionHostId: 'local', connectedProofAvailable: false, handshakeProofAvailable: false, providers: [{ providerId: 'codex-cli', status: 'handshake-confirmed', detail: 'No handshake proof', activeSessionCount: 0, configuredSessionCount: 0 }] })))
  expect(host.querySelector('tbody')?.textContent).not.toContain('Connected')
  act(() => host.querySelector<HTMLButtonElement>('[aria-label="Provider settings host"]')!.click())
  act(() => [...document.querySelectorAll<HTMLButtonElement>('[role="option"]')].find(option => option.textContent === 'Lab')!.click())
  await act(async () => replies[1].reply(JSON.stringify({ executionHostId: 'ssh-lab', connectedProofAvailable: false, handshakeProofAvailable: true, providers: [{ providerId: 'codex-cli', status: 'handshake-confirmed', detail: 'Handshake succeeded', activeSessionCount: 1, configuredSessionCount: 1 }] })))
  expect(host.querySelector('tbody')?.textContent).toContain('Handshake confirmed')
  expect(host.querySelector('tbody')?.textContent).not.toContain('Connected')
  act(() => host.querySelector<HTMLButtonElement>('[aria-label="Provider settings host"]')!.click())
  act(() => [...document.querySelectorAll<HTMLButtonElement>('[role="option"]')].find(option => option.textContent === 'This computer')!.click())
  expect(host.querySelector('tbody')?.textContent).not.toContain('Handshake confirmed')
  dispose()
  window.cefQuery = undefined
})

it('keeps UAM MCP enabled when shared instructions are disabled', () => {
  useAppStore.setState(state => ({ centralProviderConfiguration: { ...state.centralProviderConfiguration, enabled: false, uamControlEnabled: true } }))
  const { host, dispose } = render('codex-cli')
  expect(host.querySelector('[role="alert"]')).toBeNull()
  expect(host.querySelector('tbody')?.textContent).toContain('Enabled, unverified')
  dispose()
})
