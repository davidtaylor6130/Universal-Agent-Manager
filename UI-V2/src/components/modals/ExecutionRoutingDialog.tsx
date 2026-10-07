import { useEffect, useRef, useState } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { routingConfigurationError, routingReservation, type ExecutionRoutingSettings, type RoutingHostPolicy, type RoutingReservation } from '../../types/executionRouting'
import type { Session } from '../../types/session'
import { Button, ConfirmDialog, Overlay } from '../ui'
import { getTopmostModal, trapModalTab } from '../../utils/modalFocus'

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
  return <Overlay open zIndex={80} onBackdropMouseDown={() => { if (!busyRef.current) onClose() }}>
    <div ref={panel} tabIndex={-1} role="dialog" aria-modal="true" aria-label="SSH routing" aria-busy={busy} className="w-full max-w-xl max-h-[85vh] overflow-y-auto bg-black text-white border border-[var(--border)] p-4 text-xs">
      <div className="flex justify-between items-center mb-3"><strong>SSH routing</strong><Button variant="ghost" size="sm" disabled={busy} onClick={onClose}>Close</Button></div>
      <label className="flex gap-2 items-center mb-3"><input type="checkbox" checked={draft.enabled} disabled={busy} onChange={(event) => setDraft({ ...draft, enabled: event.target.checked })} />Enable automatic routing</label>
      {hosts.filter((host) => host.transport === 'ssh').map((host) => {
        const policy = draft.hosts.find((item) => item.hostId === host.id)
        return <fieldset key={host.id} disabled={busy} className="border-t border-[var(--border)] py-3 space-y-2"><legend>{host.label}</legend>
          <label className="flex gap-2 items-center"><input type="checkbox" aria-label={`Route to ${host.label}`} checked={policy?.enabled ?? false} onChange={(event) => update(host.id, (item) => ({ ...item, enabled: event.target.checked }))} />Use this computer</label>
          <label className="flex gap-2 items-center">Concurrent sessions<input className="bg-black border border-[var(--border)] w-20 p-1" type="number" min={1} max={1024} aria-label={`Concurrent sessions on ${host.label}`} value={policy?.maxActiveTurns ?? 1} onChange={(event) => update(host.id, (item) => ({ ...item, maxActiveTurns: Number(event.target.value) }))} /></label>
          <p>Confirm each provider after signing in on this computer.</p>
          {providers.filter((provider) => provider.supportsStructured !== false || provider.supportsCli !== false).map((provider) => <label key={provider.id} className="flex gap-2 items-center"><input type="checkbox" aria-label={`${provider.name} signed in on ${host.label}`} checked={confirmed(policy, host.id, provider.id)} onChange={(event) => update(host.id, (item) => ({ ...item, providers: event.target.checked ? [...item.providers.filter((entry) => entry.providerId !== provider.id), { providerId: provider.id, connectionIdentity: '', providerConfigurationIdentity: '', confirm: true }] : item.providers.filter((entry) => entry.providerId !== provider.id) }))} />{provider.name} signed in</label>)}
          {folders.filter((folder) => folder.directory).map((source) => <label key={source.id} className="flex gap-2 items-center"><span className="flex-1 truncate" title={source.directory}>{source.name} · {source.directory}</span><select className="bg-black border border-[var(--border)] p-1 max-w-[50%]" aria-label={`Destination for ${source.name} on ${host.label}`} value={policy?.workspaceMappings.find((mapping) => mapping.sourceFolderId === source.id && mapping.sourceDirectory === source.directory)?.destinationFolderId ?? ''} onChange={(event) => update(host.id, (item) => ({ ...item, workspaceMappings: [...item.workspaceMappings.filter((mapping) => mapping.sourceFolderId !== source.id || mapping.sourceDirectory !== source.directory), ...(event.target.value ? [{ sourceFolderId: source.id, sourceDirectory: source.directory, destinationFolderId: event.target.value }] : [])] }))}><option value="">Do not route this workspace</option>{folders.filter((folder) => folder.executionHostId === host.id).map((folder) => <option key={folder.id} value={folder.id}>{folder.name} · {folder.directory}</option>)}</select></label>)}
        </fieldset>
      })}
      {error && <p role="alert" className="py-2">{error}</p>}
      <Button size="sm" disabled={busy} onClick={() => void submit()}>{busy ? 'Saving…' : 'Save routing'}</Button>
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
  return <div role="status" className="border-b border-[var(--border)] bg-black px-3 py-2 text-xs">
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
