import { sanitizeManagedOriginRunId } from '../../store/cpp/sanitizers'
import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from 'react'
import { ChevronRight, Square, Users } from 'lucide-react'
import { isCefContext } from '../../ipc/cefBridge'
import { SubagentCard } from './SubagentCard'
import { cancelManagedSubagent, listManagedSubagents, retryManagedRead, managedTaskTitle, managedRunActive, managedRunLabel, managedElapsed, type ManagedSubagentRun } from './managedSubagents'
import { useAppStore, type AcpToolCall } from '../../store/useAppStore'
import { Button } from '../ui'
import { managedLaunchRunId } from './inlineSubagents'
import './InlineSubagentGroup.css'

type Run = ManagedSubagentRun & { controlScopeId: string }
type Feed = { runs: Map<string, Run>; loading: boolean; error: boolean; readError: boolean; stopping: string; feedback: string; now: number; register: (ids: string[]) => () => void; stop: (id: string) => Promise<void>; open?: (id: string) => void }
const Context = createContext<Feed | null>(null)
const active = (run: Run) => managedRunActive(run.status, run.lastReported)
const firstLevelRun = (run: Run | undefined) => run?.parentRunId === '' ? run : undefined

/** One scoped metadata reader per parent; cards never start or retry provider work. */
export function InlineSubagentProvider({ sourceChatId, onOpenManagedRun, children }: { sourceChatId: string; onOpenManagedRun?: (id: string) => void; children: ReactNode }) {
  const registered = useRef(new Map<string, number>())
  const [membership, setMembership] = useState(0)
  const [attempt, setAttempt] = useState(0)
  const [runs, setRuns] = useState(new Map<string, Run>())
  const [error, setError] = useState(false)
  const [readError, setReadError] = useState(false)
  const [readingSourceChatId, setReadingSourceChatId] = useState('')
  const [stopping, setStopping] = useState('')
  const [feedback, setFeedback] = useState('')
  const [now, setNow] = useState(Date.now)
  const mutating = useRef(false)
  const generation = useRef(0)
  const [failures, setFailures] = useState(0)
  const revision = useAppStore(state => (state.sessions.find(session => session.id === sourceChatId) as { managedSubagentRevision?: number } | undefined)?.managedSubagentRevision ?? 0)
  const register = useCallback((ids: string[]) => {
    ids.forEach(id => registered.current.set(id, (registered.current.get(id) ?? 0) + 1)); setMembership(n => n + 1)
    return () => { ids.forEach(id => { const left = (registered.current.get(id) ?? 1) - 1; if (left) registered.current.set(id, left); else registered.current.delete(id) }); setMembership(n => n + 1) }
  }, [])
  const refresh = useCallback(() => setAttempt(n => n + 1), [])
  const expectedKey = useMemo(() => [...registered.current.keys()].sort().join('\0'), [membership])
  useEffect(() => {
    const expected = new Set(expectedKey ? expectedKey.split('\0') : [])
    if (!isCefContext() || !expected.size) return
    setReadingSourceChatId(sourceChatId); setError(false)
    const readGeneration = ++generation.current
    let cancelled = false
    const controller = new AbortController()
    void (async () => {
      const next = new Map<string, Run>()
      let offset = 0
      let scope: string | undefined
      let total: number | undefined
      do {
        const page = await retryManagedRead(() => listManagedSubagents(sourceChatId, '', offset, 100, controller.signal), controller.signal)
        if (cancelled || readGeneration !== generation.current) return
        if (scope !== undefined && (scope !== page.controlScopeId || total !== page.totalCount)) throw new Error('Listing changed')
        scope = page.controlScopeId; total = page.totalCount
        for (const run of page.runs) {
          if (!expected.has(run.runId)) continue
          if (next.has(run.runId)) throw new Error('Repeated run')
          next.set(run.runId, { ...run, controlScopeId: page.controlScopeId })
        }
        if (page.nextOffset === null || next.size === expected.size) break
        if (!Number.isSafeInteger(page.nextOffset) || page.nextOffset <= offset || page.nextOffset >= page.totalCount) throw new Error('Invalid page')
        offset = page.nextOffset
      } while (offset < 10000)
      if (!cancelled && readGeneration === generation.current) { setFailures(0); setReadError(false); setFeedback(''); setRuns(next); setNow(Date.now()); setError(false); setReadingSourceChatId('') }
    })().catch(() => { if (!cancelled && readGeneration === generation.current) { setFailures(value => value + 1); setReadError(true); setError(true); setReadingSourceChatId(''); setRuns(previous => new Map([...previous].map(([id, run]) => [id, { ...run, lastReported: true, canCancel: false }]))) } })
    return () => { cancelled = true; controller.abort(); ++generation.current }
  }, [sourceChatId, expectedKey, revision, attempt])
  useEffect(() => {
    if (!registered.current.size || error || stopping || ![...registered.current.keys()].some(id => runs.has(id) && active(runs.get(id)!))) return
    const timer = window.setTimeout(() => { if (document.visibilityState !== 'hidden') refresh() }, 5000)
    return () => window.clearTimeout(timer)
  }, [runs, error, stopping, membership, attempt, refresh])
  // No manual refresh: a failed read is retried with capped backoff; Stop is never replayed, only the read.
  useEffect(() => {
    if (!error || stopping || !registered.current.size) return
    const timer = window.setTimeout(() => { if (document.visibilityState !== 'hidden') refresh() }, Math.min(60000, 5000 * 2 ** Math.max(0, failures - 1)))
    return () => window.clearTimeout(timer)
  }, [error, failures, stopping, membership, attempt, refresh])
  useEffect(() => {
    const visible = () => { if (document.visibilityState === 'visible' && registered.current.size) refresh() }
    document.addEventListener('visibilitychange', visible); return () => document.removeEventListener('visibilitychange', visible)
  }, [refresh])
  useEffect(() => {
    if (!expectedKey || ![...runs.values()].some(active)) return
    const timer = window.setInterval(() => { if (document.visibilityState !== 'hidden') setNow(Date.now()) }, 1000)
    return () => window.clearInterval(timer)
  }, [runs, expectedKey])
  const stop = async (id: string) => {
    const run = firstLevelRun(runs.get(id))
    if (!isCefContext() || mutating.current || error || !run?.canCancel || run.lastReported) return
    ++generation.current // An older read cannot restore a pre-Stop control scope.
    mutating.current = true; setStopping(id); setFeedback('')
    try {
      await cancelManagedSubagent(sourceChatId, id, run.controlScopeId)
      setFeedback('Stop requested. Reading the child’s recorded status.'); refresh()
    } catch { setFeedback('Stop was not confirmed. Checking the child’s recorded status.'); setError(true); setRuns(previous => new Map([...previous].map(([key, value]) => [key, { ...value, lastReported: true, canCancel: false }]))) }
    finally { mutating.current = false; setStopping('') }
  }
  return <Context.Provider value={{ runs, loading: readingSourceChatId === sourceChatId, error, readError, stopping, feedback, now, register, stop, open: onOpenManagedRun }}>{children}</Context.Provider>
}

