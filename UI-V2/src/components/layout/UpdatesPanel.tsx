import { ArrowUpCircle, Download, ExternalLink, RefreshCw, X } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import type { UpdateMonitor } from '../../hooks/useUpdateMonitor'
import { Button, IconButton } from '../ui'
import { Notice } from '../ui/Notice'

export function UpdatesPanel({ monitor, onClose }: { monitor: UpdateMonitor; onClose: () => void }) {
  const [installError, setInstallError] = useState('')
  const [checkError, setCheckError] = useState('')
  const [updatingAll, setUpdatingAll] = useState(false)
  const batchRef = useRef<AbortController | null>(null)
  useEffect(() => () => batchRef.current?.abort(), [])

  const checkFailure = monitor.error || checkError
	const installableUpdates = monitor.updates.filter((update) => update.remoteHostId || (update.providerId && update.installable))
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
      <header className="grid shrink-0 grid-cols-[minmax(0,1fr)_auto] items-start gap-3 px-4 py-3" style={{ borderBottom: '1px solid var(--border)' }}>
        <div className="min-w-0">
          <div className="flex items-center gap-2 text-sm font-semibold" style={{ color: 'var(--text)' }}>
            <ArrowUpCircle size={16} /> Updates
          </div>
          <div className="mt-0.5 text-[11px]" style={{ color: 'var(--text-3)' }}>{checkedLabel}</div>
        </div>
        <IconButton icon={<X size={16} />} label="Close updates" onClick={onClose} />
        <div className="col-span-2 flex min-w-0 flex-wrap items-center gap-1">
          {installableUpdates.length > 0 && (
            <Button
              size="sm"
              variant="primary"
              leadingIcon={<Download size={14} aria-hidden />}
              aria-label="Update everything"
              loading={updatingAll}
              disabled={updatingAll || monitor.providerTaskRunning || Boolean(monitor.remoteHelperUpdatingId)}
              onClick={async () => {
                if (batchRef.current) return
                const batch = new AbortController()
                batchRef.current = batch
                setUpdatingAll(true)
                setInstallError('')
                try {
                  for (const update of installableUpdates) {
                    if (batch.signal.aborted) break
                    const result = update.remoteHostId
                      ? await monitor.applyRemoteHelperUpdate(update.remoteHostId)
                      : { ok: await monitor.installCliProviderVersion(update.providerId!, update.latestVersion, update.executionHostId, batch.signal) }
                    if (!result.ok) {
                      if (!batch.signal.aborted) setInstallError(`Updates stopped at ${update.name}. ${result.error?.trim() || 'Check its update status before trying again.'}`)
                      break
                    }
                  }
                } catch {
                  if (!batch.signal.aborted) setInstallError('Updates stopped. Check update status before trying again.')
                } finally {
                  batchRef.current = null
                  if (!batch.signal.aborted) setUpdatingAll(false)
                }
              }}
            >
              {updatingAll ? 'Updating…' : 'Update everything'}
            </Button>
          )}
          <Button
            size="sm"
            variant="ghost"
            leadingIcon={<RefreshCw size={14} aria-hidden />}
            aria-label={monitor.checking ? 'Checking for updates' : 'Check for updates'}
            aria-busy={monitor.checking}
            disabled={monitor.checking || updatingAll}
            onClick={() => { setCheckError(''); void monitor.checkNow() }}
          >
            {monitor.checking ? 'Checking…' : 'Check again'}
          </Button>
        </div>
      </header>

      <div className="min-w-0 flex-1 overflow-y-auto p-4">
        {checkFailure && (
          <Notice key={`check:${monitor.lastCheckedAt}:${checkFailure}`} tone="error" title="Update check failed" dismissLabel="Dismiss update check error">
            {checkFailure}
          </Notice>
        )}
        {installError && (
          <Notice key={`install:${installError}`} tone="error" title="Update failed" dismissLabel="Dismiss update error" onDismiss={() => setInstallError('')}>
            {installError}
          </Notice>
        )}

        {monitor.providerCheckErrors.map((result) => (
          <Notice
            key={`check:${JSON.stringify([result.executionHostId, result.providerId])}:${result.message}`}
            tone="error"
            title={`${result.name} version check failed`}
            dismissLabel={`Dismiss ${result.name} version check error`}
            actions={<Button size="sm" variant="ghost" leadingIcon={<RefreshCw size={14} aria-hidden />}
              aria-label={`Retry ${result.name} version check`} disabled={monitor.checking || monitor.providerTaskRunning}
              onClick={async () => {
                setCheckError('')
                if (!await monitor.refreshCliProviderVersion(result.providerId, result.executionHostId)) {
                  setCheckError(`${result.name} check could not be started. Check its SSH connection and try again.`)
                }
              }}>Retry check</Button>}
          >
            {result.message}
          </Notice>
        ))}

        {monitor.providerUpdateResults.some((result) => result.status === 'failed') && (
          <div className="mb-3 grid min-w-0 max-w-full gap-2">
            {monitor.providerUpdateResults.filter((result) => result.status === 'failed').map((result) => (
              <Notice
                key={`${JSON.stringify([result.executionHostId || '', result.providerId])}:${result.message}`}
                tone="error"
                title={`${result.name} update failed`}
                dismissLabel={`Dismiss ${result.name} update error`}
              >
                <div className="mt-1" style={{ color: 'var(--text-2)' }}>
                  {result.message || 'The installer returned an error.'}
                </div>
                {result.output && (
                  <details className="mt-2 min-w-0 max-w-full">
                    <summary className="cursor-pointer">Installer output</summary>
                    <pre className="mt-1 max-h-36 max-w-full overflow-auto whitespace-pre-wrap break-all rounded p-2 font-mono text-[10px]" style={{ color: 'var(--text-2)', background: 'var(--bg)' }}>
                      {result.output}
                    </pre>
                  </details>
                )}
              </Notice>
            ))}
          </div>
        )}

        {monitor.updates.length === 0 ? monitor.checking ? (
          <div role="status" className="grid place-items-center gap-2 rounded-xl px-4 py-10 text-center" style={{ border: '1px solid var(--border)', color: 'var(--text-3)' }}>
            <RefreshCw size={24} />
            <div className="text-sm" style={{ color: 'var(--text-2)' }}>Checking for updates…</div>
          </div>
        ) : checkFailure || monitor.providerCheckErrors.length > 0 ? (
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
              const providerState = monitor.providerStates.find((state) => state.providerId === update.providerId && (state.executionHostId || '') === (update.executionHostId || ''))
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
                        disabled={updatingAll || (Boolean(monitor.remoteHelperUpdatingId) && monitor.remoteHelperUpdatingId !== update.remoteHostId)}
                        onClick={async () => {
                          setInstallError('')
                          const result = await monitor.applyRemoteHelperUpdate(update.remoteHostId!)
                          if (!result.ok) {
                            setInstallError(`${update.name} update failed. ${result.error?.trim() || 'Check its SSH connection and bundled helper in Remote Hosts.'}`)
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
                        disabled={updatingAll || (monitor.providerTaskRunning && !providerRunning)}
                        onClick={async () => {
                          setInstallError('')
                          if (!await monitor.applyCliProviderVersion(update.providerId!, update.latestVersion, ...update.executionHostId ? [update.executionHostId] : [])) {
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
