import { forwardRef, useEffect, useId, useImperativeHandle, useRef, useState } from 'react'
import { ChevronRight } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { sendToCEF } from '../../ipc/cefBridge'
import { routingConfigurationError, sanitizeRepositoryMatches, shortRepositoryIdentity, type ExecutionRoutingSettings as RoutingSettings, type RoutingHostPolicy, type RoutingWorkspaceMapping, type WorkspaceRepositoryMatches } from '../../types/executionRouting'
import type { ExecutionHost, Folder } from '../../types/session'
import { Button, StatusDot, Switch, type StatusTone } from '../ui'
import { SettingRow, SettingsField, SettingsLoading, SettingsSection } from './SettingsPrimitives'
import { UnsavedChangesDialog } from './UnsavedChangesDialog'
import '../shared/review.css'
import '../modals/ExecutionRoutingDialog.css'
import { isSshHost } from '../../utils/remoteWorkspace'

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

export interface ExecutionRoutingSettingsHandle { requestLeave: (next: () => void) => void }

/** The routing capability flag is read-only from the backend and is never sent back. */
function savePayload(settings: RoutingSettings) {
  const { repositoryMatching: _capability, ...payload } = settings
  return payload
}

/**
 * SSH routing configuration, shared by the New chat dialog ("dialog") and the Settings page ("page").
 * The dialog closes on Cancel or a successful save; the page keeps Discard/Save and guards leaving with unsaved edits.
 */
