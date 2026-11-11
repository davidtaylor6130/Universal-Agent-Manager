import { ArrowUpCircle, ArrowUpRight, Check, CheckCircle2, CircleAlert, Copy, Download, Loader2, RefreshCw, RotateCcw, TriangleAlert, X } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import type { UpdateMonitor } from '../../hooks/useUpdateMonitor'
import { ProviderLogo } from '../shared/ProviderLogo'
import { Button, ConfirmDialog, IconButton, ProgressFill, cx } from '../ui'
import { reportAppFailure } from '../../utils/appFailure'
import { Notice } from '../ui/Notice'
import { useRowLeave } from './useRowLeave'
import './panels.css'

// The build define is used directly here: this chunk imports buildFeatures through a chunk cycle, which stops
// the bundler from folding the imported constant and would ship the SSH helper branches.

type UpdateItem = UpdateMonitor['updates'][number]
type RowOutcome = { tone: 'error' | 'success'; output?: string }
type UpdateResult = UpdateMonitor['providerUpdateResults'][number]

const FAILURE_MS = 15_000
const SUCCESS_MS = 1_500
// Module-level so a remounted panel does not re-alert the same persisting monitor error.
let reportedMonitorError = ''

export function UpdatesPanel({ monitor, onClose, closing = false }: { monitor: UpdateMonitor; onClose: () => void; closing?: boolean }) {
  const [installError, setInstallError] = useState('')
  const [checkError, setCheckError] = useState('')
  const [outcomes, setOutcomes] = useState<Record<string, RowOutcome>>({})
  const [doneIds, setDoneIds] = useState<ReadonlySet<string>>(() => new Set())
  const [updatingAll, setUpdatingAll] = useState(false)
  // Failed results with no matching update row: they stay until dismissed.
  const [orphans, setOrphans] = useState<{ key: string; result: UpdateResult }[]>([])
  const [blockedUpdate, setBlockedUpdate] = useState<UpdateItem | null>(null)
  const batchRef = useRef<AbortController | null>(null)
  const pendingInstalls = useRef(new Set<string>())
  const [pendingInstallIds, setPendingInstallIds] = useState<Set<string>>(() => new Set())
  const timers = useRef(new Map<string, number>())
  const seenResults = useRef<Set<string> | null>(null)
  const rows = useRowLeave()
  useEffect(() => () => {
    batchRef.current?.abort()
    timers.current.forEach((timer) => window.clearTimeout(timer))
  }, [])

  const checkFailure = monitor.error || checkError
  // Update check and install failures go to the shared alert path; the panel's Check again and update buttons are the retry.
  // The monitor's error persists across panel mounts; alert once per distinct error, not on every remount.
  useEffect(() => {
    if (!monitor.error) { reportedMonitorError = ''; return }
    if (monitor.error === reportedMonitorError) return
    reportedMonitorError = monitor.error
    reportAppFailure('Update check failed', monitor.error)
  }, [monitor.error])
  useEffect(() => {
    if (!checkError) return
    reportAppFailure('Update check failed', checkError)
    setCheckError('')
  }, [checkError])
  useEffect(() => {
    if (!installError) return
    reportAppFailure('Update failed', installError)
    setInstallError('')
  }, [installError])
  const installableUpdates = monitor.updates.filter((update) => (__UAM_FEATURE_SSH__ && update.remoteHostId) || (update.providerId && update.installable))
  const checked = monitor.lastCheckedAt ? new Date(monitor.lastCheckedAt) : null
  const checkedTime = checked && !Number.isNaN(checked.getTime()) ? checked.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }) : ''

  const findState = (update: UpdateItem) => monitor.providerStates.find((state) => state.providerId === update.providerId && (state.executionHostId || '') === (update.executionHostId || ''))
  const findUpdate = (result: UpdateResult) => monitor.updates.find((update) => update.providerId === result.providerId && (update.executionHostId || '') === (result.executionHostId || ''))

  // Row outcome: a failed install tints the row for FAILURE_MS, then the row is normal and retryable again.
  const clearOutcome = (id: string) => {
    window.clearTimeout(timers.current.get(id))
    timers.current.delete(id)
    setOutcomes((current) => {
      if (!(id in current)) return current
      const next = { ...current }
      delete next[id]
      return next
    })
  }
  const settleAfter = (id: string, ms: number, run: () => void) => {
    window.clearTimeout(timers.current.get(id))
    timers.current.set(id, window.setTimeout(() => { timers.current.delete(id); run() }, ms))
  }
  const failRow = (update: UpdateItem, reason: string, output?: string) => {
    reportAppFailure(`${update.name} update failed`, reason)
    setOutcomes((current) => ({ ...current, [update.id]: { tone: 'error', output: output || reason } }))
    settleAfter(update.id, FAILURE_MS, () => clearOutcome(update.id))
  }

  // Provider results arrive asynchronously from the monitor: a new result drives its row (the first snapshot is history, not a new event).
  useEffect(() => {
    const initial = seenResults.current === null
    const seen = seenResults.current ?? new Set<string>()
    seenResults.current = seen
    for (const result of monitor.providerUpdateResults) {
      const key = JSON.stringify([result.executionHostId || '', result.providerId, result.status, result.message])
      if (seen.has(key)) continue
      seen.add(key)
      const update = findUpdate(result)
      // Failures from before mount still count; successes from before mount are history.
      if (result.status === 'failed') {
        if (update) failRow(update, result.message, result.output)
        else setOrphans((current) => [...current, { key, result }])
        continue
      }
      if (initial || !update) continue
      setOutcomes((current) => ({ ...current, [update.id]: { tone: 'success' } }))
      settleAfter(update.id, SUCCESS_MS, () => rows.leave([update.id], () => {
        setDoneIds((current) => new Set(current).add(update.id))
        clearOutcome(update.id)
      }))
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [monitor.providerUpdateResults])

  async function installProvider(update: UpdateItem) {
    if (pendingInstalls.current.has(update.id) || batchRef.current) return
    setInstallError('')
    const providerState = findState(update)
    if (providerState?.blockingChatIds?.length) { setBlockedUpdate(update); return }
    pendingInstalls.current.add(update.id)
    setPendingInstallIds(new Set(pendingInstalls.current))
    clearOutcome(update.id)
    try {
      const result = await monitor.requestCliProviderUpdate(update.providerId!, update.latestVersion, ...update.executionHostId ? [update.executionHostId] : [])
      if (!result.ok) failRow(update, result.error?.trim() || 'Check its installation and connection, then retry.', result.error)
    } catch {
      failRow(update, 'Check its installation and connection, then retry.')
    } finally {
      pendingInstalls.current.delete(update.id)
      setPendingInstallIds(new Set(pendingInstalls.current))
    }
  }

  async function installHelper(update: UpdateItem) {
    if (!__UAM_FEATURE_SSH__) return
    setInstallError('')
    clearOutcome(update.id)
    const result = await monitor.applyRemoteHelperUpdate(update.remoteHostId!)
    if (!result.ok) failRow(update, result.error?.trim() || 'Check its SSH connection and bundled helper in Remote Hosts.', result.error)
  }

  return (
    <aside
      aria-label="Updates"
      data-testid="updates-panel"
      data-state={closing ? 'closed' : 'open'}
      className="uam-shell-panel uam-shell-panel--right flex h-full w-[360px] max-w-full shrink-0 flex-col overflow-hidden"
      style={{ background: 'var(--surface)', color: 'var(--text)', borderLeft: '1px solid var(--border)', '--panel-w': '360px' } as React.CSSProperties}
    >
      <header className="flex shrink-0 items-center gap-2 px-4" style={{ height: 'var(--pane-header-h)', borderBottom: '1px solid var(--border)' }}>
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

      <div className="min-w-0 flex-1 overflow-y-auto px-4 pb-4 pt-2">
        <ConfirmDialog
          open={blockedUpdate !== null}
          title={`${blockedUpdate?.name ?? 'Provider'} sessions are running`}
          confirmLabel="Stop session and install update"
          onCancel={() => setBlockedUpdate(null)}
          onConfirm={async () => {
            if (!blockedUpdate) return
            const result = await monitor.stopSessionsAndUpdate(blockedUpdate.providerId!, blockedUpdate.latestVersion, blockedUpdate.executionHostId)
            if (result.ok) setBlockedUpdate(null)
            else setInstallError(result.error || 'Could not stop the provider sessions. The update was not installed.')
          }}
        >
          Only this provider's sessions on this machine will stop. Current work will end.
        </ConfirmDialog>

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

        {orphans.length > 0 && (
          <div className="mb-2 grid min-w-0 max-w-full gap-2">
            {orphans.map(({ key, result }) => {
              const target = findUpdate(result)
              const helper = Boolean(__UAM_FEATURE_SSH__ && target?.remoteHostId)
              return (
                <article key={key} className="uam-update-row uam-update-row--error flex min-w-0 max-w-full items-center gap-1.5 rounded-lg px-2 py-1.5">
                  <CircleAlert size={14} aria-hidden className="shrink-0" style={{ color: 'var(--error)' }} />
                  <span className="min-w-0 flex-1 truncate text-sm font-medium" style={{ color: 'var(--error)' }}>{`${result.name} update failed`}</span>
                  <IconButton size="sm" icon={<Copy size={14} />} label={`Copy ${result.name} update logs`} onClick={() => { void navigator.clipboard?.writeText(result.output || result.message) }} />
                  {target && (
                    <IconButton size="sm" icon={<RotateCcw size={14} />} label={`Retry ${result.name} update to ${target.latestVersion}`} onClick={() => (helper ? installHelper(target) : installProvider(target))} />
                  )}
                  <IconButton size="sm" icon={<X size={14} />} label={`Dismiss ${result.name} update failure`} onClick={() => setOrphans((current) => current.filter((orphan) => orphan.key !== key))} />
                </article>
              )
            })}
          </div>
        )}
        {monitor.updates.length === 0 && orphans.length === 0 ? monitor.checking ? (
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
            {monitor.updates.filter((update) => !doneIds.has(update.id)).map((update) => {
              const providerState = findState(update)
              const helper = Boolean(__UAM_FEATURE_SSH__ && update.remoteHostId)
              const providerInstallable = !helper && Boolean(update.providerId && update.installable)
              const outcome = outcomes[update.id]
              const succeeded = outcome?.tone === 'success'
              const failed = outcome?.tone === 'error'
              const installing = !failed && !succeeded && (Boolean(providerState?.running) || pendingInstallIds.has(update.id) || (__UAM_FEATURE_SSH__ && helper && monitor.remoteHelperUpdatingId === update.remoteHostId))
              const waiting = providerState?.blockingChatIds?.length ?? 0
              const excerpt = update.releaseNotesExcerpt?.trim()
              const install = __UAM_FEATURE_SSH__ && helper ? () => installHelper(update) : () => installProvider(update)
              const leaving = rows.leaving.has(update.id)
              const dismiss = () => rows.leave([update.id], () => monitor.dismiss(update.id, update.latestVersion))
              const rowClass = cx('uam-update-row flex min-w-0 max-w-full items-center gap-1.5 rounded-lg px-2 py-1.5', failed && 'uam-update-row--error')
              const content = failed ? (
                <>
                  <CircleAlert size={14} aria-hidden className="shrink-0" style={{ color: 'var(--error)' }} />
                  <span className="min-w-0 flex-1 truncate text-sm font-medium" style={{ color: 'var(--error)' }}>{`${update.name} update failed`}</span>
                  {outcome.output && (
                    <IconButton size="sm" icon={<Copy size={14} />} label={`Copy ${update.name} update logs`} onClick={() => { void navigator.clipboard?.writeText(outcome.output ?? '') }} />
                  )}
                  <IconButton size="sm" icon={<RotateCcw size={14} />} label={`Retry ${update.name} update to ${update.latestVersion}`} onClick={install} />
                  <IconButton size="sm" icon={<X size={14} />} label={`Dismiss ${update.name} ${update.latestVersion}`} onClick={dismiss} />
                </>
              ) : (
                <>
                  {succeeded ? <Check size={14} aria-hidden className="shrink-0" style={{ color: 'var(--success)' }} />
                    : update.providerId ? <ProviderLogo providerId={update.providerId} size={14} className="shrink-0" /> : <ArrowUpCircle size={14} aria-hidden className="shrink-0" style={{ color: 'var(--text-3)' }} />}
                  <span className="min-w-0 flex-1 truncate text-sm font-medium" style={{ color: 'var(--text)' }}>{update.name}</span>
                  {!succeeded && <span className="shrink-0 font-mono text-xs" style={{ color: 'var(--text-3)' }}>
                    {update.currentVersion} <span aria-hidden>→</span> <span data-kind="next" style={{ color: 'var(--accent)' }}>{update.latestDisplay ?? update.latestVersion}</span>
                  </span>}
                  {update.url && !succeeded && (
                    <IconButton
                      size="sm"
                      icon={<ArrowUpRight size={14} />}
                      label={`Release notes for ${update.name} ${update.latestVersion}`}
                      tooltip={excerpt && excerpt.length <= 160 ? excerpt : undefined}
                      onClick={() => window.open(update.url, '_blank', 'noopener')}
                    />
                  )}
                  {installing ? (
                    <Loader2 size={14} aria-hidden className="shrink-0 animate-spin" style={{ color: 'var(--accent)' }} />
                  ) : !succeeded && (helper || providerInstallable) && (
                    <IconButton
                      size="sm"
                      icon={<Download size={14} />}
                      label={`Update ${update.name} to ${update.latestVersion}`}
                      tooltip={waiting ? `Waiting for ${waiting} provider session(s)` : undefined}
                      disabled={updatingAll}
                      onClick={install}
                    />
                  )}
                  {!succeeded && <IconButton size="sm" icon={<X size={14} />} label={`Dismiss ${update.name} ${update.latestVersion}`} onClick={dismiss} />}
                </>
              )
              return (
                <div key={update.id} className="uam-np-collapse" data-state={leaving ? 'closed' : 'open'}>
                  {installing || succeeded ? (
                    <ProgressFill
                      label={succeeded ? `${update.name} installed` : `Updating ${update.name}`}
                      indeterminate={!succeeded}
                      value={succeeded ? 100 : undefined}
                      className={cx(rowClass, succeeded && 'uam-update-row--success')}
                    >
                      {content}
                    </ProgressFill>
                  ) : (
                    <article
                      data-update-available="true"
                      className={rowClass}
                    >
                      {content}
                    </article>
                  )}
                </div>
              )
            })}
            <div className="mt-1 grid grid-cols-2 gap-2">
              <Button
                size="sm"
                variant="secondary"
                className="w-full"
                leadingIcon={<Download size={14} aria-hidden />}
                aria-label="Update everything"
                loading={updatingAll}
                disabled={installableUpdates.length === 0 || updatingAll || pendingInstallIds.size > 0 || monitor.providerTaskRunning || (__UAM_FEATURE_SSH__ && Boolean(monitor.remoteHelperUpdatingId))}
                onClick={async () => {
                  if (batchRef.current || pendingInstalls.current.size > 0) return
                  const batch = new AbortController()
                  batchRef.current = batch
                  setUpdatingAll(true)
                  setInstallError('')
                  try {
                    const providers = installableUpdates.filter((update): update is typeof update & { providerId: string } => Boolean(update.providerId && !(__UAM_FEATURE_SSH__ && update.remoteHostId)))
                    const helpers = __UAM_FEATURE_SSH__ ? installableUpdates.filter((update) => update.remoteHostId) : []
                    const results = await Promise.allSettled([
                      ...(providers.length ? [monitor.installProviderUpdates(providers)] : []),
                      ...(__UAM_FEATURE_SSH__ ? helpers.map((update) => monitor.applyRemoteHelperUpdate(update.remoteHostId!)) : []),
                    ])
                    if (batch.signal.aborted) return
                    results.forEach((result, index) => {
                      if (result.status === 'fulfilled' && result.value.ok) return
                      const reason = result.status === 'fulfilled' ? result.value.error?.trim() : ''
                      const targets = providers.length && index === 0 ? providers : helpers.slice(providers.length ? index - 1 : index, providers.length ? index : index + 1)
                      targets.forEach((update) => failRow(update, reason || 'Check its update status and retry.', reason))
                    })
                  } catch {
                    if (!batch.signal.aborted) installableUpdates.forEach((update) => failRow(update, 'Check each provider status and retry.'))
                  } finally {
                    batchRef.current = null
                    if (!batch.signal.aborted) setUpdatingAll(false)
                  }
                }}
              >
                {updatingAll ? 'Updating…' : 'Update everything'}
              </Button>
              <Button size="sm" variant="secondary" className="w-full" aria-label="Dismiss all updates" onClick={() => rows.leave(monitor.updates.map((update) => update.id), monitor.dismissAll)}>Dismiss all</Button>
            </div>
          </div>
        )}
      </div>
    </aside>
  )
}
