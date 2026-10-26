import { useCallback, useEffect, useId, useLayoutEffect, useMemo, useRef, useState, useSyncExternalStore } from 'react'
import { Square } from 'lucide-react'
import { Button } from '../ui'
import type { Message } from '../../types/message'
import type { Session } from '../../types/session'
import { useAppStore } from '../../store/useAppStore'
import { sanitizeCppMessage } from '../../store/cpp/sanitizers'
import { buildMessageFromCpp } from '../../store/cpp/reconcile'
import { SubagentHeader, SubagentTranscript, SubagentTranscriptLoading } from './SubagentTranscript'
import { cancelManagedSubagent, listManagedSubagents, managedRead, managedRunActive, managedRunLabel, managedTimestamp, managedTranscriptFingerprint, record, retryManagedRead, string, validManagedId, acquireManagedFollowupDraft, releaseManagedFollowupDraft, subscribeManagedFollowupDraft, notifyManagedFollowupDraft, newManagedFollowupEnvelope, submitManagedFollowup, managedFollowupRefusal, managedFollowupTextAllowed, MANAGED_FOLLOWUP_TEXT_BYTES, type ManagedSubagentPage, type ManagedFollowupTarget, type ManagedFollowupDraft } from './managedSubagents'
import './ManagedSubagentConversation.css'

export { isManagedSubagentTool } from './managedSubagents'

export type ManagedSubagentConversationProps = {
  sourceChatId: string
  runId: string
  transcriptChatId?: string
  title?: string
  status?: string
  /** Display-only parent name for the single child header. */
  parentName?: string
  onBack?: () => void
  autoFocus?: boolean
  navigationKey?: string
}

type Transcript = { runId: string; title: string; status: string; lastReported: boolean; providerId: string; messages: unknown[]; fingerprint: string }
type StopTarget = { controlScopeId: string; parentOwner: string }

// Match the caller's parent read identity; a revision is a read wakeup, never a new owner or control grant.
const managedRevisionOwner = (session: Session | undefined) => session ? JSON.stringify([
  session.id, session.createdAt.getTime(), session.providerId, session.executionHostId,
  session.workspaceDirectory, session.workspaceWorktreeDirectory, session.folderId,
  session.importedReadOnly, session.subagentParentChatId, session.subagentParentCreatedAt,
]) : ''

/** Serialized child messages become the same Message shape the main transcript renders, keyed by transcript ID and raw
 * index. A missing or unparseable date stays invalid (no fabricated epoch); an unreadable row stays visible as such. */
export function managedHistoryMessages(transcriptChatId: string, values: readonly unknown[]): Message[] {
  return values.map((value, index) => {
    const stored = sanitizeCppMessage(value)
    if (!stored) return { id: `managed-unreadable-${transcriptChatId}-${index}`, sessionId: transcriptChatId, nativeMessageIndex: index, role: 'system',
      content: 'This stored message could not be read.', createdAt: new Date(NaN) }
    const message = buildMessageFromCpp(transcriptChatId, stored, index)
    return { ...message, createdAt: new Date(stored.createdAt ? managedTimestamp(stored.createdAt) : NaN) }
  })
}

/** Stored child conversation with explicit server-scoped UserRoot follow-up; never adopts a runtime chat. */
export function ManagedSubagentConversation(props: ManagedSubagentConversationProps) {
  return <ManagedConversationContent key={JSON.stringify([props.sourceChatId, props.runId, props.transcriptChatId, props.navigationKey])} {...props} />
}