export function InlineSubagentGroup({ tools, sourceChatId, onSelectTool, renderNative }: { tools: AcpToolCall[]; sourceChatId?: string; onSelectTool: (id: string) => void; renderNative: (tools: AcpToolCall[]) => ReactNode }) {
  const scopedFeed = useContext(Context)
  // Passive recorded history must not register against an enclosing parent feed.
  const feed = sourceChatId ? scopedFeed : null
  const signature = tools.map(tool => managedLaunchRunId(tool)).filter(Boolean).join('\0')
  const ids = useMemo(() => signature ? signature.split('\0') : [], [signature])
  const register = feed?.register
  useEffect(() => register?.(ids), [register, ids])
  const [open, setOpen] = useState(true)
  const rows = tools.map(tool => {
    const id = managedLaunchRunId(tool)
    if (!id) return <li key={tool.id} data-launch-tool={tool.id}>{renderNative([tool])}</li>
    const candidate = feed?.runs.get(id)
    const run = firstLevelRun(candidate)
    if (candidate && !run) return <li key={tool.id} data-launch-tool={tool.id}>{renderNative([tool])}</li>
    const label = run ? managedRunLabel(run.status, run.lastReported) : 'Status unavailable'
    const title = run ? managedTaskTitle(run) : 'Subagent task'
    return <li key={tool.id} data-launch-tool={tool.id}>
      <div className="uam-inline-subagents__row">
        <SubagentCard data-managed-open title={title} status={label} providerId={run?.providerId} summary={run?.summary}
          working={Boolean(run && active(run))} elapsed={run ? managedElapsed(run, feed?.now ?? Date.now()) : 'Timing unavailable'}
          aria-label={`Open managed conversation for ${title}`} disabled={!isCefContext() || !run?.transcriptChatId || !feed?.open}
          onClick={() => feed?.open?.(id)} />
        {run?.canCancel && !run.lastReported && <Button size="sm" variant="ghost" leadingIcon={<Square size={12} aria-hidden />} disabled={Boolean(feed?.stopping) || feed?.error} onClick={event => { event.currentTarget.closest('li')?.querySelector<HTMLButtonElement>('[data-managed-open]')?.focus(); void feed?.stop(id!) }} aria-label={`Stop managed subagent ${title}`}>Stop</Button>}
        <Button size="sm" variant="ghost" onClick={() => onSelectTool(tool.id)} aria-label={`Launch details for ${title}`}>Details</Button>
      </div>
      {run && <p className="uam-inline-subagents__meta">{[run.agentId, run.providerId, run.modelId].filter(Boolean).join(' · ')}</p>}
    </li>
  })
  if (!ids.length) return <section className="uam-inline-subagents" aria-label={tools.length > 1 ? 'Consecutive subagent launches' : 'Subagent launch'} data-source-chat={sourceChatId}>{renderNative(tools)}</section>
  return <section className="uam-inline-subagents" aria-label={tools.length > 1 ? 'Consecutive subagent launches' : 'Subagent launch'} data-source-chat={sourceChatId}>
    {tools.length > 1 ? <details open={open} onToggle={event => setOpen(event.currentTarget.open)}><summary><Users size={15} aria-hidden />{tools.length} launches<ChevronRight size={14} aria-hidden /></summary>{open && <ul>{rows}</ul>}</details> : <ul>{rows}</ul>}
    {ids.length > 0 && feed?.readError && <p className="uam-inline-subagents__meta" role="status">Could not read the latest subagent status. Previously read statuses remain visible.</p>}
    {ids.length > 0 && feed?.feedback && <p role="status">{feed.feedback}</p>}
  </section>
}

