import { forwardRef, useEffect, useImperativeHandle, useRef, useState } from 'react'
import { ArrowRight, LoaderCircle, Pencil, Plus, RotateCcw, Unlink } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { boundedRead, sendToCEF, type CEFResponse } from '../../ipc/cefBridge'
import { routingConfigurationError, routingFolderRows, sanitizeRepositoryMatches, shortRepositoryIdentity, type ExecutionRoutingSettings as RoutingSettings, type RoutingFolderGroup, type RoutingHostPolicy, type RoutingWorkspaceMapping, type WorkspaceRepositoryMatches } from '../../types/executionRouting'
import type { ExecutionHost, Folder } from '../../types/session'
import { Button, Disclosure, IconButton, Presence, Switch, type StatusTone, Reveal } from '../ui'
import { NumberField } from './NumberField'
import { SaveButton, SettingRow, SettingsSection, StatusMark } from './SettingsPrimitives'
import { MapFolderChooser, type FolderMappingChoice } from './MapFolderChooser'
import { UnsavedChangesDialog } from './UnsavedChangesDialog'
import '../shared/review.css'
import '../modals/ExecutionRoutingDialog.css'
import { isSshHost } from '../../utils/remoteWorkspace'
import { reportAppFailure } from '../../utils/appFailure'

export const RUNNER_STATUS: Record<ExecutionHost['runnerStatus'], { tone: StatusTone; label: string }> = {
  ready: { tone: 'success', label: 'Ready' },
  installing: { tone: 'info', label: 'Setting up' },
  uninstalled: { tone: 'warning', label: 'Not set up' },
  offline: { tone: 'warning', label: 'Offline' },
  error: { tone: 'error', label: 'Error' },
}

const plural = (count: number, one: string, many = `${one}s`) => `${count} ${count === 1 ? one : many}`

export interface ExecutionRoutingSettingsHandle {
  requestLeave: (next: () => void) => void
  /** Closes an in-place step (the dialog's Map folder chooser); true when Escape was used for it. */
  closeStep?: () => boolean
}

/** The routing capability flag is read-only from the backend and is never sent back. */
function savePayload(settings: RoutingSettings) {
  const { repositoryMatching: _capability, ...payload } = settings
  return payload
}

/**
 * SSH routing configuration, shared by the New chat dialog ("dialog") and the Settings page ("page").
 * The dialog closes on Cancel or a successful save; the page keeps Discard/Save and guards leaving with unsaved edits.
 */
