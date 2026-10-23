import { useLayoutEffect, useRef, useState } from 'react'
import { CircleDashed, RefreshCw } from 'lucide-react'
import { isCefContext, sendToCEF } from '../../ipc/cefBridge'
import type { AcpBinding, CopilotAutoRoutingProjection } from '../../store/cpp/types'
import { Button, MenuSelect, Notice } from '../ui'

type LocalRequest = {
  kind: 'preference' | 'refresh'
  value: string | null | undefined
  initialEffective: string
  initialAck: string
  accepted: boolean
  sawNativeWork: boolean
  settled: boolean
  failed: boolean
  unchanged: boolean
}

type Props = {
  acp: AcpBinding
  disabled?: boolean
}

function preferenceName(projection: CopilotAutoRoutingProjection, value: string | null): string {
  if (value === null) return 'Default'
  return projection.eligibleTiers.present
    ? projection.eligibleTiers.value.find((tier) => tier.id === value)?.displayName || value
    : value
}

/** Only a different native owner retires local requests; ordinary pushes retain the focused DOM. */
export function CopilotAutoRoutingControl({ acp, disabled = false }: Props) {
  const projection = acp.nativeModelOptions?.autoRouting
  if (!projection) return null
  const live = acp.running && acp.lifecycleState === 'ready' && !acp.processing && !acp.compacting && !(acp.backgroundTaskCount ?? 0)
  const scope = JSON.stringify(projection.ownership ?? null)
  return <ScopedAutoRoutingControl key={scope} projection={projection} live={live} disabled={disabled} />
}

