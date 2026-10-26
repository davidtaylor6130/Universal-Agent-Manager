import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState, type ReactNode, type RefObject } from 'react'
import { ArrowLeft, RefreshCw } from 'lucide-react'
import { Button, IconButton } from '../ui'
import { FileInspectionContext } from '../shared/fileInspectionContext'
import type { Message } from '../../types/message'
import { MessageNavigationRail } from './MessageNavigationRail'
import { PersistedConversationHistory } from './PersistedConversationMessage'
import type { PassiveHistoryView } from './usePassiveHistoryWindow'
import { readSubagentViewportVisit, rememberSubagentViewportVisit } from '../../utils/subagentViewportVisits'
import { alignRowStart } from '../../utils/motionStorage'

export const TRANSCRIPT_BATCH = 20
const REVEAL_EDGE_PX = 320
const TAIL_PX = 48
// ponytail: a rail jump into unloaded history reads at most this many existing older pages, one at a time.
export const MAX_JUMP_PAGES = 10

const nearTail = (region: HTMLElement) => region.scrollHeight - region.scrollTop - region.clientHeight < TAIL_PX

/** Local index of the first rendered loaded message. `anchor` is an absolute native index; null follows the tail.
 * Absolute anchoring keeps the same rows rendered when older history is prepended or newer rows are appended. */
export function transcriptWindowStart(count: number, nativeStartIndex: number, anchor: number | null) {
  const tail = Math.max(0, count - TRANSCRIPT_BATCH)
  return anchor === null ? tail : Math.max(0, Math.min(anchor - nativeStartIndex, tail))
}

/** Result of one existing child-scoped older-history read. `unchanged` = no rows and no recorded failure (stale or
 * superseded read); `failed` = the read recorded an error; `end` = nothing older exists. */
export type OlderHistoryOutcome = 'loaded' | 'unchanged' | 'failed' | 'end'

export type TranscriptWindowOptions = {
  /** Window-local reading-position key; '' disables memory. */
  viewportKey: string
  /** Loaded message count and the absolute native index of loaded row 0 (0 for managed transcripts). */
  count: number
  nativeStartIndex: number
  /** Any value that changes when loaded rows change (immutable rows array or full fingerprint). A change that arrives
   * together with a new nativeStartIndex is an older-page prepend, never new activity. */
  contentVersion: unknown
  /** One bounded older-history page; omit when none remain. */
  loadOlder?: () => Promise<OlderHistoryOutcome>
  /** Changes on explicit Refresh: clears older-read failure/suppression for the same history position. */
  recoveryEpoch?: unknown
}

/** Continuous child transcript scrolling: renders a bounded window, reveals older batches as the reader nears the top
 * (preserving the visible row), follows the tail only while the reader is there, and remembers the reading position.
 * Older reads are single-flight; a failed or unchanged read stops automatic retries until the reader scrolls up again,
 * chooses Retry, or Refresh changes the recovery epoch. Loaded rows are never dropped. Read-only: no input, no parent state. */
