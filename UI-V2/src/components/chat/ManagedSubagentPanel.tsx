import { readChatGridLayout } from '../../utils/chatGridStorage'
import { useEffect, useId, useMemo, useRef, useState, useSyncExternalStore } from 'react'
import { Bot, CircleDashed, Square, Users } from 'lucide-react'
import { Button, Tooltip } from '../ui'
import { useAppStore } from '../../store/useAppStore'
import { isCefContext } from '../../ipc/cefBridge'
import { subagentRouteFor, subscribeSubagentNavigation, subagentNavigationRevision, openSubagentView } from '../../utils/subagentNavigation'
import { cancelManagedSubagent, isManagedSubagentTool, listManagedSubagents, managedRunActive, managedRunLabel, managedElapsed, managedTaskTitle, retryManagedRead, type ManagedSubagentPage, type ManagedSubagentRun } from './managedSubagents'
import './ManagedSubagentPanel.css'
import { SubagentCard } from './SubagentCard'

/** `viewportVisible=false` (sidebar rows scrolled out of view) pauses polling and the clock; rows and the last read stay. */
export type ManagedSubagentPanelProps = { sourceChatId: string; runId?: string; catalogue?: boolean; viewportVisible?: boolean; collapsed?: boolean }

/** Message boundaries change for new user/results; streaming text does not rescan history. */
export function managedParentEvidenceSelector(sourceChatId: string) {
  let boundary = ''
  let evidence = '\0'
  return (state: Pick<ReturnType<typeof useAppStore.getState>, 'messages'>) => {
    const messages = state.messages[sourceChatId] || []
    const last = messages[messages.length - 1]
    const nextBoundary = `${messages.length}:${messages[0]?.id || ''}:${last?.id || ''}:${last?.role || ''}`
    if (nextBoundary !== boundary) {
      boundary = nextBoundary
      let userId = ''
      let results = 0
      for (let index = messages.length - 1; index >= 0; --index) {
        const message = messages[index]
        if (!userId && message.role === 'user') userId = message.id
        results += (message.toolCalls || []).filter(isManagedSubagentTool).length
      }
      evidence = `${userId}\0${results}`
    }
    return evidence
  }
}

export function ManagedSubagentPanel(props: ManagedSubagentPanelProps) {
  if (!isCefContext()) return null
  return <ManagedPanelContent key={JSON.stringify([props.sourceChatId, props.runId])} {...props} />
}

export { managedElapsed } from './managedSubagents'