/** An annotation requests metadata; only exact first-level scoped list evidence creates a card. */
export function InitialManagedSubagentGroup({ runId, sourceChatId }: { runId: string; sourceChatId: string }) {
  const feed = useContext(Context)
  const id = sanitizeManagedOriginRunId(runId)
  const register = feed?.register
  useEffect(() => id ? register?.([id]) : undefined, [id, register])
  const candidate = id ? feed?.runs.get(id) : undefined
  const run = firstLevelRun(candidate)
  if (!id || !feed) return null
  if (!run) return feed.error || feed.loading ? <section className="uam-inline-subagents" aria-label="Linked subagent status" data-source-chat={sourceChatId}>
    <p className="uam-inline-subagents__meta" role="status">{feed.error ? 'Could not load linked subagent status' : 'Reading linked subagent status'}</p>
  </section> : null
  const title = managedTaskTitle(run)
  return <section className="uam-inline-subagents" aria-label="Subagent launch" data-source-chat={sourceChatId} data-managed-origin={id}>
    <ul><li><div className="uam-inline-subagents__row">
      <SubagentCard data-managed-open title={title} status={managedRunLabel(run.status, run.lastReported)} providerId={run.providerId} summary={run.summary}
        working={active(run)} elapsed={managedElapsed(run, feed.now)} aria-label={`Open managed conversation for ${title}`}
        disabled={!isCefContext() || !run.transcriptChatId || !feed.open} onClick={() => feed.open?.(id)} />
      {run.canCancel && !run.lastReported && <Button size="sm" variant="ghost" leadingIcon={<Square size={12} aria-hidden />}
        disabled={Boolean(feed.stopping) || feed.error} onClick={event => { event.currentTarget.closest('li')?.querySelector<HTMLButtonElement>('[data-managed-open]')?.focus(); void feed.stop(id) }} aria-label={`Stop managed subagent ${title}`}>Stop</Button>}
    </div></li></ul>
    {feed.readError && <p role="status">Could not read the latest subagent status. Previously read statuses remain visible.</p>}
    {feed.feedback && <p role="status">{feed.feedback}</p>}
  </section>
}