export function useSubagentTranscriptWindow({ viewportKey, count, nativeStartIndex, contentVersion, loadOlder, recoveryEpoch }: TranscriptWindowOptions) {
  const restored = useMemo(() => readSubagentViewportVisit(viewportKey), [viewportKey])
  const scrollRef = useRef<HTMLDivElement>(null)
  const lastRegion = useRef<HTMLDivElement | null>(null)
  // Visit memory reuses `page` for the absolute first rendered index (window-local only, never persisted).
  const [anchor, setAnchor] = useState<number | null>(() => restored && !restored.followTail ? restored.page : null)
  const [newContent, setNewContent] = useState(false)
  const [older, setOlder] = useState<'idle' | 'loading' | 'failed'>('idle')
  const followTail = useRef(restored?.followTail ?? true)
  const initialized = useRef(false)
  const prepend = useRef<{ height: number; top: number } | null>(null)
  const target = useRef<number | null>(null)
  const jumpPages = useRef(0)
  const toTail = useRef(false)
  const prior = useRef({ version: contentVersion, start: nativeStartIndex })
  const olderSerial = useRef(0)
  const olderInFlight = useRef(false)
  // A smooth rail jump owns the scroll until `scrollend`; reveal-older and tail intent wait so they cannot cancel it.
  const jumping = useRef(false)
  // Automatic (layout/fill) reads stay off at this native start until user intent or recovery.
  const autoHeldAt = useRef<number | null>(null)
  const start = transcriptWindowStart(count, nativeStartIndex, anchor)
  const live = useRef({ absoluteStart: 0, anchor, nativeStartIndex, loadOlder })
  live.current = { absoluteStart: nativeStartIndex + start, anchor, nativeStartIndex, loadOlder }

  useEffect(() => () => { ++olderSerial.current }, [])
  const epoch = useRef(recoveryEpoch)
  if (epoch.current !== recoveryEpoch) {
    // ponytail: render-phase reset of plain refs only; the state reset happens in the effect below.
    epoch.current = recoveryEpoch
    autoHeldAt.current = null
  }
  useEffect(() => { setOlder(current => current === 'failed' ? 'idle' : current) }, [recoveryEpoch])

  const checkpoint = useCallback((region: HTMLElement | null = scrollRef.current) => {
    if (!region || !viewportKey) return
    rememberSubagentViewportVisit(viewportKey, { page: followTail.current ? null : live.current.absoluteStart, top: region.scrollTop, followTail: followTail.current })
  }, [viewportKey])

  /** Reveal the next loaded batch, or read one older page. `intent` marks a reader action that may retry a held read. */
  const revealOlder = useCallback((intent: boolean) => {
    const region = scrollRef.current
    const { absoluteStart, anchor: current, nativeStartIndex: first, loadOlder: read } = live.current
    if (!region || prepend.current || olderInFlight.current) return
    if (absoluteStart > first) {
      prepend.current = { height: region.scrollHeight, top: region.scrollTop }
      setAnchor(Math.max(first, absoluteStart - TRANSCRIPT_BATCH))
      return
    }
    if (!read || (!intent && autoHeldAt.current === first)) return
    // Pin the rendered rows first so the store's prepend cannot shift them; reveal after the page arrives.
    if (current === null) setAnchor(absoluteStart)
    const serial = ++olderSerial.current
    olderInFlight.current = true
    setOlder('loading')
    const settle = (outcome: OlderHistoryOutcome) => {
      if (olderSerial.current !== serial) return
      olderInFlight.current = false
      if (outcome === 'loaded') {
        autoHeldAt.current = null
        const latest = scrollRef.current
        if (latest && target.current === null) prepend.current = { height: latest.scrollHeight, top: latest.scrollTop }
        if (target.current === null) setAnchor(absoluteStart - TRANSCRIPT_BATCH)
        setOlder('idle')
        return
      }
      autoHeldAt.current = first
      target.current = null
      setOlder(outcome === 'failed' ? 'failed' : 'idle')
    }
    read().then(settle, () => settle('failed'))
  }, [])

  useLayoutEffect(() => {
    const region = scrollRef.current
    if (!region) return
    lastRegion.current = region
    const positioned = !initialized.current || toTail.current || target.current !== null || prepend.current !== null
    const changed = prior.current.version !== contentVersion && prior.current.start === nativeStartIndex
    prior.current = { version: contentVersion, start: nativeStartIndex }
    if (!initialized.current) {
      initialized.current = true
      region.scrollTop = restored && !restored.followTail
        ? Math.min(restored.top, Math.max(0, region.scrollHeight - region.clientHeight)) : region.scrollHeight
    } else if (toTail.current) {
      region.scrollTop = region.scrollHeight
    } else if (target.current !== null) {
      // A rail jump below the loaded start keeps reading existing pages one at a time, within its page budget.
      if (target.current < nativeStartIndex) {
        if (jumpPages.current < MAX_JUMP_PAGES && !olderInFlight.current) { ++jumpPages.current; revealOlder(true) }
        else if (!olderInFlight.current) target.current = null
      } else {
        const row = region.querySelector<HTMLElement>(`[data-message-index="${target.current}"]`)
        if (row || (anchor !== null && anchor <= target.current)) { alignRow(row); target.current = null }
        else setAnchor(target.current)
      }
    } else if (prepend.current) {
      region.scrollTop = prepend.current.top + region.scrollHeight - prepend.current.height
    } else if (changed) {
      if (followTail.current) region.scrollTop = region.scrollHeight
      else setNewContent(true)
    }
    toTail.current = false
    prepend.current = null
    // Only an actual programmatic alignment may recompute intent here; user scroll owns every other change.
    if (target.current === null && positioned && !jumping.current) followTail.current = nearTail(region)
    checkpoint(region)
    // A short window fills itself one bounded batch at a time; scroll drives every later reveal.
    if (target.current === null && region.scrollHeight <= region.clientHeight + REVEAL_EDGE_PX) revealOlder(false)
  })
  useLayoutEffect(() => () => checkpoint(lastRegion.current), [checkpoint])

  const onScroll = useCallback(() => {
    const region = scrollRef.current
    if (!region) return
    if (!jumping.current) followTail.current = nearTail(region)
    if (followTail.current) setNewContent(false)
    // Leaving the tail pins the rendered rows so live appends cannot move what the reader is looking at.
    // ponytail: revealed rows stay rendered until Show latest/remount, as in the main transcript; trim if DOM size matters.
    else if (live.current.anchor === null) setAnchor(live.current.absoluteStart)
    if (!jumping.current && region.scrollTop < REVEAL_EDGE_PX) revealOlder(true)
    checkpoint(region)
  }, [checkpoint, revealOlder])

  /** Aligns a rail target row, smoothly unless Motion is Off or reduced. */
  const alignRow = useCallback((row: HTMLElement | null | undefined) => {
    const region = scrollRef.current
    if (row && region && alignRowStart(region, row, () => { jumping.current = false; onScroll() })) jumping.current = true
  }, [onScroll])

  /** Message-navigation rail target: reveals rows down to `index` (reading existing older pages if needed), then aligns it. */
  const revealIndex = useCallback((index: number) => {
    followTail.current = false
    jumpPages.current = 0
    const loaded = index >= live.current.nativeStartIndex
    if (loaded && index >= live.current.absoluteStart) {
      alignRow(scrollRef.current?.querySelector<HTMLElement>(`[data-message-index="${index}"]`))
      return
    }
    target.current = index
    if (loaded) setAnchor(index)
    // Reveal every loaded row first; the layout pass then reads existing older pages toward the target.
    else if (live.current.absoluteStart > live.current.nativeStartIndex) setAnchor(live.current.nativeStartIndex)
    else { ++jumpPages.current; revealOlder(true) }
  }, [alignRow, revealOlder])

  const retryOlder = useCallback(() => { autoHeldAt.current = null; revealOlder(true) }, [revealOlder])

  const showLatest = useCallback(() => {
    followTail.current = true
    toTail.current = true
    target.current = null
    setNewContent(false)
    setAnchor(null)
  }, [])

  return { scrollRef, start, onScroll, revealIndex, showLatest, retryOlder, newContent, older }
}

