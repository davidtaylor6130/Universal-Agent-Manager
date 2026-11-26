import { useEffect, useRef, useState } from 'react'
import { X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { routingReservation, type RoutingReservation } from '../../types/executionRouting'
import type { Session } from '../../types/session'
import { ROUTING_SKIP_LABEL, hostSlotsLabel, type RoutingWaitHost } from '../../types/hostLoad'
import { Button, ConfirmDialog, IconButton, Notice, Overlay } from '../ui'
import { ExecutionRoutingSettings, type ExecutionRoutingSettingsHandle } from '../settings/ExecutionRoutingSettings'
import './ExecutionRoutingDialog.css'
import { fromViewportMenu, getTopmostModal, trapModalTab } from '../../utils/modalFocus'

/** Explicit opt-in and workspace mappings use registered folders; native settings own persisted confirmations. */
export function ExecutionRoutingDialog({ onClose }: { onClose: () => void }) {
  const [busy, setBusy] = useState(false)
  const busyRef = useRef(false)
  const panel = useRef<HTMLDivElement>(null)
  const settings = useRef<ExecutionRoutingSettingsHandle>(null)
  const closeRef = useRef(onClose); closeRef.current = onClose
  useEffect(() => {
    const previous = document.activeElement as HTMLElement | null
    const dialog = panel.current
    dialog?.focus()
    const key = (event: KeyboardEvent) => {
      if (event.defaultPrevented || fromViewportMenu(event) || getTopmostModal() !== panel.current) return
      if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); if (!settings.current?.closeStep?.() && !busyRef.current) closeRef.current() }
      else trapModalTab(event)
    }
    window.addEventListener('keydown', key, true)
    return () => {
      window.removeEventListener('keydown', key, true)
      if (previous?.isConnected && (!document.activeElement || document.activeElement === document.body || dialog?.contains(document.activeElement))) previous.focus()
    }
  }, [])
  return <Overlay open zIndex={80} onBackdropMouseDown={() => { if (!busyRef.current) onClose() }}>
    <div ref={panel} tabIndex={-1} role="dialog" aria-modal="true" aria-label="SSH routing" aria-busy={busy} className="uam-rt-dialog">
      <div className="uam-rt-dialog__head">
        <div className="min-w-0">
          <h2 className="uam-rt-dialog__title">SSH routing</h2>
          <p className="uam-rt-dialog__desc">Choose which computers can run your chats and which remote folders they use.</p>
        </div>
        <IconButton icon={<X size={15} />} label="Close SSH routing" size="sm" disabled={busy} onClick={onClose} />
      </div>
      <ExecutionRoutingSettings ref={settings} variant="dialog" onClose={onClose} onBusyChange={(next) => { busyRef.current = next; setBusy(next) }} />
    </div>
  </Overlay>
}

export function ExecutionRoutingWaitNotice({ session }: { session: Session }) {
  const release = useAppStore((state) => state.releaseExecutionRoutingReservation)
  const [configure, setConfigure] = useState(false)
  const [expected, setExpected] = useState<RoutingReservation | null>(null)
  const [busy, setBusy] = useState(false)
  const busyRef = useRef(false)
  const [error, setError] = useState('')
  if (!session.routingWaitReason && !session.routingUnresolvedReservation) return null
  return <>
    <Notice tone="warning" title="Routing needs attention" dismissible={false} dismissLabel="Dismiss" actions={<>
      <Button variant="ghost" size="sm" onClick={() => setConfigure(true)}>Routing settings</Button>
      {routingReservation(session) && <Button variant="ghost" size="sm" onClick={() => { setExpected(routingReservation(session)); setError('') }}>Release reservation</Button>}
    </>}>
      {session.routingWaitReason || 'This chat has an unresolved remote reservation.'}
    </Notice>
    {/* Outside the notice's live region: its in-use counts change with every load reading and must not re-announce the notice. */}
    <RoutingWaitHosts hosts={session.routingWaitHosts} />
    {configure && <ExecutionRoutingDialog onClose={() => setConfigure(false)} />}
    <ConfirmDialog open={expected !== null} title="Release remote reservation?" confirmLabel="Release reservation" busy={busy} error={error} onCancel={() => { if (!busyRef.current) setExpected(null) }} onConfirm={() => {
      if (!expected || busyRef.current || !release) return
      busyRef.current = true; setBusy(true); setError('')
      void release(expected).then(() => setExpected(null)).catch((failure) => setError(failure instanceof Error ? failure.message : 'The reservation could not be released.')).finally(() => { busyRef.current = false; setBusy(false) })
    }}>The existing runner process may still exist on this computer. Release this reservation only after checking the runner. UAM may assign new work to this computer after release.</ConfirmDialog>
  </>
}

/** Each computer automatic routing passed over and why, with its in-use count. Computers turned off share one line. */
export function RoutingWaitHosts({ hosts }: { hosts?: RoutingWaitHost[] }) {
  const executionHosts = useAppStore((state) => state.executionHosts)
  const load = useAppStore((state) => state.executionHostLoad)
  if (!hosts?.length) return null
  const shown = hosts.filter((host) => host.reason !== 'off')
  const off = hosts.length - shown.length
  return <ul className="uam-rt-wait-hosts" aria-label="Computers passed over">
    {shown.map((host) => {
      const slots = hostSlotsLabel(load.hosts[host.hostId])
      return <li key={host.hostId} data-reason={host.reason}>
        <span className="uam-rt-wait-hosts__host">{executionHosts.find((candidate) => candidate.id === host.hostId)?.label || 'Removed computer'}</span>
        <span>{ROUTING_SKIP_LABEL[host.reason]}{slots ? ` · ${slots}` : ''}</span>
      </li>
    })}
    {off > 0 && <li data-reason="off"><span className="uam-rt-wait-hosts__host">{off === 1 ? '1 computer' : `${off} computers`}</span><span>{ROUTING_SKIP_LABEL.off}</span></li>}
  </ul>
}