function ManagedPanelContent({ sourceChatId, runId = '', catalogue = false, viewportVisible: inViewport = true, collapsed = false }: ManagedSubagentPanelProps) {
  // A collapsed parent behaves like an off-screen row: no polling, retry or clock; rows stay cached but are stale until one fresh owned read.
  const viewportVisible = inViewport && !collapsed
  const statusIdBase = useId()
  useSyncExternalStore(subscribeSubagentNavigation, subagentNavigationRevision)
  const activeSessionId = useAppStore(state => state.activeSessionId)
  const parent = useAppStore.getState().sessions.find(session => session.id === sourceChatId)
  const route = parent && activeSessionId === parent.id ? subagentRouteFor(readChatGridLayout().activeLeafId, parent) : undefined
  const [page, setPage] = useState<ManagedSubagentPage | null>(null)
  const [offsets, setOffsets] = useState([0])
  const offset = offsets[offsets.length - 1]
  const [attempt, setAttempt] = useState(0)
  const [loading, setLoading] = useState(true)
  const [readPending, setReadPending] = useState(true)
  const [stopping, setStopping] = useState('')
  const [error, setError] = useState('')
  const [stopMessage, setStopMessage] = useState('')
  const [stopFailed, setStopFailed] = useState(false)
  const [failures, setFailures] = useState(0)
  const [now, setNow] = useState(Date.now)
  const [stale, setStale] = useState(false)
  const [open, setOpen] = useState(true)
  const parentActive = useAppStore(state => Boolean(state.acpBindingBySessionId[sourceChatId]?.processing || state.cliBindingBySessionId[sourceChatId]?.processing))
  const managedRevision = useAppStore(state => state.sessions.find(session => session.id === sourceChatId)?.managedSubagentRevision ?? 0)
  const evidenceSelector = useMemo(() => managedParentEvidenceSelector(sourceChatId), [sourceChatId])
  const managedEvidence = useAppStore(evidenceSelector)
  const userEvidence = managedEvidence.split('\0')[0]
  const previousParentActive = useRef(parentActive)
  const previousEvidence = useRef(managedEvidence)
  const previousRevision = useRef(managedRevision)
  const previousUser = useRef(userEvidence)
  const refreshAfterRead = useRef(false)
  const busy = useRef(false)
  const mutation = useRef('')
  const serial = useRef(0)
  const mounted = useRef(true)
  const names = useRef(new Map<string, string>())
  const readController = useRef<AbortController | null>(null)
  const backgroundRead = useRef(false)
  const panelRef = useRef<HTMLElement | null>(null)
  const viewportRef = useRef(viewportVisible)
  viewportRef.current = viewportVisible
  const missedRead = useRef(false)
  // Event-driven reads wait for the viewport; one catch-up read runs when the row returns.
  const requestRead = () => {
    if (!viewportRef.current) { missedRead.current = true; return }
    if (busy.current) refreshAfterRead.current = true
    else { backgroundRead.current = true; setAttempt(value => value + 1) }
  }
  useEffect(() => { mounted.current = true; return () => { mounted.current = false; ++serial.current } }, [])

  useEffect(() => {
    const request = ++serial.current
    const controller = new AbortController()
    readController.current = controller
    let disposed = false
    busy.current = true
    refreshAfterRead.current = false
    setLoading(!backgroundRead.current)
    setReadPending(true)
    setError('')
    void retryManagedRead(() => listManagedSubagents(sourceChatId, runId, offset, 20, controller.signal), controller.signal).then(result => {
      if (disposed || serial.current !== request) return
      setFailures(0)
      setStopFailed(false)
      setPage(result)
      setStale(false)
      result.runs.forEach(run => names.current.set(run.runId, managedTaskTitle(run)))
      setNow(Date.now())
    }).catch(cause => {
      if (!disposed && serial.current === request && !controller.signal.aborted) {
        setFailures(value => value + 1)
        setError(cause instanceof Error ? cause.message : 'Managed subagents are unavailable.')
      }
    }).finally(() => {
      if (!disposed && serial.current === request) {
        busy.current = false
        setLoading(false)
        setReadPending(false)
        if (refreshAfterRead.current) { refreshAfterRead.current = false; requestRead() }
      }
    })
    return () => { disposed = true; controller.abort(); ++serial.current }
  }, [sourceChatId, runId, offset, attempt])

  useEffect(() => {
    if (previousParentActive.current === parentActive && previousEvidence.current === managedEvidence && previousRevision.current === managedRevision) return
    previousParentActive.current = parentActive
    previousEvidence.current = managedEvidence
    previousRevision.current = managedRevision
    // Capture short turns that create and complete children between polling ticks.
    requestRead()
  }, [parentActive, managedEvidence, managedRevision])

  useEffect(() => {
    if (previousUser.current === userEvidence) return
    previousUser.current = userEvidence
    if (!userEvidence) return
    // The optimistic user turn can precede accepted-enqueue revision publication.
    const timer = window.setTimeout(() => {
      requestRead()
    }, 2000)
    return () => window.clearTimeout(timer)
  }, [userEvidence])

  const listedActive = page?.runs.some(run => managedRunActive(run.status, run.lastReported)) ?? false
  const active = parentActive || listedActive
  useEffect(() => {
    if (!active || readPending || stopping || error || !open || !viewportVisible) return
    const timer = window.setTimeout(() => {
      if (document.visibilityState !== 'hidden' && !busy.current) { backgroundRead.current = true; setAttempt(value => value + 1) }
    }, listedActive ? 5000 : 30000)
    const onVisible = () => { if (document.visibilityState === 'visible' && !busy.current) { backgroundRead.current = true; setAttempt(value => value + 1) } }
    document.addEventListener('visibilitychange', onVisible)
    return () => { window.clearTimeout(timer); document.removeEventListener('visibilitychange', onVisible) }
  }, [active, listedActive, readPending, stopping, error, open, viewportVisible, attempt])

  // No manual refresh exists: a failed read is retried with backoff (read-only, never a Stop replay), with a60s cap while visible/eligible.
  useEffect(() => {
    if (!error || readPending || stopping || !open || !viewportVisible) return
    const retry = () => { if (document.visibilityState !== 'hidden' && !busy.current) { backgroundRead.current = true; setAttempt(value => value + 1) } }
    const timer = window.setTimeout(retry, Math.min(60000, 5000 * 2 ** Math.max(0, failures - 1)))
    const onVisible = () => { if (document.visibilityState === 'visible') retry() }
    document.addEventListener('visibilitychange', onVisible)
    return () => { window.clearTimeout(timer); document.removeEventListener('visibilitychange', onVisible) }
  }, [error, failures, readPending, stopping, open, viewportVisible, attempt])

  useEffect(() => {
    if (!listedActive || !open || error || !viewportVisible) return
    const timer = window.setInterval(() => { if (document.visibilityState !== 'hidden') setNow(Date.now()) }, 1000)
    return () => window.clearInterval(timer)
  }, [listedActive, open, error, viewportVisible])

  // Back in view: read once if polling, a retry or an event read was held back while off-screen.
  // Reopening a collapsed parent always reads once, because nothing observed the ledger meanwhile.
  const wasOffscreen = useRef(!viewportVisible)
  const wasCollapsed = useRef(collapsed)
  useEffect(() => {
    if (collapsed && !wasCollapsed.current) { wasCollapsed.current = true; if (!mutation.current) interruptRead(); setStale(true); return }
    if (!viewportVisible) { wasOffscreen.current = true; return }
    const reopened = wasCollapsed.current && !collapsed
    wasCollapsed.current = collapsed
    if (!wasOffscreen.current && !reopened) return
    wasOffscreen.current = false
    if (missedRead.current || active || error || reopened) { missedRead.current = false; requestRead() }
  }, [viewportVisible, collapsed])

  const interruptRead = () => {
    ++serial.current
    readController.current?.abort()
    busy.current = false
    setReadPending(false)
    setLoading(false)
    backgroundRead.current = false
  }

  const stop = async (run: ManagedSubagentRun) => {
    if (!page || mutation.current || error || run.lastReported || !run.canCancel) return
    const row = [...(panelRef.current?.querySelectorAll<HTMLButtonElement>('[data-managed-run]') || [])].find(button => button.dataset.managedRun === run.runId)
    if ((document.activeElement as HTMLElement | null)?.dataset.managedStop === run.runId) row?.focus()
    interruptRead()
    mutation.current = run.runId
    const request = serial.current
    busy.current = true
    setStopping(run.runId)
    setStopMessage('')
    setStopFailed(false)
    try {
      await cancelManagedSubagent(sourceChatId, run.runId, page.controlScopeId)
      if (!mounted.current || serial.current !== request) return
      setStopMessage(`Stop requested for ${managedTaskTitle(run)}. Reading its stored status.`)
      refreshAfterRead.current = true
    } catch (cause) {
      if (mounted.current && serial.current === request) { setStopFailed(true); setError(`${cause instanceof Error ? cause.message : 'Could not stop this child.'} Checking its latest status.`) }
    } finally {
      if (mounted.current && serial.current === request) { busy.current = false; mutation.current = ''; setStopping(''); if (refreshAfterRead.current) { refreshAfterRead.current = false; requestRead() } }
    }
  }

  const openRun = (run: ManagedSubagentRun) => {
    if (!mutation.current) interruptRead()
    const parent = useAppStore.getState().sessions.find(session => session.id === sourceChatId)
    if (parent) { useAppStore.getState().setActiveSession(parent.id); openSubagentView(parent, { kind: 'managed', runId: run.runId, title: managedTaskTitle(run), status: run.lastReported || error || stale ? undefined : run.status, transcriptChatId: run.transcriptChatId || undefined }) }
  }

  if (catalogue) return <section ref={panelRef} tabIndex={-1} className="uam-managed-panel uam-managed-panel--catalogue" aria-label="UAM-managed subagents" aria-busy={stale || undefined} data-stale={stale ? '' : undefined}>
    {!page && !error && <p className="uam-subagent-line__note"><CircleDashed size={12} className="uam-working-ring" aria-hidden />Loading subagents</p>}
    {!page && error && <p role="status" className="uam-subagent-line__note">Subagents unavailable · retrying</p>}
    {stopMessage && <p role="status" className="sr-only">{stopMessage}</p>}
    {page && <>
      <ul aria-label="Managed children">
        {page.runs.map((run, index) => {
          const name = managedTaskTitle(run)
          const current = route?.kind === 'managed' && route.runId === run.runId
          // After a failed read nothing on screen may claim to be live or stoppable.
          const lastReported = Boolean(run.lastReported || error || stale)
          const activeRun = managedRunActive(run.status, lastReported)
          const elapsed = managedElapsed({ ...run, lastReported }, now)
          const statusText = `${managedRunLabel(run.status, lastReported)} · ${elapsed}`
          const trailing = lastReported ? 'Last reported' : activeRun && /^\d/.test(elapsed) ? elapsed : managedRunLabel(run.status)
          const detail = `${name} · ${statusText}\n${[run.agentId, run.providerId, run.modelId, run.host || 'Execution host unavailable'].filter(Boolean).join(' · ')}`
          const statusId = `${statusIdBase}-status-${index}`
          return <li key={run.runId} className="uam-subagent-line" data-nested={run.parentRunId ? '' : undefined}>
            <span id={statusId} className="sr-only">{statusText}</span>
            <Tooltip label={detail}>
              <button type="button" data-managed-run={run.runId} aria-current={current ? 'page' : undefined} aria-label={`Open managed conversation for ${name}`} aria-describedby={statusId}
                className={`uam-sb-row uam-subagent-line__open${current ? ' uam-sb-row--selected' : ''}`} onClick={() => openRun(run)}>
                <Bot size={16} className="shrink-0" aria-hidden />
                <span className="truncate">{name}</span>
                <span className="uam-subagent-line__status" aria-hidden>{activeRun && <CircleDashed size={11} className="uam-working-ring" />}{trailing}</span>
              </button>
            </Tooltip>
            {!lastReported && run.canCancel && <Tooltip label={`Stop ${name}`}><button type="button" className="uam-subagent-line__stop" data-managed-stop={run.runId} aria-label={`Stop managed subagent ${name}`}
              disabled={Boolean(stopping)} onClick={() => void stop(run)}><Square size={11} aria-hidden /></button></Tooltip>}
          </li>
        })}
      </ul>
      {error && <p role="status" className="uam-subagent-line__note">{stopFailed ? 'Stop was not confirmed · checking status' : 'Subagent list may be out of date · retrying'}</p>}
      {(offset > 0 || page.nextOffset !== null) && <div className="uam-subagent-line__pager" role="group" aria-label="Managed subagent pages">
        <Button size="sm" variant="ghost" disabled={offsets.length === 1 || Boolean(stopping)} onClick={() => { interruptRead(); setOffsets(value => value.slice(0, -1)) }}>Previous children</Button>
        <span role="status">{page.runs.length ? `${page.offset + 1}–${page.offset + page.runs.length}` : '0'} of {page.totalCount}</span>
        <Button size="sm" variant="ghost" disabled={page.nextOffset === null || Boolean(stopping)} onClick={() => { if (page.nextOffset !== null) { interruptRead(); if (offset === page.nextOffset) setAttempt(value => value + 1); else setOffsets(value => [...value, page.nextOffset!]) } }}>More children</Button>
      </div>}
    </>}
  </section>
  if (page?.totalCount === 0 && !error && !loading) return null
  return <section ref={panelRef} tabIndex={-1} className="uam-managed-panel" aria-label="UAM-managed subagents">
    <details open={open} onToggle={event => setOpen(event.currentTarget.open)}>
      <summary><Users size={15} aria-hidden /><span>Managed subagents{page ? ` (${page.totalCount})` : ''}</span></summary>
      {open && <>
        {loading && !page && <p role="status">Loading managed subagents…</p>}
        {error && <p role={page?.totalCount || runId ? 'alert' : undefined} className={page?.totalCount || runId ? 'uam-managed-panel__error' : 'uam-managed-panel__meta'}>{error}{page ? ' Previously loaded rows remain visible.' : ''}</p>}
        {stopMessage && <p role="status" className="uam-managed-panel__meta">{stopMessage}</p>}
        {page && <>
          {page.runs.length === 0 && <p role="status">No managed subagents on this page.</p>}
          <ul aria-label="Managed children">
            {page.runs.map(run => <li key={run.runId}>
              <div className="uam-managed-panel__row">
                <SubagentCard aria-current={route?.kind === 'managed' && route.runId === run.runId ? 'page' : undefined} data-managed-run={run.runId}
                  title={managedTaskTitle(run)} providerId={run.providerId} summary={run.summary} status={managedRunLabel(run.status, run.lastReported || Boolean(error))} working={managedRunActive(run.status, run.lastReported || Boolean(error))} elapsed={managedElapsed({ ...run, lastReported: run.lastReported || Boolean(error) }, now)}
                  aria-label={`Open managed conversation for ${managedTaskTitle(run)}`} onClick={() => openRun(run)} />
                {!run.lastReported && run.canCancel && <Button data-managed-stop={run.runId} variant="ghost" size="sm" leadingIcon={<Square size={12} aria-hidden />} aria-label={`Stop managed subagent ${managedTaskTitle(run)}`} loading={stopping === run.runId} disabled={Boolean(stopping) || Boolean(error)} onClick={() => void stop(run)}>Stop</Button>}
              </div>
              <details className="uam-managed-panel__details">
                <summary aria-label={`Details for ${managedTaskTitle(run)}`}>Details</summary>
                <ManagedRunDetails run={run} runs={page.runs} names={names.current} />
              </details>
              {!run.canCancel && managedRunActive(run.status, run.lastReported) && <p className="uam-managed-panel__meta">Stop is unavailable for this run.</p>}
            </li>)}
          </ul>
          {(offset > 0 || page.nextOffset !== null) && <div className="uam-managed-panel__toolbar" role="group" aria-label="Managed subagent pages">
            <Button size="sm" variant="ghost" disabled={offsets.length === 1 || Boolean(stopping)} onClick={() => { interruptRead(); setOffsets(value => value.slice(0, -1)) }}>Previous children</Button>
            <span role="status" className="uam-managed-panel__meta">{page.runs.length ? `${page.offset + 1}–${page.offset + page.runs.length}` : '0'} of {page.totalCount}</span>
            <Button size="sm" variant="ghost" disabled={page.nextOffset === null || Boolean(stopping)} onClick={() => { if (page.nextOffset !== null) { interruptRead(); if (offset === page.nextOffset) setAttempt(value => value + 1); else setOffsets(value => [...value, page.nextOffset!]) } }}>More children</Button>
          </div>}
        </>}
      </>}
    </details>
  </section>
}


function ManagedRunDetails({ run, runs, names }: { run: ManagedSubagentRun; runs: ManagedSubagentRun[]; names: Map<string, string> }) {
  return <>
    <p className="uam-managed-panel__meta">{run.parentRunId ? `Subagent of ${names.get(run.parentRunId) || 'another managed run'}` : 'Subagent of this parent chat'} · {[run.agentId, run.providerId, run.modelId, run.host || 'Execution host unavailable'].filter(Boolean).join(' · ')}</p>
    {run.parentRunId && <p className="uam-managed-panel__identity">Parent run: {run.parentRunId}</p>}
  </>
}