function ManagedConversationContent({ sourceChatId, runId, transcriptChatId, title, status, parentName, onBack, autoFocus = true, navigationKey = '' }: ManagedSubagentConversationProps) {
  const viewportKey = navigationKey ? JSON.stringify([navigationKey, 'managed', sourceChatId, runId]) : ''
  const mounted = useRef(true)
  const [followupTarget, setFollowupTarget] = useState<ManagedFollowupTarget | null>(null)
  const followupTargetRef = useRef<ManagedFollowupTarget | null>(null)
  const [followupDraft, setFollowupDraft] = useState<ManagedFollowupDraft | null>(null)
  const followupDraftRef = useRef<ManagedFollowupDraft | null>(null)
  const [draftUnavailable, setDraftUnavailable] = useState(false)
  // No standing composer: an eligible child opens its follow-up form on request.
  const [composing, setComposing] = useState(false)
  const [followupHidden, setFollowupHidden] = useState(false)
  const fieldId = useId()
  const followupField = useRef<HTMLTextAreaElement | null>(null)
  const detachedFieldOwnedFocus = useRef(false)
  const setFollowupField = useCallback((field: HTMLTextAreaElement | null) => {
    if (!field && followupField.current?.form?.contains(document.activeElement)) detachedFieldOwnedFocus.current = true
    followupField.current = field
  }, [])
  const followupAction = useRef<HTMLButtonElement | null>(null)
  const detachedActionOwnedFocus = useRef(false)
  const setFollowupAction = useCallback((button: HTMLButtonElement | null) => {
    if (!button && followupAction.current === document.activeElement) detachedActionOwnedFocus.current = true
    followupAction.current = button
  }, [])
  useEffect(() => {
    mounted.current = true
    if (followupDraftRef.current) ++followupDraftRef.current.users // Reconnected effects retain the same inert mounted draft.
    return () => { mounted.current = false; followupTargetRef.current = null; if (followupDraftRef.current) releaseManagedFollowupDraft(followupDraftRef.current) }
  }, [])
  const subscribeDraft = useCallback((listener: () => void) => followupDraft ? subscribeManagedFollowupDraft(followupDraft, listener) : () => {}, [followupDraft])
  const draftVersion = useCallback(() => followupDraft?.version ?? 0, [followupDraft])
  useSyncExternalStore(subscribeDraft, draftVersion, draftVersion)
  const [transcript, setTranscript] = useState<Transcript | null>(null)
  const hasTranscriptRef = useRef(false)
  const [loading, setLoading] = useState(true)
  const [readPending, setReadPending] = useState(true)
  const [error, setError] = useState('')
  const [readFailures, setReadFailures] = useState(0)
  const [refreshMessage, setRefreshMessage] = useState('')
  const manualRefresh = useRef(false)
  const [attempt, setAttempt] = useState(0)
  const busy = useRef(false)
  const revisionOwner = useRef(managedRevisionOwner(useAppStore.getState().sessions.find(session => session.id === sourceChatId)))
  const parentOwner = useAppStore(state => managedRevisionOwner(state.sessions.find(session => session.id === sourceChatId)))
  const currentParent = () => {
    const parent = useAppStore.getState().sessions.find(session => session.id === sourceChatId)
    return Boolean(parent && Number.isFinite(parent.createdAt.getTime()) && !parent.importedReadOnly && revisionOwner.current && managedRevisionOwner(parent) === revisionOwner.current)
  }
  const [stopTarget, setStopTarget] = useState<StopTarget | null>(null)
  const stopTargetRef = useRef<StopTarget | null>(null)
  const stoppingRef = useRef(false)
  const [stopping, setStopping] = useState(false)
  const [stopMessage, setStopMessage] = useState('')
  const [stopFailed, setStopFailed] = useState(false)
  const [stopReadError, setStopReadError] = useState('')
  const stopButton = useRef<HTMLButtonElement | null>(null)
  const detachedStopOwnedFocus = useRef(false)
  const setStopButton = useCallback((button: HTMLButtonElement | null) => {
    if (!button && stopButton.current === document.activeElement) detachedStopOwnedFocus.current = true
    stopButton.current = button
  }, [])
  const managedRevision = useAppStore(state => {
    const parent = state.sessions.find(session => session.id === sourceChatId)
    return revisionOwner.current && managedRevisionOwner(parent) === revisionOwner.current ? parent?.managedSubagentRevision ?? 0 : null
  })
  const previousRevision = useRef(managedRevision)
  const revisionReadQueued = useRef(false)
  const requestRevisionRead = () => {
    const parent = useAppStore.getState().sessions.find(session => session.id === sourceChatId)
    if (!mounted.current || !revisionOwner.current || managedRevisionOwner(parent) !== revisionOwner.current) return
    if (busy.current) revisionReadQueued.current = true
    else { stopTargetRef.current = null; backgroundRead.current = hasTranscriptRef.current; setAttempt(value => value + 1) }
  }
  const request = useRef(0)
  const resolvedId = useRef(transcriptChatId || '')
  const reResolved = useRef(false)
  const backgroundRead = useRef(false)
  const readController = useRef<AbortController | null>(null)
  const heading = useRef<HTMLHeadingElement>(null)
  const currentStatus = transcript?.status || (error ? '' : status) || ''
  const lastReported = transcript?.lastReported ?? false
  // A retained snapshot stays last reported through recovery; only a validated read restores live status.
  const displayLastReported = lastReported || Boolean(transcript && readFailures > 0)
  const active = managedRunActive(currentStatus, lastReported)
  const hasTranscript = transcript !== null
  useEffect(() => { if (autoFocus) heading.current?.focus({ preventScroll: true }) }, [])

  useEffect(() => {
    if (stoppingRef.current) { revisionReadQueued.current = true; return }
    const serial = ++request.current
    const controller = new AbortController()
    const manual = manualRefresh.current
    manualRefresh.current = false
    readController.current = controller
    let disposed = false
    busy.current = true
    setLoading(!backgroundRead.current)
    setReadPending(true)
    setError('')
    stopTargetRef.current = null
    // Retain only presentation during background reads; the authority ref is already revoked.
    if (!backgroundRead.current) setStopTarget(null)
    if (!backgroundRead.current) setStopReadError('')
    followupTargetRef.current = null
    if (!backgroundRead.current) setFollowupTarget(null)
    setDraftUnavailable(false)
    void (async () => {
      try {
        if (!validManagedId(sourceChatId) || !validManagedId(runId)) throw new Error('Managed subagent identity is unavailable.')
        let listing: ManagedSubagentPage | undefined
        const loadListing = () => retryManagedRead(() => listManagedSubagents(sourceChatId, runId, 0, 100, controller.signal), controller.signal)
        const resolveReference = async () => {
          const result = await loadListing()
          if (disposed || request.current !== serial) return
          listing = result
          resolvedId.current = result.runs.find(run => run.runId === runId)?.transcriptChatId || ''
        }
        if (!resolvedId.current || !transcriptChatId) await resolveReference()
        if (!validManagedId(resolvedId.current)) throw new Error('This managed subagent has no available stored conversation.')
        const read = () => retryManagedRead(() => managedRead('getManagedAgentTranscript', { chatId: sourceChatId, transcriptChatId: resolvedId.current }, controller.signal), controller.signal)
        let raw: unknown
        try { raw = await read() }
        catch (cause) {
          if (controller.signal.aborted || reResolved.current || !(cause instanceof Error) || !/outside this chat|unavailable|changed|no longer exists/i.test(cause.message)) throw cause
          reResolved.current = true
          resolvedId.current = ''
          await resolveReference()
          if (!validManagedId(resolvedId.current)) throw new Error('This managed subagent has no available stored conversation.')
          raw = await read()
        }
        const data = record(raw)
        if (disposed || request.current !== serial) return
        if (data.runId !== runId || !Array.isArray(data.messages) ||
            (Object.prototype.hasOwnProperty.call(data, 'lastReported') && typeof data.lastReported !== 'boolean')) throw new Error('The managed conversation changed or is unavailable. Refresh to try again.')
        // A settled supplied route keeps the legacy get-only path unless the live response carries the hint.
        // The hint is a discovery signal only, including false; only a fresh exact listing may enable control.
        if (!listing && data.lastReported !== true && Object.prototype.hasOwnProperty.call(data, 'followup')) listing = await loadListing()
        // Stop is independent of native messaging support. A live conversation with no follow-up hint
        // still discovers cancellation through the existing exact-run control listing.
        if (!listing && data.lastReported !== true && managedRunActive(string(data.status)) && currentParent()) {
          // Cancellation discovery must not hold an already-readable conversation behind its read.
          hasTranscriptRef.current = true
          setTranscript({ runId, title: string(data.title), status: string(data.status), lastReported: false, providerId: string(data.providerId), messages: data.messages, fingerprint: managedTranscriptFingerprint(data.messages) })
          try { listing = await loadListing() }
          catch (cause) {
            if (controller.signal.aborted) throw cause
            if (!disposed && request.current === serial && currentParent()) setStopReadError('Stop is unavailable until this child’s current status can be read. Refresh to try again.')
          }
        }
        if (disposed || request.current !== serial) return
        if (listing || data.lastReported === true || !managedRunActive(string(data.status))) setStopReadError('')
        const row = listing?.runs.find(candidate => candidate.runId === runId)
        let nextStopTarget: StopTarget | null = null
        if (row?.canCancel && !row.lastReported && data.lastReported !== true &&
            managedRunActive(row.status) && managedRunActive(string(data.status)) &&
            row.transcriptChatId === resolvedId.current && row.providerId && row.providerId === string(data.providerId) && currentParent()) {
          nextStopTarget = { controlScopeId: listing!.controlScopeId, parentOwner: revisionOwner.current }
        }
        stopTargetRef.current = nextStopTarget
        setStopTarget(nextStopTarget)
        const target: ManagedFollowupTarget | null = row?.followup && !row.lastReported && data.lastReported !== true && row.transcriptChatId === resolvedId.current && row.providerId === string(data.providerId)
          ? { chatId: sourceChatId, runId, transcriptChatId: resolvedId.current, controlScopeId: listing!.controlScopeId, state: row.followup } : null
        followupTargetRef.current = target
        setFollowupTarget(target)
        if (target) {
          const key = JSON.stringify([sourceChatId, runId, resolvedId.current])
          if (followupDraftRef.current?.key !== key) {
            if (followupDraftRef.current) releaseManagedFollowupDraft(followupDraftRef.current)
            followupDraftRef.current = acquireManagedFollowupDraft(target) ?? null
            setFollowupDraft(followupDraftRef.current)
          }
          setDraftUnavailable(target.state.canSend && !followupDraftRef.current)
        }
        setReadFailures(0)
        setStopMessage('')
        setStopFailed(false)
        hasTranscriptRef.current = true
        setTranscript({ runId, title: string(data.title), status: string(data.status), lastReported: data.lastReported === true, providerId: string(data.providerId), messages: data.messages, fingerprint: managedTranscriptFingerprint(data.messages) })
        if (manual) setRefreshMessage('Conversation refreshed.')
      } catch (cause) {
        if (!disposed && request.current === serial && !controller.signal.aborted) {
          stopTargetRef.current = null
          setStopTarget(null)
          setReadFailures(value => value + 1)
          setError(cause instanceof Error ? cause.message : 'Managed conversation is unavailable.')
          if (manual) setRefreshMessage('Could not refresh the conversation.')
        }
      } finally {
        if (!disposed && request.current === serial) {
          busy.current = false; setLoading(false); setReadPending(false)
          if (revisionReadQueued.current) { revisionReadQueued.current = false; requestRevisionRead() }
        }
      }
    })()
    return () => { disposed = true; controller.abort(); ++request.current }
  }, [attempt, runId, sourceChatId, transcriptChatId])

  useEffect(() => {
    if (previousRevision.current === managedRevision) return
    previousRevision.current = managedRevision
    if (managedRevision !== null) requestRevisionRead()
  }, [managedRevision])

  useEffect(() => {
    if (readPending || (!error && !active) || !validManagedId(sourceChatId) || !validManagedId(runId)) return
    const refreshRead = () => { stopTargetRef.current = null; backgroundRead.current = hasTranscript; setAttempt(value => value + 1) }
    // Failed reads recover independently of run status; only a validated read resets the capped backoff.
    const delay = error ? Math.min(60000, 5000 * 2 ** Math.max(0, readFailures - 1)) : 5000
    const timer = window.setTimeout(() => {
      if (document.visibilityState !== 'hidden' && !busy.current) refreshRead()
    }, delay)
    const onVisible = () => {
      if (document.visibilityState === 'visible' && !busy.current) refreshRead()
    }
    document.addEventListener('visibilitychange', onVisible)
    return () => { window.clearTimeout(timer); document.removeEventListener('visibilitychange', onVisible) }
  }, [active, readPending, error, attempt, readFailures, hasTranscript, sourceChatId, runId])

  const stopChild = async () => {
    const target = stopTargetRef.current
    if (!mounted.current || !target || stoppingRef.current || busy.current || readPending || error ||
        displayLastReported || !currentParent() || target.parentOwner !== revisionOwner.current) return
    stoppingRef.current = true
    stopTargetRef.current = null
    setStopTarget(null)
    // Retire read callbacks before the mutation. Neither polling nor Refresh may replay this Stop.
    const serial = ++request.current
    readController.current?.abort()
    busy.current = true
    setStopping(true)
    setStopFailed(false)
    setStopMessage('Requesting Stop for this child…')
    heading.current?.focus({ preventScroll: true })
    try {
      await cancelManagedSubagent(sourceChatId, runId, target.controlScopeId)
      if (mounted.current && request.current === serial && currentParent()) setStopMessage('Stop requested. Reading this child’s status.')
    } catch {
      if (mounted.current && request.current === serial && currentParent()) {
        setStopFailed(true)
        setStopMessage('Stop was not confirmed. Checking this child’s latest status. Stop will not be sent again automatically.')
      }
    } finally {
      stoppingRef.current = false
      if (mounted.current && request.current === serial) {
        busy.current = false
        setStopping(false)
        if (currentParent()) {
          revisionReadQueued.current = false
          backgroundRead.current = true
          setAttempt(value => value + 1)
        } else { revisionReadQueued.current = false; setStopMessage('') }
      }
    }
  }

  const sendFollowup = async (retry = false) => {
    const target = followupTargetRef.current
    const entry = followupDraftRef.current
    if (!mounted.current || !target || !entry || stoppingRef.current || readPending || error || lastReported || entry.phase === 'pending') return
    if (entry.phase === 'unknown' && !retry) return
    if (!retry && !target.state.canSend) return
    const envelope = retry ? entry.request : newManagedFollowupEnvelope(target, entry.text)
    if (!envelope || (!retry && !target.state.canSend)) {
      entry.feedback = 'Enter nonblank follow-up work of at most 32 KiB. Secure request identification must be available.'
      notifyManagedFollowupDraft(entry)
      return
    }
    const invocation = {}
    entry.request = envelope
    entry.invocation = invocation
    // Keep the user's keyboard position before Send disables or Retry disappears.
    // Only hand off focus already owned by this exact child form.
    const field = followupField.current
    if (field?.form?.contains(document.activeElement)) field.focus({ preventScroll: true })
    entry.phase = 'pending'
    entry.feedback = ''
    notifyManagedFollowupDraft(entry)
    const outcome = await submitManagedFollowup(target, envelope)
    // Completion updates only this retained child draft, even after the pane unmounts. It never submits again.
    if (entry.invocation !== invocation || entry.request !== envelope) return
    entry.invocation = undefined
    if (!outcome || (retry && !outcome.accepted)) {
      entry.phase = 'unknown'
      entry.feedback = retry && outcome ? `${managedFollowupRefusal(outcome.code)} The original request is still unconfirmed.`
        : 'The result is unknown. Check this child before sending more work, or retry the same request.'
    } else {
      entry.phase = 'idle'
      entry.request = undefined
      entry.feedback = outcome.accepted ? '' : managedFollowupRefusal(outcome.code)
      if (outcome.accepted && entry.text === envelope.text) entry.text = ''
    }
    notifyManagedFollowupDraft(entry)
    // Retire only this mounted entry's accepted, settled empty editor; a newer draft or recovery stays visible.
    if (outcome?.accepted && mounted.current && followupDraftRef.current === entry &&
        entry.phase === 'idle' && !entry.text && !entry.request && !entry.feedback) setComposing(false)
    if (mounted.current && followupDraftRef.current === entry && followupTargetRef.current?.transcriptChatId === target.transcriptChatId && outcome?.accepted) {
      // Stop owns readback until its acknowledgement settles. A late follow-up receipt must not
      // replace the read effect and retire that mutation's response fence.
      if (stoppingRef.current) revisionReadQueued.current = true
      else { stopTargetRef.current = null; backgroundRead.current = true; setAttempt(value => value + 1) }
    }
  }

  // Hiding an idle draft changes this view only. Pending/uncertain requests and feedback stay disclosed
  // so their recovery controls cannot disappear behind an old hidden preference.
  const canStart = Boolean(followupTarget?.state.canSend && followupDraft && !lastReported)
  const needsRecovery = Boolean(followupDraft && (followupDraft.phase !== 'idle' || followupDraft.request || followupDraft.feedback))
  const retainedDraft = Boolean(followupDraft?.text)
  const canInspectFollowup = Boolean(followupTarget && followupDraft && !lastReported && (canStart || retainedDraft || needsRecovery))
  const showFollowup = Boolean(canInspectFollowup && (needsRecovery || !followupHidden && (composing && followupTarget?.state.canSend || retainedDraft)))
  const canHideFollowup = Boolean(showFollowup && !needsRecovery)
  useEffect(() => { if (needsRecovery) setFollowupHidden(false) }, [needsRecovery])
  useEffect(() => {
    // An absent target during a read/error is not a confirmed loss of eligibility.
    if (followupTarget?.state.canSend === false) setComposing(false)
  }, [followupTarget?.state.canSend])
  useEffect(() => { if (composing) followupField.current?.focus({ preventScroll: true }) }, [composing])
  useLayoutEffect(() => {
    const lostField = detachedFieldOwnedFocus.current && !followupField.current
    const lostStop = detachedStopOwnedFocus.current && !stopButton.current
    const lostAction = detachedActionOwnedFocus.current && !followupAction.current
    detachedFieldOwnedFocus.current = false
    detachedStopOwnedFocus.current = false
    detachedActionOwnedFocus.current = false
    // Runs only after a commit in this surviving child, never after unmount.
    if ((lostField || lostStop || lostAction) && autoFocus && heading.current?.isConnected && document.activeElement === document.body) {
      heading.current.focus({ preventScroll: true })
    }
  })
  const history = useMemo(() => transcript ? managedHistoryMessages(resolvedId.current, transcript.messages) : [], [transcript])
  const refresh = () => { if (stoppingRef.current) return; stopTargetRef.current = null; ++request.current; readController.current?.abort(); backgroundRead.current = false; manualRefresh.current = true; setRefreshMessage('Refreshing conversation…'); setAttempt(value => value + 1) }
  const failure = error && <p role="alert" className="uam-managed-conversation__error">{error}{transcript ? ' The previously loaded conversation is still shown.' : ''}</p>
  return <section className="uam-managed-conversation" aria-label="Managed subagent conversation">
    <SubagentHeader title={transcript?.title || title || 'Managed subagent'} parentName={parentName} detail={managedRunLabel(currentStatus, displayLastReported)}
      headingRef={heading} onBack={onBack} onRefresh={refresh} refreshing={loading} refreshDisabled={stopping}>
      {(stopping || stopTarget && parentOwner === stopTarget.parentOwner && !displayLastReported) && <Button ref={setStopButton} variant="danger" size="sm" leadingIcon={<Square size={12} aria-hidden />}
        loading={stopping} aria-disabled={stopping || readPending || Boolean(error) || !stopTarget || undefined} onClick={() => void stopChild()}>Stop child</Button>}
      {canInspectFollowup && !showFollowup && <Button ref={setFollowupAction} variant="ghost" size="sm" onClick={() => { setFollowupHidden(false); setComposing(true) }}>{canStart ? 'Follow up' : 'Show draft'}</Button>}
      {canHideFollowup && <Button ref={setFollowupAction} variant="ghost" size="sm" aria-disabled={readPending || Boolean(error) || undefined} onClick={() => {
        const entry = followupDraftRef.current
        if (!entry || entry.phase !== 'idle' || entry.request || entry.feedback || busy.current || readPending || error) return
        setFollowupHidden(true)
        setComposing(false)
        heading.current?.focus({ preventScroll: true })
      }}>Hide input</Button>}
    </SubagentHeader>
    <p className="uam-managed-conversation__meta sr-only" role="status">{managedRunLabel(currentStatus, displayLastReported)}{loading && !transcript ? ' · Loading conversation…' : ''}</p>
    <p className="uam-managed-conversation__meta sr-only">{showFollowup || canStart ? 'Follow-up work stays attached to this child.' : stopping || stopTarget && parentOwner === stopTarget.parentOwner && !displayLastReported ? 'Stored child conversation. Available controls apply only to this child.' : 'Read-only stored conversation.'}</p>
    {draftUnavailable && <p role="alert" className="uam-managed-conversation__error">Follow-up editing has reached its limit. Close other settled child views or check unresolved requests first.</p>}
    {refreshMessage && <p className="uam-managed-conversation__meta sr-only" role="status">{refreshMessage}</p>}
    {stopMessage && !stopFailed && <p className="uam-managed-conversation__meta" role="status">{stopMessage}</p>}
    {stopMessage && stopFailed && <p className="uam-managed-conversation__meta" role="alert">{stopMessage}</p>}
    {stopReadError && <p className="uam-managed-conversation__meta" role="status">{stopReadError}</p>}
    {canInspectFollowup && followupHidden && retainedDraft && !showFollowup && <p className="uam-managed-conversation__meta" role="status">Draft kept in this child.</p>}
    {transcript && history.length > 0
      ? <SubagentTranscript messages={history} fallbackProviderId={transcript.providerId} sessionId={resolvedId.current} viewportKey={viewportKey}
          contentVersion={transcript.fingerprint} label="Stored conversation messages" notices={failure} />
      : <div className="uam-subagent-transcript__empty">
          {failure}
          {transcript ? <p role="status" className="conversation-empty">No stored messages yet.</p>
            : loading ? <SubagentTranscriptLoading label="Loading conversation…" /> : null}
        </div>}
    {showFollowup && followupDraft && <form className="uam-managed-conversation__followup" aria-label="Send follow-up work" onSubmit={event => { event.preventDefault(); void sendFollowup() }}>
      <label htmlFor={fieldId}>Follow-up message</label>
      <textarea ref={setFollowupField} id={fieldId} className="uam-field uam-field--area" rows={3} value={followupDraft.text}
        readOnly={followupDraft.phase === 'pending'} aria-busy={followupDraft.phase === 'pending' || undefined} aria-describedby={`${fieldId}-help`}
        onChange={event => {
          if (followupDraft.phase === 'pending') return
          if (event.currentTarget.value.length > MANAGED_FOLLOWUP_TEXT_BYTES || new TextEncoder().encode(event.currentTarget.value).length > MANAGED_FOLLOWUP_TEXT_BYTES) {
            followupDraft.feedback = 'Follow-up work must fit within 32 KiB.'
          } else { followupDraft.text = event.currentTarget.value; if (followupDraft.phase === 'idle') followupDraft.feedback = '' }
          notifyManagedFollowupDraft(followupDraft)
        }}
        onKeyDown={event => {
          if (event.key === 'Enter' && !event.shiftKey && !event.nativeEvent.isComposing && event.keyCode !== 229) {
            event.preventDefault(); void sendFollowup()
          }
        }} />
      <p id={`${fieldId}-help`} className="uam-managed-conversation__meta">Enter sends. Shift+Enter adds a line. Text only, up to 32 KiB.</p>
      {followupDraft.phase === 'unknown' && <p className="uam-managed-conversation__meta">Retry sends the original message. Any edits stay in this separate draft.</p>}
      {followupDraft.phase === 'idle' && !followupTarget?.state.canSend && <p role="status" className="uam-managed-conversation__meta">{managedFollowupRefusal(followupTarget?.state.code ?? 'native_resume_unavailable')}</p>}
      <div className="uam-managed-conversation__followup-actions">
        <Button type="submit" variant="primary" size="sm" loading={followupDraft.phase === 'pending'} disabled={stopping || followupDraft.phase !== 'idle' || !followupTarget?.state.canSend || readPending || Boolean(error) || !managedFollowupTextAllowed(followupDraft.text)}>Send follow-up</Button>
        {followupDraft.phase === 'unknown' && <Button type="button" size="sm" aria-disabled={stopping || readPending || Boolean(error)} onClick={() => void sendFollowup(true)}>Retry same request</Button>}
        {followupDraft.phase === 'pending' && <span role="status" className="uam-managed-conversation__meta">Recording follow-up work…</span>}
      </div>
      {followupDraft.feedback && <p role="alert" className="uam-managed-conversation__error">{followupDraft.feedback}</p>}
    </form>}
  </section>
}
