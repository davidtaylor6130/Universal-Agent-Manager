import { useEffect, useRef, useState } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { useAppStore } from '../../store/useAppStore'
import { SUPPORTED_CLI_PROVIDER_IDS, providerMetadataForId } from '../../utils/providerMetadata'
import { ProviderLogo } from '../shared/ProviderLogo'
import { sendToCEF } from '../../ipc/cefBridge'
import { Check, Minus, RefreshCw } from 'lucide-react'
import { Button, IconButton, Notice, StatusDot, type StatusTone } from '../ui'
import './provider-settings.css'

interface McpStatus {
  executionHostId: string
  connectedProofAvailable: boolean
  handshakeProofAvailable?: boolean
  computerUseBuiltInAvailable?: boolean
  providers: Array<{ providerId: string; status: 'disabled' | 'missing' | 'configured-unverified' | 'failed' | 'handshake-confirmed' | 'connected'; detail: string; activeSessionCount: number; configuredSessionCount: number; uamControlEnabled?: boolean; sharedSetupEnabled?: boolean; mcpServers?: Array<{ id: string; name: string; enabled: boolean }> }>
}

interface Props {
  providerId?: string
  selectedHostId?: string
  onHostChange?: (id: string) => void
  onProviderSelect?: (id: string) => void
  onMcpSelect: () => void
  onPreferencesSelect: () => void
}

