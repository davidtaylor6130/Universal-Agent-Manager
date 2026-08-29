import { ArrowUpCircle, Download, ExternalLink, RefreshCw, X } from 'lucide-react'
import { useState } from 'react'
import type { UpdateMonitor } from '../../hooks/useUpdateMonitor'
import { Button, IconButton } from '../ui'

export function UpdatesPanel({ monitor, onClose }: { monitor: UpdateMonitor; onClose: () => void }) {
  const [installError, setInstallError] = useState('')
  const checked = monitor.lastCheckedAt ? new Date(monitor.lastCheckedAt) : null
  const checkedLabel = checked && !Number.isNaN(checked.getTime())
    ? `Last checked ${checked.toLocaleString()}`
    : 'Not checked yet'

  return (
    <aside
      aria-label="Updates"
      data-testid="updates-panel"
      className="uam-side-panel-in uam-shell-panel uam-shell-panel--right flex h-full w-[360px] max-w-full shrink-0 flex-col overflow-hidden"
      style={{ background: 'var(--surface)', borderLeft: '1px solid var(--border)' }}
    >
      <header className="flex items-center justify-between gap-3 px-4 py-3" style={{ borderBottom: '1px solid var(--border)' }}>
        <div className="min-w-0">
          <div className="flex items-center gap-2 text-sm font-semibold" style={{ color: 'var(--text)' }}>
            <ArrowUpCircle size={16} /> Updates
          </div>
          <div className="mt-0.5 text-[11px]" style={{ color: 'var(--text-3)' }}>{checkedLabel}</div>
        </div>
        <div className="flex items-center gap-1">
          <Button
            size="sm"
            variant="ghost"
            leadingIcon={<RefreshCw size={14} className={monitor.checking ? 'animate-spin' : ''} aria-hidden />}
            aria-label={monitor.checking ? 'Checking for updates' : 'Check for updates'}
            aria-busy={monitor.checking}
            disabled={monitor.checking}
            onClick={() => { void monitor.checkNow() }}
          >
            {monitor.checking ? 'Checking…' : 'Check again'}
          </Button>
          <IconButton icon={<X size={16} />} label="Close updates" onClick={onClose} />
        </div>
      </header>

      <div className="min-w-0 flex-1 overflow-y-auto p-4">
        {monitor.error && (
          <div role="alert" className="mb-3 rounded-lg p-3 text-xs" style={{ color: 'var(--red)', border: '1px solid var(--red)', background: 'var(--surface-up)' }}>
            {monitor.error}
          </div>
        )}
        {installError && (
          <div role="alert" className="mb-3 rounded-lg p-3 text-xs" style={{ color: 'var(--red)', border: '1px solid var(--red)', background: 'var(--surface-up)' }}>
            {installError}
          </div>
        )}

        {monitor.providerUpdateResults.length > 0 && (
          <div className="mb-3 grid min-w-0 max-w-full gap-2">
            {monitor.providerUpdateResults.map((result) => (
              <div
                key={result.providerId}
                role={result.status === 'failed' ? 'alert' : 'status'}
                className="min-w-0 max-w-full rounded-lg p-3 text-xs"
                style={{
                  color: result.status === 'failed' ? 'var(--red)' : 'var(--green)',
                  border: `1px solid ${result.status === 'failed' ? 'var(--red)' : 'var(--green)'}`,
                  background: 'var(--surface-up)',
                }}
              >
                <div className="font-semibold">
                  {result.name} update {result.status === 'failed' ? 'failed' : 'completed'}
                </div>
                <div className="mt-1" style={{ color: 'var(--text-2)' }}>
                  {result.message || (result.status === 'failed' ? 'The installer returned an error.' : `Installed ${result.installedVersion}.`)}
                </div>
                {result.output && (
                  <details className="mt-2 min-w-0 max-w-full">
                    <summary className="cursor-pointer">Installer output</summary>
                    <pre className="mt-1 max-h-36 max-w-full overflow-auto whitespace-pre-wrap break-all rounded p-2 font-mono text-[10px]" style={{ color: 'var(--text-2)', background: 'var(--bg)' }}>
                      {result.output}
                    </pre>
                  </details>
                )}
              </div>
            ))}
          </div>
        )}

        {monitor.updates.length === 0 ? monitor.checking ? (
          <div role="status" className="grid place-items-center gap-2 rounded-xl px-4 py-10 text-center" style={{ border: '1px solid var(--border)', color: 'var(--text-3)' }}>
            <RefreshCw size={24} className="animate-spin" />
            <div className="text-sm" style={{ color: 'var(--text-2)' }}>Checking for updates…</div>
          </div>
        ) : monitor.error ? (
          <div className="grid place-items-center gap-2 rounded-xl px-4 py-10 text-center" style={{ border: '1px solid var(--border)', color: 'var(--text-3)' }}>
            <ArrowUpCircle size={24} />
            <div className="text-sm" style={{ color: 'var(--text-2)' }}>Could not confirm update status</div>
            <div className="text-xs">Try the check again when the update services are reachable.</div>
          </div>
        ) : monitor.hasCatalog ? (
          <div className="grid place-items-center gap-2 rounded-xl px-4 py-10 text-center" style={{ border: '1px solid var(--border)', color: 'var(--text-3)' }}>
            <ArrowUpCircle size={24} />
            <div className="text-sm" style={{ color: 'var(--text-2)' }}>Everything is up to date</div>
            <div className="text-xs">UAM and detected provider CLIs have no newer known release.</div>
          </div>
        ) : (
          <div className="grid place-items-center gap-2 rounded-xl px-4 py-10 text-center" style={{ border: '1px solid var(--border)', color: 'var(--text-3)' }}>
            <ArrowUpCircle size={24} />
            <div className="text-sm" style={{ color: 'var(--text-2)' }}>Updates have not been checked</div>
            <div className="text-xs">Run a check to compare UAM and detected provider versions.</div>
          </div>
        ) : (
          <div className="grid min-w-0 max-w-full gap-3">
            <div
              className="flex items-center gap-3 rounded-lg px-3 py-2.5"
              style={{ background: 'var(--accent-dim)', border: '1px solid var(--border)' }}
            >
              <ArrowUpCircle size={18} aria-hidden style={{ color: 'var(--accent)' }} />
              <div>
                <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>
                  {monitor.updates.length} update{monitor.updates.length === 1 ? '' : 's'} available
                </div>
                <div className="text-[11px]" style={{ color: 'var(--text-2)' }}>
                  Review and install each update when convenient.
                </div>
              </div>
            </div>
            {monitor.updates.map((update) => {
              const providerState = monitor.providerStates.find((state) => state.providerId === update.providerId)
              const providerRunning = Boolean(providerState?.running)
              return (
                <article
                  key={update.id}
                  data-update-available="true"
                  className="grid min-w-0 max-w-full gap-3 rounded-lg p-3"
                  style={{ background: 'var(--surface-up)', border: '1px solid var(--border)' }}
                >
                  <div className="flex items-start justify-between gap-3">
                    <div className="min-w-0">
                      <div className="truncate text-sm font-semibold" style={{ color: 'var(--text)' }}>{update.name}</div>
                      <div className="mt-1.5 flex items-center gap-2 text-[11px]">
                        <span style={{ color: 'var(--text-3)' }}>Current <span className="font-mono">{update.currentVersion}</span></span>
                        <span aria-hidden style={{ color: 'var(--text-3)' }}>→</span>
                        <strong style={{ color: 'var(--text)' }}>Available <span className="font-mono">{update.latestVersion}</span></strong>
                      </div>
                    </div>
                    <IconButton
                      icon={<X size={14} />}
                      label={`Dismiss ${update.name} ${update.latestVersion}`}
                      onClick={() => monitor.dismiss(update.id, update.latestVersion)}
                    />
                  </div>
                  <div className="flex flex-wrap gap-2">
                    {update.remoteHostId ? (
                      <Button
                        size="sm"
                        variant="primary"
                        leadingIcon={<Download size={14} aria-hidden />}
                        aria-label={`Update ${update.name} to ${update.latestVersion}`}
                        loading={monitor.remoteHelperUpdatingId === update.remoteHostId}
                        disabled={Boolean(monitor.remoteHelperUpdatingId) && monitor.remoteHelperUpdatingId !== update.remoteHostId}
                        onClick={async () => {
                          setInstallError('')
                          if (!await monitor.applyRemoteHelperUpdate(update.remoteHostId!)) {
                            setInstallError(`${update.name} update failed. Check its SSH connection and bundled helper in Remote Hosts.`)
                          }
                        }}
                      >
                        {monitor.remoteHelperUpdatingId === update.remoteHostId ? 'Updating…' : 'Update helper'}
                      </Button>
                    ) : update.providerId && update.installable ? (
                      <Button
                        size="sm"
                        variant="primary"
                        leadingIcon={<Download size={14} aria-hidden />}
                        aria-label={`Update ${update.name} to ${update.latestVersion}`}
                        loading={providerRunning}
                        disabled={monitor.providerTaskRunning && !providerRunning}
                        onClick={async () => {
                          setInstallError('')
                          if (!await monitor.applyCliProviderVersion(update.providerId!, update.latestVersion)) {
                            setInstallError(`${update.name} update could not be started. Finish active provider work and try again.`)
                          }
                        }}
                      >
                        {providerState?.status === 'checking' ? 'Verifying…' : providerRunning ? 'Updating…' : 'Install update'}
                      </Button>
                    ) : (
                      <Button
                        size="sm"
                        variant="primary"
                        leadingIcon={<ExternalLink size={14} aria-hidden />}
                        aria-label={`View ${update.name} ${update.latestVersion} release`}
                        onClick={() => window.open(update.url, '_blank', 'noopener')}
                      >
                        View release
                      </Button>
                    )}
                    {update.providerId && !update.remoteHostId && (
                      <Button
                        size="sm"
                        variant="ghost"
                        leadingIcon={<ExternalLink size={14} aria-hidden />}
                        aria-label={`Open ${update.name} update instructions`}
                        onClick={() => window.open(update.url, '_blank', 'noopener')}
                      >
                        Release notes
                      </Button>
                    )}
                  </div>
                </article>
              )
            })}
          </div>
        )}
      </div>

      {monitor.updates.length > 0 && (
        <footer className="flex items-center justify-end p-3" style={{ borderTop: '1px solid var(--border)' }}>
          <IconButton size="sm" icon={<X size={14} />} label="Dismiss all updates" onClick={monitor.dismissAll} />
        </footer>
      )}
    </aside>
  )
}