export const ExecutionRoutingSettings = /* @__PURE__ */ forwardRef<ExecutionRoutingSettingsHandle, { variant: 'dialog' | 'page'; onClose?: () => void; onBusyChange?: (busy: boolean) => void }>(
  function ExecutionRoutingSettings({ variant, onClose, onBusyChange }, ref) {
    const saved = useAppStore((state) => state.executionRouting)
    const hosts = useAppStore((state) => state.executionHosts)
    const folders = useAppStore((state) => state.folders)
    const providers = useAppStore((state) => state.providers)
    const readiness = useAppStore((state) => state.cliVersionManager.remoteProviders)
    const save = useAppStore((state) => state.setExecutionRoutingSettings)
    const [draft, setDraft] = useState<RoutingSettings>(() => structuredClone(saved))
    const [baseline, setBaseline] = useState(saved)
    const savedRef = useRef(saved)
    const [busy, setBusy] = useState(false)
    const busyRef = useRef(false)
    const [error, setError] = useState('')
    const [exit, setExit] = useState<(() => void) | null>(null)
    const [repositories, setRepositories] = useState<WorkspaceRepositoryMatches[] | null>(null)
    const [repositoryError, setRepositoryError] = useState('')
    const [picking, setPicking] = useState('')
    const matching = saved.repositoryMatching === true
    const dirty = JSON.stringify(savePayload(draft)) !== JSON.stringify(savePayload(baseline))
    const validationError = routingConfigurationError(draft, hosts, folders)
    useEffect(() => {
      if (JSON.stringify(savedRef.current) === JSON.stringify(saved)) return
      savedRef.current = saved
      if (!busyRef.current) { setBaseline(saved); if (!dirty) setDraft(structuredClone(saved)) }
    }, [saved])
    useEffect(() => {
      if (!matching) return
      let cancelled = false
      void sendToCEF({ action: 'listRepositoryMatches', payload: {} }).then((response) => {
        if (cancelled) return
        if (response.ok) { setRepositories(sanitizeRepositoryMatches(response.data)); setRepositoryError('') }
        else setRepositoryError(response.error || 'Repository matching is unavailable right now.')
      })
      return () => { cancelled = true }
    }, [matching])
    useEffect(() => {
      if (variant !== 'page' || !dirty) return
      const guard = (event: BeforeUnloadEvent) => { event.preventDefault(); event.returnValue = '' }
      window.addEventListener('beforeunload', guard)
      return () => window.removeEventListener('beforeunload', guard)
    }, [variant, dirty])
    useImperativeHandle(ref, () => ({ requestLeave: (next) => { if (busyRef.current) return; if (dirty) setExit(() => next); else next() } }))
    function update(hostId: string, change: (policy: RoutingHostPolicy) => RoutingHostPolicy) {
      setDraft((current) => {
        const policy = current.hosts.find((host) => host.hostId === hostId) ?? { hostId, enabled: false, maxActiveTurns: 1, providers: [], workspaceMappings: [] }
        const next = change(policy)
        const implicitDefault = !saved.hosts.some((host) => host.hostId === hostId) && !next.enabled && next.maxActiveTurns === 1 && next.providers.length === 0 && next.workspaceMappings.length === 0
        const existing = current.hosts.some((host) => host.hostId === hostId)
        return { ...current, hosts: implicitDefault ? current.hosts.filter((host) => host.hostId !== hostId) : existing ? current.hosts.map((host) => host.hostId === hostId ? next : host) : [...current.hosts, next] }
      })
      setError('')
    }
    function confirmed(policy: RoutingHostPolicy | undefined, hostId: string, providerId: string) {
      const entry = policy?.providers.find((provider) => provider.providerId === providerId)
      const current = readiness?.find((provider) => provider.providerId === providerId && provider.executionHostId === hostId)
      return Boolean(entry?.confirm || (entry && current?.routingConnectionIdentity && current.routingProviderConfigurationIdentity &&
        entry.connectionIdentity === current.routingConnectionIdentity && entry.providerConfigurationIdentity === current.routingProviderConfigurationIdentity))
    }
    async function submit(next?: () => void) {
      if (busyRef.current || !dirty) return
      const validation = routingConfigurationError(draft, hosts, folders)
      if (validation) { setError(validation); return }
      const beforeSave = useAppStore.getState().executionRouting
      busyRef.current = true; setBusy(true); onBusyChange?.(true); setError('')
      try {
        await save(savePayload(draft))
        // Keep the acknowledged confirmation intent while native replaces it with connection identities.
        savedRef.current = useAppStore.getState().executionRouting
        setBaseline(draft)
        setExit(null); next?.()
      }
      catch (failure) {
        const currentSaved = useAppStore.getState().executionRouting
        if (JSON.stringify(currentSaved) !== JSON.stringify(beforeSave)) setBaseline(currentSaved)
        setError(failure instanceof Error ? failure.message : 'Routing settings could not be saved.')
      }
      finally { busyRef.current = false; setBusy(false); onBusyChange?.(false) }
    }
    const sshHosts = hosts.filter((host) => isSshHost(host))
    const localFolders = folders.filter((folder) => folder.directory && (!matching || !folder.executionHostId || folder.executionHostId === 'local'))
    const repositoryOf = (folderId: string) => repositories?.find((item) => item.folderId === folderId)
    /** Manual override wins; otherwise the repository match from the backend. */
    function resolveRow(policy: RoutingHostPolicy | undefined, source: Folder, hostId: string): { override?: Folder; automatic?: string } {
      const mapping = policy?.workspaceMappings.find((item) => item.sourceFolderId === source.id && item.sourceDirectory === source.directory && !item.useAutomaticMatch)
      const override = mapping ? folders.find((folder) => folder.id === mapping.destinationFolderId) : undefined
      const automatic = repositoryOf(source.id)?.matches.find((match) => match.executionHostId === hostId && match.source === 'automatic')?.remoteDirectory ?? undefined
      return { override, automatic }
    }
    function setWorkspaceMapping(hostId: string, source: Folder, next: RoutingWorkspaceMapping | null) {
      update(hostId, (item) => {
        const index = item.workspaceMappings.findIndex((mapping) => mapping.sourceFolderId === source.id && mapping.sourceDirectory === source.directory)
        return { ...item, workspaceMappings: next ? index < 0 ? [...item.workspaceMappings, next] : item.workspaceMappings.map((mapping, position) => position === index ? next : mapping) : item.workspaceMappings.filter((_mapping, position) => position !== index) }
      })
      setPicking('')
    }
    const errorText = error || validationError ? <p role="alert" className="uam-rt-dialog__error">{error || validationError}</p> : null
    const footer = variant === 'dialog'
      ? <div className="uam-rt-dialog__foot">
          {errorText}
          <Button variant="ghost" size="md" disabled={busy} onClick={onClose}>Cancel</Button>
          <Button variant="primary" size="md" disabled={busy || !dirty || Boolean(validationError)} loading={busy} onClick={() => void submit(() => onClose?.())}>Save routing</Button>
        </div>
      : <div className="flex items-center justify-end gap-2 pt-1">
          {errorText}
          <Button size="sm" disabled={busy || !dirty} onClick={() => { setDraft(structuredClone(saved)); setBaseline(saved); setError('') }}>Discard</Button>
          <Button size="sm" variant="primary" disabled={busy || !dirty || Boolean(validationError)} loading={busy} onClick={() => void submit()}>Save routing</Button>
        </div>

    return <>
      <fieldset disabled={busy} className={variant === 'dialog' ? 'uam-rt-dialog__body' : 'grid min-w-0 gap-4 m-0 p-0 border-0'}>
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
                {matching
                  ? <Disclosure title="Remote folders" summary={`${localFolders.filter((source) => { const row = resolveRow(policy, source, host.id); return row.override || row.automatic }).length} of ${localFolders.length} matched`}>
                      <div className="grid gap-3">
                        {localFolders.length === 0 && <p className="uam-rt-dialog__empty">Give a local folder a path first, then it can be matched here.</p>}
                        {localFolders.length > 0 && repositories === null && !repositoryError && <SettingsLoading label="Checking repositories" rows={2} />}
                        {repositoryError && <p className="uam-rt-dialog__empty" role="status">Repository matching is unavailable right now. Choose folders by hand below.</p>}
                        {localFolders.map((source) => {
                          const repository = repositoryOf(source.id)?.repository
                          const { override, automatic } = resolveRow(policy, source, host.id)
                          const target = override ? override.directory : automatic
                          const pickId = `${host.id}:${source.id}`
                          return <div key={source.id} className="uam-rt-workspace">
                            <div className="uam-rt-workspace__head">
                              <span className="uam-rt-workspace__name">{source.name}</span>
                              {repository
                                ? <span className="uam-rt-alias" title={repository.identity}>{shortRepositoryIdentity(repository.identity)}</span>
                                : <span className="uam-rt-workspace__none">No repository detected</span>}
                            </div>
                            <div className="uam-rt-match">
                              <StatusDot tone={target ? (override ? 'info' : 'success') : 'warning'} size={7} />
                              {target
                                ? <span className="uam-rt-match__text">{override ? 'Chosen folder' : 'Matched by repository'} <span className="uam-rt-alias">{target}</span></span>
                                : <span className="uam-rt-match__text uam-rt-match__text--muted">No matching checkout</span>}
                              <span className="uam-rt-match__actions">
                                {override && <Button variant="ghost" size="sm" onClick={() => setWorkspaceMapping(host.id, source, { sourceFolderId: source.id, sourceDirectory: source.directory, destinationFolderId: '', useAutomaticMatch: true })}>Use automatic match</Button>}
                                <Button variant="ghost" size="sm" disabled={remoteFolders.length === 0} onClick={() => setPicking(picking === pickId ? '' : pickId)}>Choose folder…</Button>
                              </span>
                            </div>
                            {picking === pickId && <div className="uam-rt-pick">
                              <select autoFocus className="uam-rv-field w-full" aria-label={`Choose folder for ${source.name} on ${host.label}`} value={override?.id ?? ''} onChange={(event) => { if (event.target.value) setWorkspaceMapping(host.id, source, { sourceFolderId: source.id, sourceDirectory: source.directory, destinationFolderId: event.target.value }) }}>
                                <option value="">Choose a folder</option>
                                {remoteFolders.map((folder) => <option key={folder.id} value={folder.id}>{folder.name} · {folder.directory}</option>)}
                              </select>
                              <Button variant="ghost" size="sm" onClick={() => setPicking('')}>Cancel</Button>
                            </div>}
                          </div>
                        })}
                      </div>
                    </Disclosure>
                  : <Disclosure title="Remote folders" summary={localFolders.length ? `${mapped} of ${localFolders.length} mapped` : 'No folders'}>
                    <div className="grid gap-3">
                      {localFolders.length === 0 && <p className="uam-rt-dialog__empty">Give a local folder a path first, then map it here.</p>}
                      {localFolders.length > 0 && remoteFolders.length === 0 && <p className="uam-rt-dialog__empty">No folders exist on {host.label} yet, so there is nothing to map to.</p>}
                      {localFolders.map((source) => {
                        const id = `routing-map-${host.id}-${source.id}`
                        return <SettingsField key={source.id} label={source.name} htmlFor={id} hint={<span className="uam-rt-alias">{source.directory}</span>}>
                          <select id={id} className="uam-rv-field w-full" aria-label={`Destination for ${source.name} on ${host.label}`} value={policy?.workspaceMappings.find((mapping) => mapping.sourceFolderId === source.id && mapping.sourceDirectory === source.directory)?.destinationFolderId ?? ''} onChange={(event) => setWorkspaceMapping(host.id, source, event.target.value ? { sourceFolderId: source.id, sourceDirectory: source.directory, destinationFolderId: event.target.value } : null)}>
                            <option value="">Don&apos;t route this folder</option>
                            {remoteFolders.map((folder) => <option key={folder.id} value={folder.id}>{folder.name} · {folder.directory}</option>)}
                          </select>
                        </SettingsField>
                      })}
                    </div>
                  </Disclosure>}
              </div>
            </SettingsSection>
          })}
      </fieldset>
      {footer}
      {exit && <UnsavedChangesDialog title="Unsaved routing settings" busy={busy} saveDisabled={Boolean(validationError)} error={error || validationError} onStay={() => setExit(null)}
        onDiscard={() => { const next = exit; setDraft(structuredClone(saved)); setBaseline(saved); setError(''); setExit(null); next() }} onSave={() => void submit(exit)} />}
    </>
  })
