import { ArrowUpCircle, CheckCircle2, Download, ExternalLink, RefreshCw, X } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import type { UpdateMonitor } from '../../hooks/useUpdateMonitor'
import { Button, IconButton } from '../ui'
import { Notice } from '../ui/Notice'
import './panels.css'
import './update-release-notes.css'

// The build define is used directly here: this chunk imports buildFeatures through a chunk cycle, which stops
// the bundler from folding the imported constant and would ship the SSH helper branches.

export function UpdatesPanel({ monitor, onClose, closing = false }: { monitor: UpdateMonitor; onClose: () => void; closing?: boolean }) {
  const [installError, setInstallError] = useState('')
  const [checkError, setCheckError] = useState('')
  const [updatingAll, setUpdatingAll] = useState(false)
  const [blockedUpdate, setBlockedUpdate] = useState<(typeof monitor.updates)[number] | null>(null)
  const batchRef = useRef<AbortController | null>(null)
  const pendingInstalls = useRef(new Set<string>())
  const [pendingInstallIds, setPendingInstallIds] = useState<Set<string>>(() => new Set())
  useEffect(() => () => batchRef.current?.abort(), [])

  const checkFailure = monitor.error || checkError
	const installableUpdates = monitor.updates.filter((update) => (__UAM_FEATURE_SSH__ && update.remoteHostId) || (update.providerId && update.installable))
  const checked = monitor.lastCheckedAt ? new Date(monitor.lastCheckedAt) : null
  const checkedLabel = checked && !Number.isNaN(checked.getTime())
    ? `Last checked ${checked.toLocaleString()}`
    : 'Not checked yet'

  return (
    <aside
      aria-label="Updates"
      data-testid="updates-panel"
      data-state={closing ? 'closed' : 'open'}
      className="uam-shell-panel uam-shell-panel--right flex h-full w-[360px] max-w-full shrink-0 flex-col overflow-hidden"
      style={{ background: 'var(--surface)', color: 'var(--text)', borderLeft: '1px solid var(--border)', '--panel-w': '360px' } as React.CSSProperties}
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
              variant="secondary"
              leadingIcon={<Download size={14} aria-hidden />}
              aria-label="Update everything"
              loading={updatingAll}
              disabled={updatingAll || pendingInstallIds.size > 0 || monitor.providerTaskRunning || Boolean(monitor.remoteHelperUpdatingId)}
              onClick={async () => {
                if (batchRef.current || pendingInstalls.current.size > 0) return
                const batch = new AbortController()
                batchRef.current = batch
                setUpdatingAll(true)
                setInstallError('')
                try {
                  const providers = installableUpdates.filter((update): update is typeof update & { providerId: string } => Boolean(update.providerId && !(__UAM_FEATURE_SSH__ && update.remoteHostId)))
                  const results = await Promise.allSettled([
                    ...(providers.length ? [monitor.installProviderUpdates(providers)] : []),
                    ...(__UAM_FEATURE_SSH__ ? installableUpdates.filter((update) => update.remoteHostId).map((update) => monitor.applyRemoteHelperUpdate(update.remoteHostId!)) : []),
                  ])
                  const failures = results.flatMap((result, index) => result.status === 'rejected' || !result.value.ok
                    ? [`${providers.length && index === 0 ? 'Provider updates' : installableUpdates.filter((update) => __UAM_FEATURE_SSH__ && update.remoteHostId)[index - (providers.length ? 1 : 0)]?.name}: ${result.status === 'fulfilled' && result.value.error?.trim() || 'Check its update status and retry.'}`] : [])
                  if (failures.length && !batch.signal.aborted) setInstallError(failures.join('\n'))
                } catch {
                  if (!batch.signal.aborted) setInstallError('Some updates failed. Check each provider status and retry.')
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
        <label className="mb-3 flex items-start gap-2 text-xs">
          <input type="checkbox" checked={monitor.automaticProviderUpdates ?? false}
            onChange={(event) => { void monitor.setUpdateSettings({ automaticProviderUpdates: event.target.checked }) }} />
          <span>Install provider updates automatically. Active sessions delay installation.</span>
        </label>
        {blockedUpdate && (
          <Notice tone="warning" title={`${blockedUpdate.name} sessions are running`} dismissLabel="Cancel provider update" onDismiss={() => setBlockedUpdate(null)}
            actions={<><Button size="sm" variant="primary" onClick={async () => {
              const result = await monitor.stopSessionsAndUpdate(blockedUpdate.providerId!, blockedUpdate.latestVersion, blockedUpdate.executionHostId)
              if (result.ok) setBlockedUpdate(null)
              else setInstallError(result.error || 'Could not stop the provider sessions. The update was not installed.')
            }}>Stop session and install update</Button><Button size="sm" variant="ghost" onClick={() => setBlockedUpdate(null)}>Cancel</Button></>}>
            Only this provider's sessions on this machine will stop. Current work will end.
          </Notice>
        )}
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
            onDismiss={() => monitor.dismissProviderCheckError(result)}
            actions={<Button size="sm" variant="ghost" leadingIcon={<RefreshCw size={14} aria-hidden />}
              aria-label={`Retry ${result.name} version check`} disabled={monitor.checking || monitor.providerTaskRunning}
              onClick={async () => {
                setCheckError('')
                if (!await monitor.refreshCliProviderVersion(result.providerId, result.executionHostId)) {
                  setCheckError(__UAM_FEATURE_SSH__ ? `${result.name} check could not be started. Check its SSH connection and try again.` : `${result.name} check could not be started. Try again.`)
                }
              }}>Retry check</Button>}
          >
            {result.message}
          </Notice>
        ))}

        {monitor.providerUpdateResults.length > 0 && (
          <div className="mb-3 grid min-w-0 max-w-full gap-2">
            {monitor.providerUpdateResults.map((result) => (
              <Notice
                key={`${JSON.stringify([result.executionHostId || '', result.providerId])}:${result.message}`}
                tone={result.status === 'failed' ? 'error' : 'success'}
                title={`${result.name} update ${result.status === 'failed' ? 'failed' : 'installed'}`}
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
          <div role="status" className="uam-np-empty">
            <span className="uam-np-empty__icon" style={{ color: 'var(--text-2)', background: 'var(--surface-high)' }}><RefreshCw size={16} aria-hidden /></span>
            <div className="uam-np-empty__title">Checking for updates…</div>
          </div>
        ) : checkFailure || monitor.hasProviderCheckErrors ? (
          <div className="uam-np-empty">
            <span className="uam-np-empty__icon" style={{ color: 'var(--warning)', background: 'var(--warning-dim)' }}><ArrowUpCircle size={16} aria-hidden /></span>
            <div className="uam-np-empty__title">Could not confirm update status</div>
            <div className="uam-np-empty__text">Try the check again when the update services are reachable.</div>
          </div>
        ) : monitor.hasCatalog ? (
          <div className="uam-np-empty">
            <span className="uam-np-empty__icon"><CheckCircle2 size={16} aria-hidden /></span>
            <div className="uam-np-empty__title">Everything is up to date</div>
            <div className="uam-np-empty__text">UAM and detected provider CLIs have no newer known release.</div>
          </div>
        ) : (
          <div className="uam-np-empty">
            <span className="uam-np-empty__icon" style={{ color: 'var(--text-2)', background: 'var(--surface-high)' }}><ArrowUpCircle size={16} aria-hidden /></span>
            <div className="uam-np-empty__title">Updates have not been checked</div>
            <div className="uam-np-empty__text">Run a check to compare UAM and detected provider versions.</div>
          </div>
        ) : (
          <div className="grid min-w-0 max-w-full gap-3">
            <div className="flex items-center gap-2 px-1">
              <ArrowUpCircle size={16} aria-hidden style={{ color: 'var(--text-3)' }} />
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
                      <div className="uam-np-versions mt-2">
                        <span>Current <span className="uam-np-version">{update.currentVersion}</span></span>
                        <span aria-hidden>→</span>
                        <span>Available <span className="uam-np-version" data-kind="next">{update.latestVersion}</span></span>
                      </div>
                    </div>
                    <IconButton
                      icon={<X size={14} />}
                      label={`Dismiss ${update.name} ${update.latestVersion}`}
                      onClick={() => monitor.dismiss(update.id, update.latestVersion)}
                    />
                  </div>
                  {update.releaseNotesExcerpt?.trim() && (
                    <details key={update.latestVersion} className="uam-update-notes">
                      <summary aria-label={`Release notes excerpt for ${update.name} ${update.latestVersion}`}>
                        Release notes excerpt
                      </summary>
                      <p>{update.releaseNotesExcerpt}</p>
                    </details>
                  )}
                  {Boolean(providerState?.blockingChatIds?.length) && <p role="status" className="text-xs">Waiting for {providerState!.blockingChatIds!.length} provider session(s).</p>}
                  {providerRunning && <p role="status" className="text-xs">{providerState?.message || 'Installing update.'}</p>}
                  <div className="flex flex-wrap gap-2">
                    {__UAM_FEATURE_SSH__ && update.remoteHostId ? (
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
                        loading={providerRunning || pendingInstallIds.has(update.id)}
                        disabled={updatingAll || providerRunning || pendingInstallIds.has(update.id)}
                        onClick={async () => {
                          if (pendingInstalls.current.has(update.id) || batchRef.current) return
                          setInstallError('')
                          if (providerState?.blockingChatIds?.length) { setBlockedUpdate(update); return }
                          pendingInstalls.current.add(update.id)
                          setPendingInstallIds(new Set(pendingInstalls.current))
                          try {
                            const result = await monitor.requestCliProviderUpdate(update.providerId!, update.latestVersion, ...update.executionHostId ? [update.executionHostId] : [])
                            if (!result.ok) setInstallError(`${update.name} update could not be started. ${result.error?.trim() || 'Check its installation and connection, then retry.'}`)
                          } catch {
                            setInstallError(`${update.name} update could not be started. Check its installation and connection, then retry.`)
                          } finally {
                            pendingInstalls.current.delete(update.id)
                            setPendingInstallIds(new Set(pendingInstalls.current))
                          }
                        }}
                      >
                        {providerState?.status === 'checking' ? 'Verifying…' : providerRunning ? 'Updating…' : providerState?.blockingChatIds?.length ? 'Review waiting update' : 'Install update'}
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
                    {update.providerId && !(__UAM_FEATURE_SSH__ && update.remoteHostId) && (
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
          <Button size="sm" variant="ghost" aria-label="Dismiss all updates" onClick={monitor.dismissAll}>Dismiss all</Button>
        </footer>
      )}
    </aside>
  )
}