/** Provider controls show effective next-launch configuration; connection status needs live evidence. */
export function ProviderConnections({ providerId, selectedHostId, onHostChange, onProviderSelect, onMcpSelect, onPreferencesSelect }: Props) {
  const hosts = useAppStore(useShallow(state => state.executionHosts))
  const runtimeProviders = useAppStore(useShallow(state => state.providers))
  const versions = useAppStore(state => state.cliVersionManager)
  const central = useAppStore(state => state.centralProviderConfiguration)
  const servers = useAppStore(useShallow(state => state.mcpServers))
  const [ownHostId, setOwnHostId] = useState('local')
  const hostId = selectedHostId ?? ownHostId
  const setHostId = onHostChange ?? setOwnHostId
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const [status, setStatus] = useState<McpStatus | null>(null)
  const [statusError, setStatusError] = useState('')
  const [refresh, setRefresh] = useState(0)
  const requestGeneration = useRef(0)
  const busyRef = useRef(false)
  const ids = providerId ? [providerId] : SUPPORTED_CLI_PROVIDER_IDS
  const activeHostId = hosts.some(host => host.id === hostId) || hostId === 'local' ? hostId : 'local'
  const currentHost = useRef(activeHostId)
  currentHost.current = activeHostId
  const hostVersions = activeHostId === 'local' ? versions.providers : (versions.remoteProviders ?? []).filter(version => version.executionHostId === activeHostId)
  const configurationSignature = JSON.stringify([
    central.enabled, central.uamControlEnabled,
    (central.mcpOverrides ?? []).filter(override => override.executionHostId === activeHostId && ids.includes(override.providerId)),
    servers.map(server => [server.id, server.name, server.enabled, server.executionHostId, server.workspaceDirectory]),
    runtimeProviders.map(provider => [provider.id, provider.supportsStructured]),
  ])
  useEffect(() => setError(''), [activeHostId])
  const validStatus = (value: McpStatus | undefined, expectedHost: string): value is McpStatus => value?.executionHostId === expectedHost && Array.isArray(value.providers)
  useEffect(() => {
    const generation = ++requestGeneration.current
    setStatus(previous => previous?.executionHostId === activeHostId ? previous : null); setStatusError('')
    void sendToCEF<McpStatus>({ action: 'getProviderMcpStatus', payload: { executionHostId: activeHostId } }).then(result => {
      if (generation !== requestGeneration.current || currentHost.current !== activeHostId) return
      if (result.ok && validStatus(result.data, activeHostId)) setStatus(result.data)
      else { setStatus(null); setStatusError(result.error || 'Connection checks are unavailable in this build.') }
    }).catch(() => { if (generation === requestGeneration.current) { setStatus(null); setStatusError('Could not check provider MCP setup.') } })
    return () => { requestGeneration.current++ }
  }, [activeHostId, refresh, configurationSignature])
  useEffect(() => {
    const timer = window.setInterval(() => { if (!busyRef.current) setRefresh(value => value + 1) }, 30_000)
    return () => window.clearInterval(timer)
  }, [])
  const enable = async () => {
    setBusy(true); busyRef.current = true; setError('')
    try {
      const store = useAppStore.getState()
      const result = await store.setCentralProviderConfiguration({ ...store.centralProviderConfiguration, uamControlEnabled: true })
      if (result.ok) setRefresh(value => value + 1)
      if (!result.ok) setError(result.error || 'Could not enable UAM MCP.')
    } catch { setError('Could not enable UAM MCP. Try again.') }
    finally { setBusy(false); busyRef.current = false }
  }
  const setConfiguration = async (id: string, serverId: string, enabled: boolean) => {
    const generation = ++requestGeneration.current
    const selectedHost = activeHostId
    setBusy(true); busyRef.current = true; setError('')
    try {
      const result = await sendToCEF<McpStatus>({ action: 'setProviderMcpConfiguration', payload: { executionHostId: selectedHost, providerId: id, serverId, enabled } })
      if (generation !== requestGeneration.current || currentHost.current !== selectedHost) return
      const row = result.data?.providers?.find(provider => provider.providerId === id)
      const actual = serverId === 'uam-mcp' ? row?.uamControlEnabled : serverId === 'shared-setup' ? row?.sharedSetupEnabled : row?.mcpServers?.find(server => server.id === serverId)?.enabled
      if (result.ok && validStatus(result.data, selectedHost) && actual === enabled) setStatus(result.data)
      else setError(result.error || 'The provider MCP change could not be verified. Refresh and try again.')
    } catch { if (generation === requestGeneration.current) setError('Could not save provider MCP configuration. Try again.') }
    finally { setBusy(false); busyRef.current = false }
  }
  /** Configuration switch. Unknown (undefined) renders as a dashed, disabled switch. */
  const checkbox = (label: string, checked: boolean | undefined, onChange?: () => void, title?: string) => <button type="button" role="checkbox" className="uam-prov-switch" aria-label={checked === undefined ? `${label} (unknown)` : label} aria-checked={checked === undefined ? 'mixed' : checked} disabled={busy || checked === undefined || !onChange} title={title ?? (checked === undefined ? 'Not known yet' : checked ? 'On. Click to turn off.' : 'Off. Click to turn on.')} onClick={onChange}/>
  return <div className="space-y-3">
    <div className="flex items-center gap-2 flex-wrap">
      <div role="tablist" aria-label="Provider settings host" className="uam-prov-seg">
        {[{ id: 'local', label: 'This computer' }, ...hosts.filter(host => host.id !== 'local')].map(host => <button type="button" key={host.id} role="tab" className="uam-prov-seg__item" aria-label={host.label} aria-selected={host.id === activeHostId} onClick={() => setHostId(host.id)}>{host.label}</button>)}
      </div>
      <IconButton size="sm" variant="ghost" label="Refresh provider connections" tooltip="Check connections again" disabled={busy} onClick={() => setRefresh(value => value + 1)} icon={<RefreshCw size={14} className={!status && !statusError ? 'uam-prov-spin' : undefined}/>}/>
      <div className="ml-auto flex gap-1">
        <Button size="sm" variant="ghost" onClick={onPreferencesSelect}>Shared agent preferences</Button>
        <Button size="sm" onClick={onMcpSelect}>Manage MCP servers</Button>
      </div>
    </div>
    {!central.uamControlEnabled && <Notice tone="error" title="UAM MCP is off" dismissLabel="Dismiss UAM MCP warning" actions={<Button size="sm" disabled={busy} onClick={() => void enable()}>Enable UAM MCP</Button>}>New provider sessions will run without UAM tools. Existing sessions keep their current configuration.</Notice>}
    {error && <p role="alert" className="uam-prov-sub" style={{ color: 'var(--error)' }}>{error}</p>}
    {statusError && <p role="status" className="uam-prov-sub">{statusError}</p>}
    {!status && !statusError && <p role="status" className="uam-prov-sub">Checking connections…</p>}
    <div className="uam-prov-card overflow-x-auto">
      <table className="w-full text-xs text-left" aria-label="Provider connection overview">
        <thead><tr style={{ borderBottom: '1px solid var(--border)' }}>{['Provider', 'Status', 'UAM MCP', 'Computer Use', 'Shared setup'].map(label => <th key={label} className={`uam-prov-label px-3 py-2${label === 'Provider' ? ' uam-prov-sticky' : ''} font-medium whitespace-nowrap`}>{label}</th>)}{servers.map(server => <th key={server.id} title={[server.executionHostId, server.workspaceDirectory].filter(Boolean).join(' · ')} className="uam-prov-label px-3 py-2 font-medium whitespace-nowrap">{server.name}</th>)}</tr></thead>
        <tbody>{ids.map(id => {
          const metadata = providerMetadataForId(id)
          const runtime = runtimeProviders.find(provider => provider.id === id)
          const terminalOnly = runtime?.supportsStructured === undefined ? metadata.structuredProtocol === 'none' : !runtime.supportsStructured
          const version = hostVersions.find(item => item.providerId === id)
          const health = status?.executionHostId === activeHostId ? status.providers.find(item => item.providerId === id) : undefined
          const healthLabel = health?.status === 'connected' && status?.connectedProofAvailable ? 'Connected' : health?.status === 'failed' ? 'Failed' : health?.status === 'missing' ? 'Not configured' : health?.status === 'disabled' ? 'Disabled' : 'Unknown'
          const healthTone: StatusTone = terminalOnly ? 'neutral' : healthLabel === 'Connected' ? 'success' : healthLabel === 'Failed' ? 'error' : healthLabel === 'Not configured' ? 'warning' : 'neutral'
          const cli = version?.installedVersion || (version?.status === 'checking' ? 'Checking' : version?.status === 'unavailable' ? 'Not installed' : 'Not checked')
          return <tr key={id} style={{ borderTop: '1px solid var(--border)' }}>
            <td className="uam-prov-sticky px-3 py-2"><button type="button" className="uam-prov-provider" onClick={() => onProviderSelect?.(id)} disabled={!onProviderSelect}><ProviderLogo providerId={id}/><span className="grid min-w-0"><span className="uam-prov-provider__name">{metadata.shortName}</span><span className="uam-prov-detail whitespace-nowrap">{cli}</span></span></button></td>
            <td className="px-3 py-2"><span className="uam-prov-status" data-tone={healthTone} title={health?.detail}><StatusDot tone={healthTone}/>{terminalOnly ? 'Terminal only' : healthLabel}</span>
              {!terminalOnly && health?.status === 'failed' && health.detail && <div className="uam-prov-detail uam-prov-detail--clip mt-0.5" title={health.detail}>{health.detail}</div>}</td>
            <td className="px-2 py-2">{checkbox(`${metadata.shortName} UAM MCP`, health?.uamControlEnabled, terminalOnly ? undefined : () => void setConfiguration(id, 'uam-mcp', !health?.uamControlEnabled))}</td>
            <td className="px-2 py-2">{(() => {
              const available = status?.executionHostId === activeHostId && typeof status.computerUseBuiltInAvailable === 'boolean' ? !terminalOnly && status.computerUseBuiltInAvailable : undefined
              // Availability, not a setting: rendered as a read-only status glyph.
              return <button type="button" role="checkbox" disabled className="uam-prov-avail" data-state={available === undefined ? 'unknown' : available ? 'on' : 'off'} aria-label={available === undefined ? `${metadata.shortName} Computer Use (unknown)` : `${metadata.shortName} Computer Use`} aria-checked={available === undefined ? 'mixed' : available} title={available ? 'Available. Chat permissions are set in the composer.' : available === false ? 'Not available for this provider' : 'Not known yet'}>{available ? <Check size={13}/> : <Minus size={13}/>}</button>
            })()}</td>
            <td className="px-2 py-2">{checkbox(`${metadata.shortName} Shared setup`, health?.sharedSetupEnabled, terminalOnly ? undefined : () => void setConfiguration(id, 'shared-setup', !health?.sharedSetupEnabled))}</td>
            {servers.map(server => {
              const enabled = health?.mcpServers?.find(item => item.id === server.id)?.enabled
              return <td key={server.id} className="px-2 py-2">{checkbox(`${metadata.shortName} ${server.name}`, enabled, terminalOnly ? undefined : () => void setConfiguration(id, server.id, !enabled))}</td>
            })}
          </tr>
        })}</tbody>
      </table>
    </div>
  </div>
}