export const ExecutionRoutingSettings = /* @__PURE__ */ forwardRef<ExecutionRoutingSettingsHandle, { variant: 'dialog' | 'page'; onClose?: () => void; onBusyChange?: (busy: boolean) => void; onAddComputer?: () => void }>(
  function ExecutionRoutingSettings({ variant, onClose, onBusyChange, onAddComputer }, ref) {
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
    const [exit, setExit] = useState<(() => void) | null>(null)
    const [repositories, setRepositories] = useState<WorkspaceRepositoryMatches[] | null>(null)
    const [repositoryError, setRepositoryError] = useState('')
    const [repositoryAttempt, setRepositoryAttempt] = useState(0)
    // A slow native check holds a shared VCS-context slot; Retry rejoins it instead of starting another.
    const repositoryRead = useRef<Promise<CEFResponse> | null>(null)
    const [chooser, setChooser] = useState<{ hostId: string; folderId: string } | 'all' | null>(null)
    const chooserOpener = useRef<HTMLElement | null>(null)
    const root = useRef<HTMLFieldSetElement>(null)
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
      // A retry clears the failure so the check row shows again; earlier matches stay until replaced.
      setRepositoryError('')
      const reply = repositoryRead.current ?? sendToCEF({ action: 'listRepositoryMatches', payload: {} })
      if (!repositoryRead.current) {
        repositoryRead.current = reply
        void reply.finally(() => { if (repositoryRead.current === reply) repositoryRead.current = null })
      }
      void boundedRead(reply, 'Repository matching did not answer in time.').then((response) => {
        if (cancelled) return
        if (response.ok) { setRepositories(sanitizeRepositoryMatches(response.data)); setRepositoryError('') }
        else setRepositoryError(response.error || 'Repository matching is unavailable right now.')
      })
      return () => { cancelled = true }
    }, [matching, repositoryAttempt])
    useEffect(() => {
      if (variant !== 'page' || !dirty) return
      const guard = (event: BeforeUnloadEvent) => { event.preventDefault(); event.returnValue = '' }
      window.addEventListener('beforeunload', guard)
      return () => window.removeEventListener('beforeunload', guard)
    }, [variant, dirty])
    useImperativeHandle(ref, () => ({
      requestLeave: (next) => { if (busyRef.current) return; if (dirty) setExit(() => next); else next() },
      closeStep: () => { if (!chooser) return false; setChooser(null); return true },
    }))
    // Focus returns to whatever opened the chooser, or to Map folder… when the dialog's in-place step remounted the list.
    useEffect(() => {
      if (chooser) return
      const opener = chooserOpener.current
      if (!opener) return
      chooserOpener.current = null
      const target = opener.isConnected ? opener : root.current?.querySelector<HTMLElement>('[data-routing-map-folder]')
      target?.focus()
    }, [chooser])
    function update(hostId: string, change: (policy: RoutingHostPolicy) => RoutingHostPolicy) {
      setDraft((current) => {
        const policy = current.hosts.find((host) => host.hostId === hostId) ?? { hostId, enabled: false, maxActiveTurns: 1, providers: [], workspaceMappings: [] }
        const next = change(policy)
        const implicitDefault = !saved.hosts.some((host) => host.hostId === hostId) && !next.enabled && next.maxActiveTurns === 1 && next.providers.length === 0 && next.workspaceMappings.length === 0
        const existing = current.hosts.some((host) => host.hostId === hostId)
        return { ...current, hosts: implicitDefault ? current.hosts.filter((host) => host.hostId !== hostId) : existing ? current.hosts.map((host) => host.hostId === hostId ? next : host) : [...current.hosts, next] }
      })
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
      if (validation) return
      const beforeSave = useAppStore.getState().executionRouting
      busyRef.current = true; setBusy(true); onBusyChange?.(true)
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
        reportAppFailure('Routing settings could not be saved.', failure instanceof Error ? failure.message : 'Try again.')
      }
      finally { busyRef.current = false; setBusy(false); onBusyChange?.(false) }
    }
    const sshHosts = hosts.filter((host) => isSshHost(host))
    const localFolders = folders.filter((folder) => folder.directory && (!matching || !folder.executionHostId || folder.executionHostId === 'local'))
    const { groups, unmapped } = routingFolderRows(localFolders, sshHosts, draft, matching ? repositories : null, folders)
    /** Replaces every mapping of this workspace on the computer (including one left behind when the folder moved), in place. */
    function setWorkspaceMapping(hostId: string, source: Folder, next: RoutingWorkspaceMapping | null) {
      update(hostId, (item) => {
        const index = item.workspaceMappings.findIndex((mapping) => mapping.sourceFolderId === source.id)
        const workspaceMappings = item.workspaceMappings.filter((mapping) => mapping.sourceFolderId !== source.id)
        if (next) workspaceMappings.splice(index < 0 ? workspaceMappings.length : index, 0, next)
        return { ...item, workspaceMappings }
      })
    }
    /** Clears a chosen folder. Save replaces the whole list, so plain removal also returns the workspace to repository matching. */
    const clearMapping = (hostId: string, source: Folder) => setWorkspaceMapping(hostId, source, null)
    // A policy for a computer that is no longer a registered SSH computer blocks Save; it gets a row with Remove.
    const removedHosts = draft.hosts.filter((policy) => !sshHosts.some((host) => host.id === policy.hostId))
    function openChooser(next: { hostId: string; folderId: string } | 'all') {
      chooserOpener.current = document.activeElement as HTMLElement | null
      setChooser(next)
    }
    function applyChoices(choices: FolderMappingChoice[]) {
      for (const choice of choices) {
        const source = folders.find((folder) => folder.id === choice.folderId)
        if (source?.directory) setWorkspaceMapping(choice.hostId, source, { sourceFolderId: source.id, sourceDirectory: source.directory, destinationFolderId: choice.destinationFolderId })
      }
      setChooser(null)
    }
    const stepOpen = variant === 'dialog' && chooser !== null
    const hostLabel = (hostId: string) => sshHosts.find((host) => host.id === hostId)?.label ?? hostId
    const checking = matching && localFolders.length > 0 && repositories === null && !repositoryError
    const errorText = <Reveal open={!!validationError}><p role="alert" className="uam-rt-dialog__error">{validationError}</p></Reveal>
    const footer = variant === 'dialog'
      ? <div className="uam-rt-dialog__foot">
          {errorText}
          <Button variant="ghost" size="md" disabled={busy} onClick={onClose}>Cancel</Button>
          <SaveButton size="md" saving={busy} disabled={busy || !dirty || Boolean(validationError)} loading={busy} onClick={() => void submit(() => onClose?.())}>Save routing</SaveButton>
        </div>
      : <div className="flex items-center justify-end gap-2 pt-1">
          {errorText}
          <Button size="sm" disabled={busy || !dirty} onClick={() => { setDraft(structuredClone(saved)); setBaseline(saved) }}>Discard</Button>
          <SaveButton saving={busy} disabled={busy || !dirty || Boolean(validationError)} loading={busy} onClick={() => void submit()}>Save routing</SaveButton>
        </div>

    const providerRows = providers.filter((provider) => provider.supportsStructured !== false || provider.supportsCli !== false)
    const chooserView = chooser && <MapFolderChooser key={chooser === 'all' ? 'all' : `${chooser.hostId}:${chooser.folderId}`} inline={variant === 'dialog'}
      hosts={sshHosts} workspaces={[...groups.filter((group) => !group.orphan), ...unmapped]} folders={folders} preselect={chooser === 'all' ? undefined : chooser}
      onApply={applyChoices} onClose={() => setChooser(null)} />
    const mapRow = (group: RoutingFolderGroup) => group.entries.map((entry) => {
      const source = group.folder
      const label = hostLabel(entry.hostId)
      const choose = () => openChooser({ hostId: entry.hostId, folderId: source.id })
      const canUseMatch = matching && !group.orphan && entry.kind === 'chosen' && repositories?.some((item) => item.folderId === source.id && item.matches.some((match) => match.executionHostId === entry.hostId && match.source === 'automatic' && match.remoteDirectory))
      return <div key={entry.hostId} className="uam-rt-map-row" data-problem={entry.error ? 'true' : undefined}>
        <StatusMark tone={entry.error ? 'error' : 'success'} label={entry.error ? 'Problem' : 'Mapped'} size={7} />
        <span className="uam-rt-map-row__host">{label}</span>
        <ArrowRight size={14} aria-hidden className="uam-rt-map-row__arrow" />
        <span className="uam-rt-alias uam-rt-map-row__path" title={entry.target}>{entry.target}</span>
        {entry.error
          ? <span className="uam-rt-map-row__error">{entry.error}</span>
          : <span className="uam-rt-tag">{entry.kind === 'chosen' ? 'Chosen' : 'Same repository'}</span>}
        <span className="uam-rt-map-row__actions">
          {group.orphan ? null : entry.error
            ? <Button variant="ghost" size="sm" aria-label={`Choose folder for ${source.name} on ${label}`} onClick={choose}>Choose…</Button>
            : <IconButton size="sm" icon={<Pencil size={14} />} label={`Choose a different folder for ${source.name} on ${label}`} tooltip="Choose a different folder" onClick={choose} />}
          {(entry.kind === 'chosen' || entry.error) && (canUseMatch
            ? <IconButton size="sm" icon={<RotateCcw size={14} />} label={`Use repository match for ${source.name} on ${label}`} tooltip="Use repository match" onClick={() => clearMapping(entry.hostId, source)} />
            : <IconButton size="sm" icon={<Unlink size={14} />} label={`Remove mapping for ${source.name} on ${label}`} tooltip="Remove mapping" onClick={() => clearMapping(entry.hostId, source)} />)}
        </span>
      </div>
    })

    return <>
      {/* The dialog's chooser step hides the list without unmounting it, so disclosures keep their open state and focus returns to the opener. */}
      <Reveal open={!(variant === 'dialog' && exit)} className="grid min-w-0 gap-0"><div className={stepOpen ? 'hidden' : 'contents'} {...(stepOpen ? { inert: '' } : {})}><fieldset ref={root} disabled={busy} className={variant === 'dialog' ? 'uam-rt-dialog__body' : 'grid min-w-0 gap-0 m-0 p-0 border-0'}>
        <SettingsSection title="Automatic routing" actions={variant === 'page' ? footer : undefined}>
          <SettingRow htmlFor="routing-enabled" title="Route new chats automatically" description="Only computers you allow below, with folders mapped, are used."
            control={<Switch id="routing-enabled" hideLabel label="Enable automatic routing" checked={draft.enabled} onChange={(event) => setDraft({ ...draft, enabled: event.target.checked })} />} />
        </SettingsSection>

        <SettingsSection title="Computers" actions={onAddComputer && <Button size="sm" leadingIcon={<Plus size={13} />} onClick={onAddComputer}>Add computer</Button>}>
          <Reveal open={sshHosts.length === 0}><SettingRow title={<span className="uam-rt-dialog__empty">No SSH computers yet.</span>} /></Reveal>
          {removedHosts.map((policy) => <SettingRow key={policy.hostId}
            title={<span className="inline-flex items-center gap-2"><StatusMark tone="error" label="Problem" size={7} /><span>Removed computer</span><span className="uam-rt-alias">{hosts.find((host) => host.id === policy.hostId)?.label ?? policy.hostId}</span></span>}
            description={<span className="uam-rt-map-row__error">Computer removed</span>}
            control={<Button variant="ghost" size="sm" leadingIcon={<Unlink size={13} />} aria-label={`Remove routing for removed computer ${policy.hostId}`}
              onClick={() => setDraft((current) => ({ ...current, hosts: current.hosts.filter((host) => host.hostId !== policy.hostId) }))}>Remove</Button>} />)}
          {sshHosts.map((host) => {
            const policy = draft.hosts.find((item) => item.hostId === host.id)
            const look = RUNNER_STATUS[host.runnerStatus]
            const signedIn = providerRows.filter((provider) => confirmed(policy, host.id, provider.id)).length
            const turns = policy?.maxActiveTurns ?? 1
            // ui: the Use this computer switch cannot sit inside the disclosure button, so it overlays the trigger row's right edge.
            return <div key={host.id} className="uam-rt-host">
              <Disclosure summary={<>
                <span className="uam-set-row__title">{host.label}</span>
                <span className="uam-rt-alias">{host.sshAlias}</span>
                <span className="uam-rt-summary">{providerRows.length ? `${signedIn} of ${providerRows.length} signed in · ` : ''}{plural(turns, 'chat')} at once</span>
                <span className="uam-rt-status"><StatusMark tone={look.tone} label={look.label} size={7} /></span>
              </>}>
                <SettingRow title="Concurrent sessions" description="How many chats can run here at the same time."
                  control={<NumberField label={`Concurrent sessions on ${host.label}`} value={turns} min={1} max={1024} onCommit={(value) => update(host.id, (item) => ({ ...item, maxActiveTurns: value }))} />} />
                {providerRows.map((provider) => (
                  <SettingRow key={provider.id} title={provider.name} description={`Confirm once you have signed in to ${provider.name} on this computer.`}
                    control={<Switch hideLabel label={`${provider.name} signed in on ${host.label}`} checked={confirmed(policy, host.id, provider.id)} onChange={(event) => update(host.id, (item) => ({ ...item, providers: event.target.checked ? [...item.providers.filter((entry) => entry.providerId !== provider.id), { providerId: provider.id, connectionIdentity: '', providerConfigurationIdentity: '', confirm: true }] : item.providers.filter((entry) => entry.providerId !== provider.id) }))} />} />
                ))}
              </Disclosure>
              <span className="uam-rt-host__switch"><Switch hideLabel label={`Route to ${host.label}`} checked={policy?.enabled ?? false} onChange={(event) => update(host.id, (item) => ({ ...item, enabled: event.target.checked }))} /></span>
            </div>
          })}
        </SettingsSection>

        {/* Folders stay usable while repositories are checked; manual mapping never waits for matching. */}
        <Reveal open={sshHosts.length > 0}>
          <SettingsSection title="Folders" className="uam-rt-folders" actions={<>
            <Reveal open={localFolders.length > 0 && !checking} as="span" className="uam-rt-summary">{groups.filter((group) => group.entries.some((entry) => !entry.error)).length} of {plural(localFolders.length, 'workspace')} mapped</Reveal>
            <Button size="sm" leadingIcon={<Plus size={13} />} data-routing-map-folder disabled={localFolders.length === 0} onClick={() => openChooser('all')}>Map folder…</Button>
          </>}>
            <Reveal open={localFolders.length === 0}><SettingRow title={<span className="uam-rt-dialog__empty">Give a local folder a path first, then it can be mapped here.</span>} /></Reveal>
            <Reveal open={checking}><SettingRow title={<span className="inline-flex items-center gap-1.5" role="status"><LoaderCircle size={13} aria-hidden className="animate-spin motion-reduce:animate-none" />Checking repositories… You can map folders by hand meanwhile.</span>} /></Reveal>
            <Reveal open={matching && localFolders.length > 0 && !!repositoryError}><SettingRow title={<span className="uam-rt-dialog__empty" role="status">Repository matching is unavailable right now. Choose folders by hand.</span>}
              control={<Button size="sm" onClick={() => { root.current?.querySelector<HTMLElement>('[data-routing-map-folder]')?.focus({ preventScroll: true }); setRepositoryAttempt((value) => value + 1) }}>Retry</Button>} /></Reveal>
            {groups.map((group, index) => {
              const problems = group.entries.filter((entry) => entry.error)
              return <Disclosure key={group.folder.id} defaultOpen={problems.length > 0 || index < 2} summary={<>
                <span className="uam-set-row__title">{group.folder.name}</span>
                {group.repository && <span className="uam-rt-alias" title={group.repository.identity}>{shortRepositoryIdentity(group.repository.identity)}</span>}
                {problems.length
                  ? <span className="uam-rt-summary uam-rt-summary--error"><StatusMark tone="error" label="Problem" size={7} />{problems.length === 1 ? problems[0].error : plural(problems.length, 'problem')}</span>
                  : <span className="uam-rt-summary">{plural(group.entries.length, 'computer')}</span>}
              </>}>{mapRow(group)}</Disclosure>
            })}
            <Reveal open={unmapped.length > 0}><Disclosure summary={<>
              <span className="uam-set-row__title uam-rt-muted-title">Not mapped</span>
              <span className="uam-rt-summary">{plural(unmapped.length, 'workspace')} without a remote folder</span>
            </>}>
              {unmapped.map((group) => <div key={group.folder.id} className="uam-rt-map-row uam-rt-map-row--unmapped">
                <span className="uam-rt-map-row__host">{group.folder.name}</span>
                <span className="uam-rt-alias uam-rt-map-row__path" title={group.folder.directory}>{group.folder.directory}{matching ? ` · ${group.repository ? shortRepositoryIdentity(group.repository.identity) : checking ? 'Checking repository…' : repositoryError ? 'Repository unknown' : 'No repository'}` : ''}</span>
                <span className="uam-rt-map-row__actions"><Button variant="ghost" size="sm" aria-label={`Map ${group.folder.name}`} onClick={() => openChooser({ hostId: sshHosts[0].id, folderId: group.folder.id })}>Map…</Button></span>
              </div>)}
            </Disclosure></Reveal>
          </SettingsSection>
        </Reveal>
      </fieldset>
      {variant === 'dialog' && footer}</div></Reveal>
      {/* The dialog never opens a second dialog: the Map folder chooser is an in-place step in the same panel. */}
      <Reveal open={variant === 'dialog' && chooser !== null && !exit} className="uam-rt-dialog__step">{variant === 'dialog' && chooserView}</Reveal>
      {variant === 'page' && <Presence open={chooser !== null}>{chooserView}</Presence>}
      {/* The dialog never opens a second dialog: leaving with unsaved edits is an in-place step in the same panel. */}
      <Reveal open={variant === 'dialog' && exit !== null} className="uam-rt-dialog__body">
        {exit && <div role="group" aria-label="Unsaved routing settings">
            <p className="m-0 text-sm font-semibold">Unsaved routing settings</p>
            <p className="m-0 text-sm" style={{ color: 'var(--text-2)' }}>Your changes have not been saved.</p>
            <Reveal open={!!(validationError)}><p role="alert" className="uam-rt-dialog__error">{validationError}</p></Reveal>
            <div className="uam-rt-dialog__foot">
              <Button variant="ghost" size="md" disabled={busy} onClick={() => setExit(null)}>Keep editing</Button>
              <Button variant="secondary" size="md" disabled={busy} onClick={() => { const next = exit; setDraft(structuredClone(saved)); setBaseline(saved); setExit(null); next() }}>Discard</Button>
              <SaveButton size="md" saving={busy} disabled={busy || !dirty || Boolean(validationError)} loading={busy} onClick={() => void submit(exit)}>Save and leave</SaveButton>
            </div>
          </div>}
      </Reveal>
      {variant === 'page' && <Presence open={exit !== null}>{exit && <UnsavedChangesDialog title="Unsaved routing settings" busy={busy} saveDisabled={Boolean(validationError)} error={validationError || undefined} onStay={() => setExit(null)}
        onDiscard={() => { const next = exit; setDraft(structuredClone(saved)); setBaseline(saved); setExit(null); next() }} onSave={() => void submit(exit)} />}</Presence>}
    </>
  })