/** The single child-view bar: Back, child identity with its parent, then child-scoped actions. No close or view toggles. */
export function SubagentHeader({ title, parentName, detail, onBack, onClose, onRefresh, refreshing = false, refreshDisabled = false, headingRef, children }: {
  title: string
  /** Undefined only for an orphan child whose parent is unavailable. */
  parentName?: string
  detail?: string
  onBack?: () => void
  /** Existing orphan view-exit callback: return to chats without inventing a parent. */
  onClose?: () => void
  /** Explicit child-scoped reread; never sends input or touches the parent. */
  onRefresh?: () => void
  refreshing?: boolean
  refreshDisabled?: boolean
  headingRef?: RefObject<HTMLHeadingElement>
  children?: ReactNode
}) {
  const lineage = parentName ? `Subagent of ${parentName}` : 'Parent unavailable'
  const onReturn = onBack ?? onClose
  const returnLabel = onBack ? 'Back to parent' : onClose ? 'Back to chats' : 'Back'
  return <nav aria-label="Subagent lineage" className="uam-chat-pane__header uam-subagent-header flex h-11 shrink-0 items-center gap-3 px-4"
    style={{ borderBottom: '1px solid var(--border)', background: 'var(--surface)' }}>
    <Button variant="ghost" size="sm" disabled={!onReturn} leadingIcon={<ArrowLeft size={14} aria-hidden />} onClick={onReturn}>{returnLabel}</Button>
    <div className="flex min-w-0 flex-1 flex-col justify-center leading-tight" title={`${title}\n${lineage}`}>
      <h3 ref={headingRef} tabIndex={-1} className="uam-subagent-header__title truncate text-sm font-semibold" style={{ color: 'var(--text)' }}>{title}</h3>
      <span className="truncate text-[10px]" style={{ color: 'var(--text-3)' }}>{detail ? `${lineage} · ${detail}` : lineage}</span>
    </div>
    {children}
    {onRefresh && <IconButton size="sm" label="Refresh conversation" tooltipSide="bottom" disabled={refreshDisabled} aria-busy={refreshing || undefined} onClick={onRefresh}
      icon={<RefreshCw size={14} aria-hidden className={refreshing ? 'animate-spin motion-reduce:animate-none' : undefined} />} />}
  </nav>
}

