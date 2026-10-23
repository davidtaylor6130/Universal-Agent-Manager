import { useCallback, useEffect, useId, useLayoutEffect, useMemo, useRef, useState, useSyncExternalStore } from 'react'
import { Button } from '../ui'
import type { Message } from '../../types/message'
import type { Session } from '../../types/session'
import { useAppStore } from '../../store/useAppStore'
import { sanitizeCppMessage } from '../../store/cpp/sanitizers'
import { buildMessageFromCpp } from '../../store/cpp/reconcile'
import { SubagentHeader, SubagentTranscript, SubagentTranscriptLoading } from './SubagentTranscript'
import { listManagedSubagents, managedRead, managedRunActive, managedRunLabel, managedTimestamp, managedTranscriptFingerprint, record, retryManagedRead, string, validManagedId, acquireManagedFollowupDraft, releaseManagedFollowupDraft, subscribeManagedFollowupDraft, notifyManagedFollowupDraft, newManagedFollowupEnvelope, submitManagedFollowup, managedFollowupRefusal, managedFollowupTextAllowed, MANAGED_FOLLOWUP_TEXT_BYTES, type ManagedSubagentPage, type ManagedFollowupTarget, type ManagedFollowupDraft } from './managedSubagents'
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
  const fieldId = useId()
  const followupField = useRef<HTMLTextAreaElement | null>(null)
  const detachedFieldOwnedFocus = useRef(false)
  const setFollowupField = useCallback((field: HTMLTextAreaElement | null) => {
    if (!field && followupField.current?.form?.contains(document.activeElement)) detachedFieldOwnedFocus.current = true
    followupField.current = field
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
    else { backgroundRead.current = hasTranscriptRef.current; setAttempt(value => value + 1) }
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
        // A supplied route keeps the legacy get-only path unless the live response explicitly carries the hint.
        // The hint is a discovery signal only, including false; only a fresh exact listing may enable control.
        if (!listing && data.lastReported !== true && Object.prototype.hasOwnProperty.call(data, 'followup')) listing = await loadListing()
        if (disposed || request.current !== serial) return
        const row = listing?.runs.find(candidate => candidate.runId === runId)
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
        hasTranscriptRef.current = true
        setTranscript({ runId, title: string(data.title), status: string(data.status), lastReported: data.lastReported === true, providerId: string(data.providerId), messages: data.messages, fingerprint: managedTranscriptFingerprint(data.messages) })
        if (manual) setRefreshMessage('Conversation refreshed.')
      } catch (cause) {
        if (!disposed && request.current === serial && !controller.signal.aborted) {
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
    const refreshRead = () => { backgroundRead.current = hasTranscript; setAttempt(value => value + 1) }
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

  const sendFollowup = async (retry = false) => {
    const target = followupTargetRef.current
    const entry = followupDraftRef.current
    if (!mounted.current || !target || !entry || readPending || error || lastReported || entry.phase === 'pending') return
    if (entry.phase === 'unknown' && !retry) return
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
      backgroundRead.current = true
      setAttempt(value => value + 1)
    }
  }

  // The child view has no standing composer. The form shows when the reader asks for it on an eligible child, and
  // always while a request or draft is unresolved, so an unknown delivery keeps its explicit same-request Retry and no
  // draft text disappears.
  const canStart = Boolean(followupTarget?.state.canSend && followupDraft && !lastReported)
  const showFollowup = Boolean(followupTarget && followupDraft && !lastReported && ((composing && followupTarget.state.canSend) || followupDraft.phase !== 'idle' || followupDraft.text || followupDraft.feedback))
  const canHideFollowup = Boolean(showFollowup && followupDraft?.phase === 'idle' && !followupDraft.text &&
    !followupDraft.request && !followupDraft.feedback && !readPending && !error)
  useEffect(() => {
    // An absent target during a read/error is not a confirmed loss of eligibility.
    if (followupTarget?.state.canSend === false) setComposing(false)
  }, [followupTarget?.state.canSend])
  useEffect(() => { if (composing) followupField.current?.focus({ preventScroll: true }) }, [composing])
  useLayoutEffect(() => {
    if (!detachedFieldOwnedFocus.current) return
    detachedFieldOwnedFocus.current = false
    // Runs only after a commit in this surviving child, never after unmount.
    if (!followupField.current && autoFocus && heading.current?.isConnected && document.activeElement === document.body) {
      heading.current.focus({ preventScroll: true })
    }
  })
  const history = useMemo(() => transcript ? managedHistoryMessages(resolvedId.current, transcript.messages) : [], [transcript])
  const refresh = () => { ++request.current; readController.current?.abort(); backgroundRead.current = false; manualRefresh.current = true; setRefreshMessage('Refreshing conversation…'); setAttempt(value => value + 1) }
  const failure = error && <p role="alert" className="uam-managed-conversation__error">{error}{transcript ? ' The previously loaded conversation is still shown.' : ''}</p>
  return <section className="uam-managed-conversation" aria-label="Managed subagent conversation">
    <SubagentHeader title={transcript?.title || title || 'Managed subagent'} parentName={parentName} detail={managedRunLabel(currentStatus, displayLastReported)}
      headingRef={heading} onBack={onBack} onRefresh={refresh} refreshing={loading}>
      {canStart && !showFollowup && <Button variant="ghost" size="sm" onClick={() => setComposing(true)}>Follow up</Button>}
      {canHideFollowup && <Button variant="ghost" size="sm" aria-label="Hide follow-up input" onClick={() => {
        const entry = followupDraftRef.current
        if (!entry || entry.phase !== 'idle' || entry.text || entry.request || entry.feedback || busy.current) return
        setComposing(false)
        heading.current?.focus({ preventScroll: true })
      }}>Hide input</Button>}
    </SubagentHeader>
    <p className="uam-managed-conversation__meta sr-only" role="status">{managedRunLabel(currentStatus, displayLastReported)}{loading && !transcript ? ' · Loading conversation…' : ''}</p>
    <p className="uam-managed-conversation__meta sr-only">{showFollowup ? 'Follow-up work stays attached to this child.' : 'Read-only stored conversation. Follow-up messages and native session controls are unavailable here.'}</p>
    {draftUnavailable && <p role="alert" className="uam-managed-conversation__error">Follow-up editing has reached its limit. Close other settled child views or check unresolved requests first.</p>}
    {refreshMessage && <p className="uam-managed-conversation__meta sr-only" role="status">{refreshMessage}</p>}
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
        <Button type="submit" variant="primary" size="sm" loading={followupDraft.phase === 'pending'} disabled={followupDraft.phase !== 'idle' || !followupTarget?.state.canSend || readPending || Boolean(error) || !managedFollowupTextAllowed(followupDraft.text)}>Send follow-up</Button>
        {followupDraft.phase === 'unknown' && <Button type="button" size="sm" aria-disabled={readPending || Boolean(error)} onClick={() => void sendFollowup(true)}>Retry same request</Button>}
        {followupDraft.phase === 'pending' && <span role="status" className="uam-managed-conversation__meta">Recording follow-up work…</span>}
      </div>
      {followupDraft.feedback && <p role="alert" className="uam-managed-conversation__error">{followupDraft.feedback}</p>}
    </form>}
  </section>
}
