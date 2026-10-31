import { ArrowUpCircle, CheckCircle2, Download, ExternalLink, Loader2, RefreshCw, RotateCcw, TriangleAlert, X } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import type { UpdateMonitor } from '../../hooks/useUpdateMonitor'
import { ProviderLogo } from '../shared/ProviderLogo'
import { Button, IconButton, Tooltip } from '../ui'
import { Notice } from '../ui/Notice'
import './panels.css'
import './update-release-notes.css'

// The build define is used directly here: this chunk imports buildFeatures through a chunk cycle, which stops
// the bundler from folding the imported constant and would ship the SSH helper branches.

type UpdateItem = UpdateMonitor['updates'][number]

export function UpdatesPanel({ monitor, onClose, closing = false }: { monitor: UpdateMonitor; onClose: () => void; closing?: boolean }) {
  const [installError, setInstallError] = useState('')
  const [checkError, setCheckError] = useState('')
  const [rowErrors, setRowErrors] = useState<Record<string, string>>({})
  const [updatingAll, setUpdatingAll] = useState(false)
  const [blockedUpdate, setBlockedUpdate] = useState<UpdateItem | null>(null)
  const batchRef = useRef<AbortController | null>(null)
  const pendingInstalls = useRef(new Set<string>())
  const [pendingInstallIds, setPendingInstallIds] = useState<Set<string>>(() => new Set())
  useEffect(() => () => batchRef.current?.abort(), [])

  const checkFailure = monitor.error || checkError
  const installableUpdates = monitor.updates.filter((update) => (__UAM_FEATURE_SSH__ && update.remoteHostId) || (update.providerId && update.installable))
  const checked = monitor.lastCheckedAt ? new Date(monitor.lastCheckedAt) : null
  const checkedTime = checked && !Number.isNaN(checked.getTime()) ? checked.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }) : ''

  const findState = (update: UpdateItem) => monitor.providerStates.find((state) => state.providerId === update.providerId && (state.executionHostId || '') === (update.executionHostId || ''))
  const clearRowError = (id: string) => setRowErrors((current) => {
    const next = { ...current }
    delete next[id]
    return next
  })
  const failRow = (update: UpdateItem, message: string) => {
    setInstallError(message)
    setRowErrors((current) => ({ ...current, [update.id]: message }))
  }

  async function installProvider(update: UpdateItem) {
    if (pendingInstalls.current.has(update.id) || batchRef.current) return
    setInstallError('')
    const providerState = findState(update)
    if (providerState?.blockingChatIds?.length) { setBlockedUpdate(update); return }
    pendingInstalls.current.add(update.id)
    setPendingInstallIds(new Set(pendingInstalls.current))
    clearRowError(update.id)
    try {
      const result = await monitor.requestCliProviderUpdate(update.providerId!, update.latestVersion, ...update.executionHostId ? [update.executionHostId] : [])
      if (!result.ok) failRow(update, `${update.name} update could not be started. ${result.error?.trim() || 'Check its installation and connection, then retry.'}`)
    } catch {
      failRow(update, `${update.name} update could not be started. Check its installation and connection, then retry.`)
    } finally {
      pendingInstalls.current.delete(update.id)
      setPendingInstallIds(new Set(pendingInstalls.current))
    }
  }

  async function installHelper(update: UpdateItem) {
    setInstallError('')
    clearRowError(update.id)
    const result = await monitor.applyRemoteHelperUpdate(update.remoteHostId!)
    if (!result.ok) failRow(update, `${update.name} update failed. ${result.error?.trim() || 'Check its SSH connection and bundled helper in Remote Hosts.'}`)
  }

  return (
    <aside
      aria-label="Updates"
      data-testid="updates-panel"
      data-state={closing ? 'closed' : 'open'}
      className="uam-shell-panel uam-shell-panel--right flex h-full w-[360px] max-w-full shrink-0 flex-col overflow-hidden"
      style={{ background: 'var(--surface)', color: 'var(--text)', borderLeft: '1px solid var(--border)', '--panel-w': '360px' } as React.CSSProperties}
    >
      <header className="flex shrink-0 items-center gap-2 px-4 py-2" style={{ borderBottom: '1px solid var(--border)' }}>
        <div className="flex min-w-0 flex-1 items-center gap-2 text-sm font-semibold" style={{ color: 'var(--text)' }}>
          <ArrowUpCircle size={16} aria-hidden />
          <span>Updates</span>
          {checkedTime && <span className="text-[11px] font-normal" style={{ color: 'var(--text-3)' }}>{checkedTime}</span>}
        </div>
        <IconButton
          size="sm"
          icon={<RefreshCw size={14} className={monitor.checking ? 'animate-spin' : undefined} aria-hidden />}
          label="Check again"
          aria-busy={monitor.checking}
          disabled={monitor.checking || updatingAll}
          onClick={() => { setCheckError(''); void monitor.checkNow() }}
        />
        <IconButton icon={<X size={16} />} label="Close updates" onClick={onClose} />
      </header>

      <div className="min-w-0 flex-1 overflow-y-auto p-4">
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
            <span className="uam-np-empty__icon" style={{ color: 'var(--warning)', background: 'var(--warning-dim)' }}><TriangleAlert size={16} aria-hidden /></span>
            <div className="uam-np-empty__title">Could not confirm update status</div>
            <div className="uam-np-empty__text">Try the check again when the update services are reachable.</div>
          </div>
        ) : monitor.hasCatalog ? (
          <div className="uam-np-empty">
            <span className="uam-np-empty__icon"><CheckCircle2 size={16} aria-hidden /></span>
            <div className="uam-np-empty__title">Everything is up to date</div>
          </div>
        ) : (
          <div className="uam-np-empty">
            <span className="uam-np-empty__icon" style={{ color: 'var(--text-2)', background: 'var(--surface-high)' }}><ArrowUpCircle size={16} aria-hidden /></span>
            <div className="uam-np-empty__title">Updates have not been checked</div>
            <div className="uam-np-empty__text">Run a check to compare UAM and detected provider versions.</div>
          </div>
        ) : (
          <div className="grid min-w-0 max-w-full gap-2">
            <div className="grid grid-cols-2 gap-2">
              <Button
                size="sm"
                variant="secondary"
                className="w-full"
                leadingIcon={<Download size={14} aria-hidden />}
                aria-label="Update everything"
                loading={updatingAll}
                disabled={installableUpdates.length === 0 || updatingAll || pendingInstallIds.size > 0 || monitor.providerTaskRunning || Boolean(monitor.remoteHelperUpdatingId)}
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
              <Button size="sm" variant="ghost" className="w-full" aria-label="Dismiss all updates" onClick={monitor.dismissAll}>Dismiss all</Button>
            </div>
            {monitor.updates.map((update) => {
              const providerState = findState(update)
              const helper = Boolean(__UAM_FEATURE_SSH__ && update.remoteHostId)
              const providerInstallable = !helper && Boolean(update.providerId && update.installable)
              const installing = Boolean(providerState?.running) || pendingInstallIds.has(update.id) || (helper && monitor.remoteHelperUpdatingId === update.remoteHostId)
              const failure = rowErrors[update.id]
              const waiting = providerState?.blockingChatIds?.length ?? 0
              const excerpt = update.releaseNotesExcerpt?.trim()
              const install = helper ? () => installHelper(update) : () => installProvider(update)
              return (
                <article
                  key={update.id}
                  data-update-available="true"
                  data-installing={installing || undefined}
                  aria-busy={installing || undefined}
                  className="uam-update-row flex min-w-0 max-w-full items-center gap-1.5 rounded-lg px-2 py-1.5"
                  style={{ background: installing ? 'var(--accent-dim)' : 'var(--surface-up)', border: '1px solid var(--border)' }}
                >
                  {installing && <span aria-hidden className="uam-update-row__fill" />}
                  {update.providerId ? <ProviderLogo providerId={update.providerId} size={14} className="shrink-0" /> : <ArrowUpCircle size={14} aria-hidden className="shrink-0" style={{ color: 'var(--text-3)' }} />}
                  <span className="min-w-0 flex-1 truncate text-sm font-medium" style={{ color: 'var(--text)' }}>{update.name}</span>
                  <span className="shrink-0 font-mono text-xs" style={{ color: 'var(--text-3)' }}>
                    {update.currentVersion} <span aria-hidden>→</span> <span data-kind="next" style={{ color: 'var(--accent)' }}>{update.latestVersion}</span>
                  </span>
                  {update.url && (
                    <IconButton
                      size="sm"
                      icon={<ExternalLink size={14} />}
                      label={`Release notes for ${update.name} ${update.latestVersion}`}
                      tooltip={excerpt && excerpt.length <= 160 ? excerpt : undefined}
                      onClick={() => window.open(update.url, '_blank', 'noopener')}
                    />
                  )}
                  {installing ? (
                    <Loader2 size={14} aria-hidden className="shrink-0 animate-spin" style={{ color: 'var(--accent)' }} />
                  ) : failure ? (
                    <>
                      <Tooltip label={failure}>
                        <span role="img" tabIndex={0} aria-label={`${update.name} update failed`} className="grid shrink-0 place-items-center" style={{ color: 'var(--error)' }}>
                          <X size={14} aria-hidden />
                        </span>
                      </Tooltip>
                      <IconButton size="sm" icon={<RotateCcw size={14} />} label={`Retry ${update.name} update to ${update.latestVersion}`} onClick={install} />
                    </>
                  ) : (helper || providerInstallable) && (
                    <IconButton
                      size="sm"
                      icon={<Download size={14} />}
                      label={`Update ${update.name} to ${update.latestVersion}`}
                      tooltip={waiting ? `Waiting for ${waiting} provider session(s)` : undefined}
                      disabled={updatingAll}
                      onClick={install}
                    />
                  )}
                  <IconButton size="sm" icon={<X size={14} />} label={`Dismiss ${update.name} ${update.latestVersion}`} onClick={() => monitor.dismiss(update.id, update.latestVersion)} />
                </article>
              )
            })}
          </div>
        )}
      </div>
    </aside>
  )
}