/** Matches the main transcript's loading placeholder. */
export function SubagentTranscriptLoading({ label }: { label: string }) {
  return <div role="status" className="conversation-empty">
    <div className="conversation-skeleton" aria-hidden>
      <span className="conversation-skeleton__bubble" />
      <div className="conversation-skeleton__text">
        <span className="conversation-skeleton__line" />
        <span className="conversation-skeleton__line conversation-skeleton__line--mid" />
        <span className="conversation-skeleton__line conversation-skeleton__line--short" />
      </div>
    </div>
    <span>{label}</span>
  </div>
}

/** Child history in the main transcript's viewport, gutter, rail and canonical persisted rows; no composer or page chrome. */
export type SubagentTranscriptProps = {
  messages: Message[]
  nativeStartIndex?: number
  /** Full-history user positions when the child's own catalogue supplies them; otherwise the rail marks loaded rows. */
  userMessageIndices?: number[]
  fallbackProviderId: string
  sessionId: string
  viewportKey: string
  contentVersion: unknown
  loadOlder?: () => Promise<OlderHistoryOutcome>
  recoveryEpoch?: unknown
  label: string
  notices?: ReactNode
  scopedView?: PassiveHistoryView
}

type SurfaceView = ReturnType<typeof useSubagentTranscriptWindow> & {
  busy?: boolean; historyError?: string; retryHistory?: () => void; awayFromLatest?: boolean
}
export function SubagentTranscript(props: SubagentTranscriptProps) {
  return props.scopedView
    ? <TranscriptSurface {...props} messages={props.scopedView.messages} nativeStartIndex={props.scopedView.nativeStartIndex} view={props.scopedView} />
    : <LoadedTranscript {...props} />
}
function LoadedTranscript(props: SubagentTranscriptProps) {
  const { viewportKey, messages, nativeStartIndex = 0, contentVersion, loadOlder, recoveryEpoch } = props
  const view = useSubagentTranscriptWindow({ viewportKey, count: messages.length, nativeStartIndex, contentVersion, loadOlder, recoveryEpoch })
  return <TranscriptSurface {...props} view={view} />
}
function TranscriptSurface({ messages, nativeStartIndex = 0, userMessageIndices, fallbackProviderId, sessionId, loadOlder, label, notices, scopedView, view }: SubagentTranscriptProps & { view: SurfaceView }) {
  // Unloaded marks are only offered when this view can actually read toward them.
  const marks = userMessageIndices && (loadOlder || scopedView) ? userMessageIndices : undefined
  const navigable = marks ? marks.length > 0 : messages.some(message => message.role === 'user')
  return <FileInspectionContext.Provider value={null}>
    <div className="uam-chat-viewport relative min-h-0 flex-1" data-message-navigation={navigable || undefined}>
      <div ref={view.scrollRef} className="uam-chat-transcript uam-subagent-transcript relative z-0 h-full overflow-auto" role="region" tabIndex={0} aria-label={label} onScroll={view.onScroll}>
        <div className="uam-chat-content w-full py-4">
          {notices}
          {view.busy && <p role="status" className="uam-subagent-transcript__meta">Loading conversation history…</p>}
          {view.historyError && <div role="status" className="uam-subagent-transcript__meta">
            {view.historyError} Loaded messages are still shown.{' '}
            {view.retryHistory && <Button variant="ghost" size="sm" onClick={view.retryHistory}>Retry history</Button>}
          </div>}
          {view.older === 'loading' && <p role="status" className="uam-subagent-transcript__meta">Loading earlier messages…</p>}
          {view.older === 'failed' && <div role="status" className="uam-subagent-transcript__meta">
            Earlier messages could not be loaded. Loaded messages are still shown.{' '}
            <Button variant="ghost" size="sm" onClick={view.retryOlder}>Retry earlier messages</Button>
          </div>}
          <div className="uam-message-list space-y-1.5">
            <PersistedConversationHistory messages={messages} start={view.start} end={messages.length} nativeStartIndex={nativeStartIndex}
              fallbackProviderId={fallbackProviderId} sessionId={sessionId} />
          </div>
        </div>
      </div>
      <MessageNavigationRail userMessageIndices={marks} startIndex={nativeStartIndex} messages={messages} currentIndex={Math.max(0, nativeStartIndex + messages.length - 1)}
        viewportRef={view.scrollRef} busy={view.busy || view.older === 'loading'} onNavigate={view.revealIndex} />
      {(view.newContent || view.awayFromLatest) && <div className="absolute bottom-4 left-1/2 z-10 -translate-x-1/2">
        <Button variant="secondary" size="sm" onClick={view.showLatest}>{view.awayFromLatest ? 'Show latest' : 'New activity · Show latest'}</Button>
      </div>}
    </div>
  </FileInspectionContext.Provider>
}
