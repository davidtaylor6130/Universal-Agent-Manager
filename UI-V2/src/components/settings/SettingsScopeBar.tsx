import { MenuSelect, Tooltip } from '../ui'
import type { ExecutionHost } from '../../types/session'
import { SETTINGS_ALL_ENVIRONMENTS } from '../../utils/settingsScope'
import { isSshHost } from '../../utils/remoteWorkspace'

/**
 * Which computer the settings on this page apply to. Only SSH hosts are offered;
 * "This computer" is always covered by "All environments". The selector is enabled where
 * per-computer actions exist; saves are only sent per host once the backend supports it.
 */
export function SettingsScopeBar({ hosts, value, selectorEnabled, supported, onChange }: {
  hosts: ExecutionHost[]
  /** Effective scope: SETTINGS_ALL_ENVIRONMENTS or a host id. */
  value: string
  selectorEnabled: boolean
  /** Backend accepts scopeExecutionHostId on saves. */
  supported: boolean
  onChange: (hostId: string) => void
}) {
  const sshHosts = hosts.filter((host) => isSshHost(host))
  const selected = sshHosts.find((host) => host.id === value)
  const options = [
    { value: SETTINGS_ALL_ENVIRONMENTS, label: 'All environments' },
    ...sshHosts.map((host) => ({ value: host.id, label: host.label, description: host.sshAlias })),
  ]
  return (
    <div className="mb-4 grid gap-1.5">
      <div className="flex flex-wrap items-center gap-3">
        <label htmlFor="settings-scope" className="text-xs font-medium" style={{ color: 'var(--text-2)' }}>Applying settings for</label>
        <Tooltip label={selectorEnabled ? undefined : 'Per-computer settings need an update to UAM'}>
          <span className="inline-flex">
            <MenuSelect id="settings-scope" label="Applying settings for" value={selectorEnabled ? value : SETTINGS_ALL_ENVIRONMENTS} options={options} disabled={!selectorEnabled} onChange={onChange} />
          </span>
        </Tooltip>
      </div>
      {selectorEnabled && selected && (supported
        ? <p className="text-xs" style={{ color: 'var(--text-3)' }}>Changes here apply only to {selected.label}.</p>
        : <p className="text-xs" style={{ color: 'var(--text-3)' }}>Computer actions only; defaults and MCP lists stay global.</p>)}
    </div>
  )
}
