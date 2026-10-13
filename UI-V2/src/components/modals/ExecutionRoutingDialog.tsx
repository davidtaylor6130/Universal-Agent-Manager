import { useEffect, useId, useRef, useState } from 'react'
import { ChevronRight, X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { routingConfigurationError, routingReservation, type ExecutionRoutingSettings, type RoutingHostPolicy, type RoutingReservation } from '../../types/executionRouting'
import type { ExecutionHost, Session } from '../../types/session'
import { Button, ConfirmDialog, IconButton, Overlay, StatusDot, Switch, type StatusTone } from '../ui'
import { SettingRow, SettingsField, SettingsSection } from '../settings/SettingsPrimitives'
import '../shared/review.css'
import './ExecutionRoutingDialog.css'
import { getTopmostModal, trapModalTab } from '../../utils/modalFocus'

export const RUNNER_STATUS: Record<ExecutionHost['runnerStatus'], { tone: StatusTone; label: string }> = {
  ready: { tone: 'success', label: 'Ready' },
  installing: { tone: 'info', label: 'Installing runner' },
  uninstalled: { tone: 'warning', label: 'Runner not installed' },
  offline: { tone: 'warning', label: 'Offline' },
  error: { tone: 'error', label: 'Runner error' },
}

/** Collapsed-by-default block. Content stays mounted so saved state is always in the DOM; it is inert while closed. */
function Disclosure({ title, summary, children }: { title: string; summary: string; children: React.ReactNode }) {
  const [open, setOpen] = useState(false)
  const id = useId()
  return <div className="uam-rt-disclosure" data-open={open}>
    <button type="button" className="uam-rt-disclosure__toggle" aria-expanded={open} aria-controls={id} onClick={() => setOpen((current) => !current)}>
      <ChevronRight size={14} aria-hidden />
      <span>{title}</span>
      <span className="uam-rt-disclosure__summary">{summary}</span>
    </button>
    <div id={id} className="uam-rt-disclosure__body" {...(open ? {} : { inert: '' })}>
      <div className="uam-rt-disclosure__inner">{children}</div>
    </div>
  </div>
}

/** Explicit opt-in and workspace mappings use registered folders; native settings own persisted confirmations. */
export function ExecutionRoutingDialog({ onClose }: { onClose: () => void }) {
  const saved = useAppStore((state) => state.executionRouting)
  const hosts = useAppStore((state) => state.executionHosts)
  const folders = useAppStore((state) => state.folders)
  const providers = useAppStore((state) => state.providers)
  const readiness = useAppStore((state) => state.cliVersionManager.remoteProviders)
  const save = useAppStore((state) => state.setExecutionRoutingSettings)
  const [draft, setDraft] = useState<ExecutionRoutingSettings>(() => structuredClone(saved))
  const [busy, setBusy] = useState(false)
  const busyRef = useRef(false)
  const [error, setError] = useState('')
  const panel = useRef<HTMLDivElement>(null)
  const closeRef = useRef(onClose); closeRef.current = onClose
  useEffect(() => {
    const previous = document.activeElement as HTMLElement | null
    const dialog = panel.current
    dialog?.focus()
    const key = (event: KeyboardEvent) => {
      if (event.defaultPrevented || getTopmostModal() !== panel.current) return
      if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); if (!busyRef.current) closeRef.current() }
      else trapModalTab(event)
    }
    window.addEventListener('keydown', key, true)
    return () => {
      window.removeEventListener('keydown', key, true)
      if (previous?.isConnected && (!document.activeElement || document.activeElement === document.body || dialog?.contains(document.activeElement))) previous.focus()
    }
  }, [])
  function update(hostId: string, change: (policy: RoutingHostPolicy) => RoutingHostPolicy) {
    setDraft((current) => {
      const policy = current.hosts.find((host) => host.hostId === hostId) ?? { hostId, enabled: false, maxActiveTurns: 1, providers: [], workspaceMappings: [] }
      return { ...current, hosts: [...current.hosts.filter((host) => host.hostId !== hostId), change(policy)] }
    })
    setError('')
  }
  function confirmed(policy: RoutingHostPolicy | undefined, hostId: string, providerId: string) {
    const entry = policy?.providers.find((provider) => provider.providerId === providerId)
    const current = readiness?.find((provider) => provider.providerId === providerId && provider.executionHostId === hostId)
    return Boolean(entry?.confirm || (entry && current?.routingConnectionIdentity && current.routingProviderConfigurationIdentity &&
      entry.connectionIdentity === current.routingConnectionIdentity && entry.providerConfigurationIdentity === current.routingProviderConfigurationIdentity))
  }
  async function submit() {
    if (busyRef.current) return
    const validation = routingConfigurationError(draft, hosts, folders)
    if (validation) { setError(validation); return }
    busyRef.current = true; setBusy(true); setError('')
    try { await save(draft); closeRef.current() }
    catch (failure) { setError(failure instanceof Error ? failure.message : 'Routing settings could not be saved.') }
    finally { busyRef.current = false; setBusy(false) }
  }
  const sshHosts = hosts.filter((host) => host.transport === 'ssh')
  const localFolders = folders.filter((folder) => folder.directory)
  return <Overlay open zIndex={80} onBackdropMouseDown={() => { if (!busyRef.current) onClose() }}>
    <div ref={panel} tabIndex={-1} role="dialog" aria-modal="true" aria-label="SSH routing" aria-busy={busy} className="uam-rt-dialog">
      <div className="uam-rt-dialog__head">
        <div className="min-w-0">
          <h2 className="uam-rt-dialog__title">SSH routing</h2>
          <p className="uam-rt-dialog__desc">Choose which computers can run your chats and which remote folders they use.</p>
        </div>
        <IconButton icon={<X size={15} />} label="Close SSH routing" size="sm" disabled={busy} onClick={onClose} />
      </div>
      <fieldset disabled={busy} className="uam-rt-dialog__body">
        <SettingsSection title="Automatic routing" description="Where new chats start when you do not pick a computer.">
          <SettingRow htmlFor="routing-enabled" title="Route new chats automatically" description="Only computers you allow below, with folders mapped, are used."
            control={<Switch id="routing-enabled" hideLabel label="Enable automatic routing" checked={draft.enabled} onChange={(event) => setDraft({ ...draft, enabled: event.target.checked })} />} />
        </SettingsSection>

        {sshHosts.length === 0
          ? <SettingsSection title="Computers" description="SSH computers you allow to run chats.">
              <p className="uam-rt-dialog__empty">No SSH computers yet. Add one first, then come back here to allow it.</p>
            </SettingsSection>
          : sshHosts.map((host) => {
            const policy = draft.hosts.find((item) => item.hostId === host.id)
            const look = RUNNER_STATUS[host.runnerStatus]
            const mapped = localFolders.filter((source) => policy?.workspaceMappings.some((mapping) => mapping.sourceFolderId === source.id && mapping.sourceDirectory === source.directory)).length
            const remoteFolders = folders.filter((folder) => folder.executionHostId === host.id)
            return <SettingsSection key={host.id} title={host.label}
              description={<span className="uam-rt-alias">{host.sshAlias}</span>}
              actions={<span className="uam-rt-status"><StatusDot tone={look.tone} size={7} />{look.label}</span>}>
              <SettingRow htmlFor={`routing-enabled-${host.id}`} title="Use this computer" description="Allow chats to start on this computer."
                control={<Switch id={`routing-enabled-${host.id}`} hideLabel label={`Route to ${host.label}`} checked={policy?.enabled ?? false} onChange={(event) => update(host.id, (item) => ({ ...item, enabled: event.target.checked }))} />} />
              <SettingRow htmlFor={`routing-capacity-${host.id}`} title="Concurrent sessions" description="How many chats can run here at the same time."
                control={<input id={`routing-capacity-${host.id}`} className="uam-rv-field" style={{ width: 80, height: 32 }} type="number" min={1} max={1024} aria-label={`Concurrent sessions on ${host.label}`} value={policy?.maxActiveTurns ?? 1} onChange={(event) => update(host.id, (item) => ({ ...item, maxActiveTurns: Number(event.target.value) }))} />} />
              {providers.filter((provider) => provider.supportsStructured !== false || provider.supportsCli !== false).map((provider) => (
                <SettingRow key={provider.id} title={provider.name} description={`Confirm once you have signed in to ${provider.name} on this computer.`}
                  control={<Switch hideLabel label={`${provider.name} signed in on ${host.label}`} checked={confirmed(policy, host.id, provider.id)} onChange={(event) => update(host.id, (item) => ({ ...item, providers: event.target.checked ? [...item.providers.filter((entry) => entry.providerId !== provider.id), { providerId: provider.id, connectionIdentity: '', providerConfigurationIdentity: '', confirm: true }] : item.providers.filter((entry) => entry.providerId !== provider.id) }))} />} />
              ))}
              <div className="uam-set-row">
                <Disclosure title="Remote folders" summary={localFolders.length ? `${mapped} of ${localFolders.length} mapped` : 'No folders'}>
                  <div className="grid gap-3">
                    {localFolders.length === 0 && <p className="uam-rt-dialog__empty">Give a local folder a path first, then map it here.</p>}
                    {localFolders.length > 0 && remoteFolders.length === 0 && <p className="uam-rt-dialog__empty">No folders exist on {host.label} yet, so there is nothing to map to.</p>}
                    {localFolders.map((source) => {
                      const id = `routing-map-${host.id}-${source.id}`
                      return <SettingsField key={source.id} label={source.name} htmlFor={id} hint={<span className="uam-rt-alias">{source.directory}</span>}>
                        <select id={id} className="uam-rv-field w-full" aria-label={`Destination for ${source.name} on ${host.label}`} value={policy?.workspaceMappings.find((mapping) => mapping.sourceFolderId === source.id && mapping.sourceDirectory === source.directory)?.destinationFolderId ?? ''} onChange={(event) => update(host.id, (item) => ({ ...item, workspaceMappings: [...item.workspaceMappings.filter((mapping) => mapping.sourceFolderId !== source.id || mapping.sourceDirectory !== source.directory), ...(event.target.value ? [{ sourceFolderId: source.id, sourceDirectory: source.directory, destinationFolderId: event.target.value }] : [])] }))}>
                          <option value="">Don&apos;t route this folder</option>
                          {remoteFolders.map((folder) => <option key={folder.id} value={folder.id}>{folder.name} · {folder.directory}</option>)}
                        </select>
                      </SettingsField>
                    })}
                  </div>
                </Disclosure>
              </div>
            </SettingsSection>
          })}
      </fieldset>
      <div className="uam-rt-dialog__foot">
        {error && <p role="alert" className="uam-rt-dialog__error">{error}</p>}
        <Button variant="ghost" size="md" disabled={busy} onClick={onClose}>Cancel</Button>
        <Button variant="primary" size="md" loading={busy} onClick={() => void submit()}>Save routing</Button>
      </div>
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
  return <div role="status" className="border-b border-[var(--border)] px-3 py-2 text-xs" style={{ background: 'var(--surface-up)', color: 'var(--text)' }}>
    <span>{session.routingWaitReason || 'This chat has an unresolved remote reservation.'}</span>
    <Button variant="ghost" size="sm" onClick={() => setConfigure(true)}>Routing settings</Button>
    {routingReservation(session) && <Button variant="ghost" size="sm" onClick={() => { setExpected(routingReservation(session)); setError('') }}>Release reservation</Button>}
    {configure && <ExecutionRoutingDialog onClose={() => setConfigure(false)} />}
    <ConfirmDialog open={expected !== null} title="Release remote reservation?" confirmLabel="Release reservation" busy={busy} error={error} onCancel={() => { if (!busyRef.current) setExpected(null) }} onConfirm={() => {
      if (!expected || busyRef.current) return
      busyRef.current = true; setBusy(true); setError('')
      void release(expected).then(() => setExpected(null)).catch((failure) => setError(failure instanceof Error ? failure.message : 'The reservation could not be released.')).finally(() => { busyRef.current = false; setBusy(false) })
    }}>The existing runner process may still exist on this computer. Release this reservation only after checking the runner. UAM may assign new work to this computer after release.</ConfirmDialog>
  </div>
}