function ScopedAutoRoutingControl({ projection, live, disabled }: {
  projection: CopilotAutoRoutingProjection
  live: boolean
  disabled: boolean
}) {
  const alive = useRef(true)
  const inFlight = useRef(false)
  const [sending, setSending] = useState<'preference' | 'refresh' | null>(null)
  const [error, setError] = useState('')
  const [localRequest, setLocalRequest] = useState<LocalRequest | null>(null)
  const [dismissedError, setDismissedError] = useState('')
  const groupRef = useRef<HTMLDivElement>(null)
  const focusedControl = useRef<HTMLElement | null>(null)
  // The shared disabled/loading buttons retain DOM identity; keep keyboard focus in Options
  // if a push disables an action or dismissing a Notice removes it. Never steal other focus.
  useLayoutEffect(() => {
    const group = groupRef.current
    const previous = focusedControl.current
    if (group && previous && (!previous.isConnected || (group.contains(previous) && previous.matches(':disabled')))
      && (document.activeElement === previous || document.activeElement === document.body)) group.focus()
    // Button owns loading semantics; reuse UAM's existing animated ring for its spinner.
    group?.querySelector('.uam-btn__spinner')?.classList.add('uam-working-ring')
  })
  // Native progress can supersede a local refusal without retiring an admitted same-owner request.
  const effectiveKey = JSON.stringify(projection.effective)
  const nativeProgress = JSON.stringify([projection.stateStatus, projection.effective, projection.pending, projection.activating,
    projection.requestPending, projection.lastAckStatus, projection.lastError])
  useLayoutEffect(() => {
    setError('')
    setLocalRequest((issued) => {
      if (!issued) return issued
      const nativeWork = projection.requestPending || projection.pending.present || projection.activating.present
      const freshAck = projection.lastAckStatus !== issued.initialAck || issued.sawNativeWork
      const failed = issued.failed || Boolean(projection.lastError)
      const resolved = !nativeWork && projection.stateStatus === 'ready'
        && (effectiveKey !== issued.initialEffective || issued.sawNativeWork
          || (freshAck && projection.lastAckStatus === 'unchanged'))
      const matches = issued.value === null ? !projection.effective.present
        : projection.effective.present && projection.effective.value === issued.value
      // Once resolved, later native activity can retire this receipt but cannot revive it.
      if (issued.settled) return { ...issued, failed, unchanged: issued.unchanged && !failed
        && !nativeWork && projection.stateStatus === 'ready' && matches && projection.lastAckStatus === 'unchanged' }
      return { ...issued, sawNativeWork: issued.sawNativeWork || nativeWork,
        settled: issued.settled || failed || resolved, failed,
        unchanged: !failed && issued.kind === 'preference' && resolved && freshAck
          && projection.lastAckStatus === 'unchanged' && matches }
    })
  }, [nativeProgress])
  useLayoutEffect(() => {
    alive.current = true
    return () => { alive.current = false }
  }, [])

  const connected = isCefContext()
  const owner = projection.ownership
  const canChoose = connected && live && !disabled && Boolean(owner) && projection.available && !projection.requestPending
    && projection.catalogStatus === 'ready' && projection.stateStatus === 'ready'
    && projection.currentModel.present && projection.currentModel.value === 'auto' && !projection.activating.present
    && projection.eligibleTiers.present
  // Refresh is also useful when native eligibility/state is unknown or unsupported.
  const canRefresh = connected && live && !disabled && Boolean(owner) && !projection.requestPending
  const choices = [
    { value: '', label: 'Choose preference', description: '', nativeValue: undefined as string | null | undefined, disabled: false },
    { value: 'default', label: 'Default', description: 'Return to the provider’s default Auto routing.', nativeValue: null as string | null, disabled: false },
    ...(projection.eligibleTiers.present ? projection.eligibleTiers.value.map((tier, index) => ({
      // Menu tokens cannot collide with opaque provider IDs or the explicit Default action.
      value: `tier:${index}`,
      label: tier.displayName || tier.id,
      description: [tier.description, !tier.status.enabled || tier.type !== 'auto'
        ? tier.status.message || tier.status.reason || 'Unavailable for Auto routing.' : ''].filter(Boolean).join(' · '),
      nativeValue: tier.id,
      disabled: !tier.status.enabled || tier.type !== 'auto',
    })) : []),
  ]

  const request = async (kind: 'preference' | 'refresh', value?: string | null) => {
    if (!alive.current || inFlight.current || !isCefContext() || !owner
      || (kind === 'preference' ? !canChoose : !canRefresh)) return
    inFlight.current = true
    setSending(kind)
    setError('')
    setLocalRequest({ kind, value, initialEffective: effectiveKey, initialAck: projection.lastAckStatus,
      accepted: false, sawNativeWork: false, settled: false, failed: false, unchanged: false })
    setDismissedError('')
    try {
      const response = await sendToCEF({
        action: kind === 'preference' ? 'setNativeModelOption' : 'refreshNativeModelOptions',
        payload: { ...owner, ...(kind === 'preference' ? { optionId: 'autoRouting', value } : {}) },
      }, false)
      if (!alive.current) return
      if (response.ok) setLocalRequest((issued) => issued ? { ...issued, accepted: true } : issued)
      else setError(typeof response.error === 'string' && response.error ? response.error.slice(0, 2048) : 'Copilot could not accept the request. Try refreshing.')
    } catch {
      if (alive.current) setError('Could not reach Copilot. Try refreshing.')
    } finally {
      if (alive.current) {
        inFlight.current = false
        setSending(null)
      }
    }
  }

  const effective = projection.stateStatus === 'ready'
    ? `Effective: ${projection.effective.present ? preferenceName(projection, projection.effective.value) : 'Default'}`
    : projection.effective.present ? `Last confirmed: ${preferenceName(projection, projection.effective.value)}` : 'Effective preference unavailable.'
  const unavailable = !connected ? 'Connect to UAM to change Auto routing.'
    : !projection.available ? projection.disabledReason
    : !live || disabled ? 'Wait for this chat’s current work to finish before changing Auto routing.' : ''
  const failure = error || (projection.lastError !== unavailable ? projection.lastError : '')
  const working = sending !== null || projection.requestPending || projection.activating.present
  // A retained native ACK is only feedback for a request issued in this mount, with exact
  // resolved preference equality. Bridge acceptance never supplies effective state.
  const matchesRequested = localRequest?.value === null ? !projection.effective.present
    : projection.effective.present && projection.effective.value === localRequest?.value
  const nativeReceipt = localRequest?.accepted && !localRequest.failed && !failure
    && localRequest.unchanged && matchesRequested && projection.stateStatus === 'ready'
    && !projection.requestPending && !projection.pending.present && !projection.activating.present
    && projection.lastAckStatus === 'unchanged'
    ? `No change; already ${preferenceName(projection, localRequest.value ?? null)}.` : ''
  const receipt = localRequest?.accepted && !localRequest.settled && !failure
    ? working
      ? localRequest.kind === 'preference' ? 'Preference requested; waiting for Copilot.' : 'Refresh requested; waiting for Copilot.'
      : localRequest.kind === 'preference' ? 'Preference request accepted.' : 'Refresh request accepted.'
    : ''

  return <div ref={groupRef} role="group" aria-label="Copilot Auto routing" aria-busy={working} tabIndex={-1}
    onFocusCapture={(event) => { focusedControl.current = event.target as HTMLElement }}
    onBlurCapture={(event) => {
      if (event.relatedTarget instanceof Node && !groupRef.current?.contains(event.relatedTarget)) focusedControl.current = null
    }}>
    <div className="uam-cmenu__setting">
      <span>Auto routing</span>
      <MenuSelect label="Auto routing" value="" placeholder="Choose preference" options={choices}
        disabled={!canChoose || sending !== null}
        onChange={(token) => {
          const choice = choices.find((entry) => entry.value === token)
          if (choice && !choice.disabled && choice.nativeValue !== undefined) void request('preference', choice.nativeValue)
        }} />
    </div>
    <div className="px-3 pb-1 text-[11px]" style={{ color: 'var(--text-3)' }} aria-label="Auto routing state">
      <span className="block">{effective}</span>
      {unavailable && <span className="block">{unavailable}</span>}
    </div>
    <div className="px-3 pb-1 text-[11px]" style={{ color: 'var(--text-3)' }}
      role="status" aria-label="Auto routing progress">
      {working && sending !== 'refresh' && <CircleDashed size={12} className="uam-working-ring inline-block mr-1" style={{ color: 'var(--blue)' }} aria-hidden />}
      {projection.pending.present && <span className="block">Pending: {preferenceName(projection, projection.pending.value)}</span>}
      {projection.activating.present && <span className="block">Activating: {preferenceName(projection, projection.activating.value)}</span>}
      {sending && <span className="block">{sending === 'preference' ? 'Sending preference…' : 'Refreshing…'}</span>}
      {projection.requestPending && !sending && <span className="block">Waiting for Copilot…</span>}
      {(nativeReceipt || receipt) && <span className="block">{nativeReceipt || receipt}</span>}
    </div>
    {failure && failure !== dismissedError && <Notice key={failure} tone="error" title="Auto routing"
      dismissLabel="Dismiss Auto routing error" onDismiss={() => { setDismissedError(failure) }}>{failure}</Notice>}
    <div className="flex justify-end px-2 pb-1">
      <Button variant="ghost" size="sm" leadingIcon={<RefreshCw size={12} aria-hidden />} loading={sending === 'refresh'}
        disabled={!canRefresh || sending !== null} onClick={() => { void request('refresh') }}>Refresh Auto routing</Button>
    </div>
  </div>
}
