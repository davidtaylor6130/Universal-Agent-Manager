import { useEffect, useState } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { useAppStore } from '../../store/useAppStore'
import { SUPPORTED_CLI_PROVIDER_IDS, providerMetadataForId } from '../../utils/providerMetadata'
import { ProviderLogo } from '../shared/ProviderLogo'
import { sendToCEF } from '../../ipc/cefBridge'
import { Button, MenuSelect, Notice } from '../ui'

interface McpStatus {
  executionHostId: string
  connectedProofAvailable: boolean
  handshakeProofAvailable?: boolean
  providers: Array<{ providerId: string; status: 'disabled' | 'missing' | 'configured-unverified' | 'failed' | 'handshake-confirmed'; detail: string; activeSessionCount: number; configuredSessionCount: number }>
}

interface Props {
  providerId?: string
  selectedHostId?: string
  onHostChange?: (id: string) => void
  onProviderSelect?: (id: string) => void
  onMcpSelect: () => void
  onPreferencesSelect: () => void
}

/** Settings describe configuration until a provider handshake supplies runtime evidence. */
export function ProviderConnections({ providerId, selectedHostId, onHostChange, onProviderSelect, onMcpSelect, onPreferencesSelect }: Props) {
  const hosts = useAppStore(useShallow(state => state.executionHosts))
  const runtimeProviders = useAppStore(useShallow(state => state.providers))
  const versions = useAppStore(state => state.cliVersionManager)
  const central = useAppStore(state => state.centralProviderConfiguration)
  const servers = useAppStore(useShallow(state => state.mcpServers))
  const sessions = useAppStore(useShallow(state => state.sessions))
  const [ownHostId, setOwnHostId] = useState('local')
  const hostId = selectedHostId ?? ownHostId
  const setHostId = onHostChange ?? setOwnHostId
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const [status, setStatus] = useState<McpStatus | null>(null)
  const [statusError, setStatusError] = useState('')
  const [refresh, setRefresh] = useState(0)
  const ids = providerId ? [providerId] : SUPPORTED_CLI_PROVIDER_IDS
  const activeHostId = hosts.some(host => host.id === hostId) || hostId === 'local' ? hostId : 'local'
  const hostVersions = activeHostId === 'local' ? versions.providers : (versions.remoteProviders ?? []).filter(version => version.executionHostId === activeHostId)
  useEffect(() => {
    let current = true
    setStatus(null); setStatusError('')
    void sendToCEF<McpStatus>({ action: 'getProviderMcpStatus', payload: { executionHostId: activeHostId } }).then(result => {
      if (!current) return
      if (result.ok && result.data?.executionHostId === activeHostId && Array.isArray(result.data.providers)) setStatus(result.data)
      else setStatusError(result.error || 'Connection checks are unavailable in this build.')
    }).catch(() => { if (current) setStatusError('Could not check provider MCP setup.') })
    return () => { current = false }
  }, [activeHostId, refresh, central.uamControlEnabled])
  const enable = async () => {
    setBusy(true); setError('')
    try {
      const store = useAppStore.getState()
      const result = await store.setCentralProviderConfiguration({ ...store.centralProviderConfiguration, uamControlEnabled: true })
      if (result.ok) setRefresh(value => value + 1)
      if (!result.ok) setError(result.error || 'Could not enable UAM MCP.')
    } catch { setError('Could not enable UAM MCP. Try again.') }
    finally { setBusy(false) }
  }
  return <div className="space-y-4">
    <label className="grid gap-1 text-xs max-w-sm">Host
      <MenuSelect label="Provider settings host" value={activeHostId} onChange={setHostId} options={[{ value: 'local', label: 'This computer' }, ...hosts.filter(host => host.id !== 'local').map(host => ({ value: host.id, label: host.label }))]} />
    </label>
    {!central.uamControlEnabled && <Notice tone="error" title="WARNING: UAM MCP is disabled" dismissLabel="Dismiss UAM MCP warning" actions={<Button size="sm" disabled={busy} onClick={() => void enable()}>Enable UAM MCP</Button>}>New provider sessions will run without UAM tools. Existing sessions keep their current configuration.</Notice>}
    <div className="overflow-x-auto">
      <table className="w-full text-xs text-left" aria-label="Provider connection overview">
        <thead><tr style={{ borderBottom: '1px solid var(--border)' }}>{['Provider', 'CLI', 'UAM MCP', 'Computer Use', 'Shared setup'].map(label => <th key={label} className="px-2 py-2 font-medium">{label}</th>)}</tr></thead>
        <tbody>{ids.map(id => {
          const metadata = providerMetadataForId(id)
          const terminalOnly = runtimeProviders.find(provider => provider.id === id)?.supportsStructured === false || metadata.structuredProtocol === 'none'
          const version = hostVersions.find(item => item.providerId === id)
          const providerSessions = sessions.filter(session => session.providerId === id && (session.executionHostId || 'local') === activeHostId)
          const enabledComputerSessions = providerSessions.filter(session => session.computerUseEnabled).length
          const health = status?.providers.find(item => item.providerId === id)
          const healthLabel = health?.status === 'handshake-confirmed' && status?.handshakeProofAvailable ? 'Handshake confirmed' : health?.status === 'failed' ? 'Failed' : health?.status === 'missing' ? 'Missing' : health?.status === 'disabled' ? 'Disabled' : central.uamControlEnabled ? 'Enabled, unverified' : 'Disabled'
          const cli = version?.installedVersion || (version?.status === 'checking' ? 'Checking' : version?.status === 'unavailable' ? 'Not installed' : 'Not checked')
          return <tr key={id} style={{ borderBottom: '1px solid var(--border)' }}>
            <td className="px-2 py-3"><button type="button" className="flex items-center gap-2 text-left" onClick={() => onProviderSelect?.(id)} disabled={!onProviderSelect}><ProviderLogo providerId={id}/>{metadata.shortName}</button></td>
            <td className="px-2 py-3">{cli}{version?.installMethod && <div style={{ color: 'var(--text-3)' }}>{version.installMethod}</div>}</td>
            <td className="px-2 py-3"><span title={health?.detail}>{terminalOnly ? 'Terminal only' : healthLabel}</span>{!terminalOnly && health && <div style={{ color: 'var(--text-3)' }}>{health.configuredSessionCount}/{health.activeSessionCount} active sessions configured</div>}{!terminalOnly && ['missing', 'disabled', 'failed'].includes(health?.status ?? '') && <Button size="sm" disabled={busy} onClick={() => void enable()}>Enable for new sessions</Button>}</td>
            <td className="px-2 py-3">{terminalOnly ? 'Terminal only' : activeHostId !== 'local' ? 'Unavailable over SSH' : enabledComputerSessions ? `Enabled in ${enabledComputerSessions} chats, unverified` : 'Off'}</td>
            <td className="px-2 py-3">{central.enabled ? 'Enabled for new sessions' : 'Disabled'}</td>
          </tr>
        })}</tbody>
      </table>
    </div>
    {providerId && status?.providers.find(item => item.providerId === providerId)?.detail && <p className="text-xs">{status.providers.find(item => item.providerId === providerId)?.detail}</p>}
    <p className="text-xs" style={{ color: 'var(--text-3)' }}>Enabled describes the requested setup. It does not prove the provider connected. Running chats keep their setup until restarted.</p>
    {statusError && <p role="status" className="text-xs" style={{ color: 'var(--text-3)' }}>{statusError}</p>}
    <div className="flex flex-wrap gap-2"><Button size="sm" onClick={() => setRefresh(value => value + 1)}>Check connections</Button><Button size="sm" onClick={onMcpSelect}>Manage MCP servers</Button><Button size="sm" onClick={onPreferencesSelect}>Shared agent preferences</Button></div>
    <div className="text-xs">{servers.filter(server => server.enabled && (!server.executionHostId || server.executionHostId === activeHostId)).length} enabled server definitions on this host. Workspace restrictions also apply.</div>
    {error && <p role="alert" className="text-xs" style={{ color: 'var(--red)' }}>{error}</p>}
  </div>
}
