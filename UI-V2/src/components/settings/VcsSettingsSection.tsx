import { forwardRef, useEffect, useImperativeHandle, useRef, useState, type KeyboardEvent } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { FolderTree, Pencil, Plus, X } from 'lucide-react'
import { Button, ConfirmDialog, IconButton, Notice, Overlay, Presence, SegmentedControl, Switch, usePresence } from '../ui'
import { useAppStore } from '../../store/useAppStore'
import { reportAppFailure, reportAppNotice } from '../../utils/appFailure'
import { isSvnLayoutFolder, sanitizeGitBaseBranch } from '../../store/cpp/sanitizers'
import { SaveButton, SettingRow, SettingsField, SettingsSection } from './SettingsPrimitives'

export interface VcsSettingsSectionHandle { requestLeave: (next: () => void) => void }

export const VcsSettingsSection = forwardRef<VcsSettingsSectionHandle>(function VcsSettingsSection(_props, ref) {
  const saved = useAppStore(useShallow((state) => ({
    gitEnabled: state.gitEnabled, svnEnabled: state.svnEnabled,
    gitSnapshotSvn: state.gitSnapshotSvn, gitSnapshotPlainFolders: state.gitSnapshotPlainFolders,
    gitAutoWorktree: state.gitAutoWorktree, gitDefaultBaseBranch: state.gitDefaultBaseBranch,
    gitPortMethod: state.gitPortMethod, gitPullBeforeChat: state.gitPullBeforeChat, svnUpdateBeforeChat: state.svnUpdateBeforeChat,
    svnRepositoryLayouts: state.svnRepositoryLayouts,
  })))
  const saveSettings = useAppStore((state) => state.setVcsSettings)
  const [draft, setDraft] = useState(saved)
  const [baseline, setBaseline] = useState(saved)
  const [saving, setSaving] = useState(false)
  const [exit, setExit] = useState<(() => void) | null>(null)
  const busy = useRef(false)
  const dirty = JSON.stringify(draft) !== JSON.stringify(baseline)
  useEffect(() => { if (!saving && !dirty) { setDraft(saved); setBaseline(saved) } }, [saved, saving, dirty])
  useEffect(() => {
    if (!dirty) return
    const guard = (event: BeforeUnloadEvent) => { event.preventDefault(); event.returnValue = '' }
    window.addEventListener('beforeunload', guard)
    return () => window.removeEventListener('beforeunload', guard)
  }, [dirty])
  useImperativeHandle(ref, () => ({ requestLeave: (next) => { if (busy.current) return; if (dirty) setExit(() => next); else next() } }))
  const edit = (patch: Partial<typeof draft>) => { setDraft((current) => ({ ...current, ...patch })) }
  const base = sanitizeGitBaseBranch(draft.gitDefaultBaseBranch)
  const validation = base === undefined ? 'Use a local branch name of up to 255 bytes, with no hidden characters.' : ''
  const layouts = draft.svnRepositoryLayouts
  type Layout = (typeof layouts)[number]
  const repositoryKey = (value: string) => value.trim().replace(/(.)\/+$/, '$1').toLowerCase()
  // One message for a layout being edited; any message blocks Apply. A saved list is always valid.
  const layoutProblem = (layout: Layout, index: number | null) => {
    const repository = layout.repository.trim().replace(/(.)\/+$/, '$1')
    // eslint-disable-next-line no-control-regex
    if (!repository || repository.length > 512 || /[\u0000-\u0020\u007f]/.test(repository)) return 'Enter the repository root URL or its UUID.'
    if (layouts.some((other, at) => at !== index && repositoryKey(other.repository) === repositoryKey(repository))) return 'This repository already has a layout.'
    if (![layout.trunk, layout.branches, layout.tags].every((folder) => isSvnLayoutFolder(folder.trim()))) return 'Use folders inside the repository, such as main or lines/dev, without a leading slash.'
    return ''
  }
  const [layoutEditor, setLayoutEditor] = useState<{ index: number | null; draft: Layout } | null>(null)
  const layoutDialogRef = useRef<HTMLDivElement>(null)
  const layoutListRef = useRef<HTMLDivElement>(null)
  const layoutEditorProblem = layoutEditor ? layoutProblem(layoutEditor.draft, layoutEditor.index) : ''
  const layoutEditorOpen = layoutEditor !== null
  // Focus goes into the editor and back to whatever opened it, as in the other settings editors.
  useEffect(() => {
    if (!layoutEditorOpen) return
    const opener = document.activeElement as HTMLElement | null
    layoutDialogRef.current?.querySelector<HTMLElement>('input')?.focus()
    return () => { if (opener?.isConnected) opener.focus() }
  }, [layoutEditorOpen])
  const applyLayout = () => {
    if (!layoutEditor || layoutEditorProblem) return
    const draftLayout = { repository: layoutEditor.draft.repository.trim().replace(/(.)\/+$/, '$1'), trunk: layoutEditor.draft.trunk.trim(), branches: layoutEditor.draft.branches.trim(), tags: layoutEditor.draft.tags.trim() }
    edit({ svnRepositoryLayouts: layoutEditor.index === null ? [...layouts, draftLayout] : layouts.map((layout, at) => at === layoutEditor.index ? draftLayout : layout) })
    setLayoutEditor(null)
  }
  const removeLayout = (index: number) => {
    edit({ svnRepositoryLayouts: layouts.filter((_, at) => at !== index) })
    // Focus moves to the next row's Edit button, or to Add repository when none is left.
    window.setTimeout(() => {
      const edits = layoutListRef.current?.closest('section')?.querySelectorAll<HTMLButtonElement>('button[aria-label^="Edit layout for"]')
      ;(edits?.[Math.min(index, (edits?.length ?? 1) - 1)] ?? layoutListRef.current?.closest('section')?.querySelector<HTMLButtonElement>('button[data-add-layout]'))?.focus()
    })
  }
  const trapLayoutFocus = (event: KeyboardEvent<HTMLDivElement>) => {
    if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); setLayoutEditor(null); return }
    if (event.key !== 'Tab') return
    const focusable = Array.from(layoutDialogRef.current?.querySelectorAll<HTMLElement>('input, button:not([disabled])') ?? [])
    if (!focusable.length) return
    const first = focusable[0], last = focusable[focusable.length - 1]
    if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus() }
    else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus() }
  }
  const layoutInvalid = layouts.some((layout, index) => Boolean(layoutProblem(layout, index)))
  const validationNotice = usePresence(Boolean(validation))
  const shownValidation = useRef(validation)
  if (validation) shownValidation.current = validation
  const save = async () => {
    if (busy.current || !dirty || validation || layoutInvalid) return
    busy.current = true; setSaving(true)
    try {
      if (!await saveSettings({ ...draft, gitDefaultBaseBranch: base! })) { reportAppFailure('Could not save VCS settings.', 'Your changes are retained.'); return }
      const current = useAppStore.getState()
      const accepted = { gitEnabled: current.gitEnabled, svnEnabled: current.svnEnabled,
        gitSnapshotSvn: current.gitSnapshotSvn, gitSnapshotPlainFolders: current.gitSnapshotPlainFolders, gitAutoWorktree: current.gitAutoWorktree,
        gitDefaultBaseBranch: current.gitDefaultBaseBranch, gitPortMethod: current.gitPortMethod, gitPullBeforeChat: current.gitPullBeforeChat, svnUpdateBeforeChat: current.svnUpdateBeforeChat, svnRepositoryLayouts: current.svnRepositoryLayouts }
      setDraft(accepted); setBaseline(accepted); setExit(null)
      reportAppNotice('VCS settings saved.', 'success')
    } catch { reportAppFailure('Could not save VCS settings.', 'Your changes are retained.') }
    finally { busy.current = false; setSaving(false) }
  }
  const toggle = (label: string, checked: boolean, change: (checked: boolean) => void, disabled = false) =>
    <Switch hideLabel label={label} checked={checked} disabled={saving || disabled} onChange={(event) => change(event.currentTarget.checked)} />
  return <div className="grid gap-0">
    <SettingsSection title="Git" actions={<>
        <Button size="sm" disabled={saving || !dirty} onClick={() => { setDraft(saved); setBaseline(saved) }}>Discard</Button>
        <SaveButton saving={saving} disabled={saving || !dirty || Boolean(validation) || layoutInvalid} onClick={() => void save()}>Save VCS settings</SaveButton>
      </>}>
      <SettingRow title="Enable Git" control={toggle('Enable Git', draft.gitEnabled, (value) => edit({ gitEnabled: value }))} />
      <SettingRow title="Automatic isolation" description="Separate worktree before each new chat." control={toggle('Automatic isolation', draft.gitAutoWorktree, (value) => edit({ gitAutoWorktree: value }), !draft.gitEnabled)} />
      <SettingRow title="Default base branch" description="Empty uses HEAD. Must exist locally." control={<input className="uam-field" aria-label="Default local Git base branch" placeholder="HEAD" value={draft.gitDefaultBaseBranch} disabled={saving || !draft.gitEnabled} spellCheck={false} onChange={(event) => edit({ gitDefaultBaseBranch: event.currentTarget.value })} />}>
        {validationNotice.mounted && <div className="uam-presence" data-state={validationNotice.closing ? 'closed' : 'open'}><Notice tone="error" dismissible={false} dismissLabel="Dismiss">{shownValidation.current}</Notice></div>}
      </SettingRow>
      <SettingRow title="Port changes" description="Merge and Squash stage changes for you to commit."
        control={<SegmentedControl aria-label="Port method" disabled={saving || !draft.gitEnabled} value={draft.gitPortMethod}
          options={[{ value: 'patch', label: 'Patch' }, { value: 'merge', label: 'Merge' }, { value: 'squash', label: 'Squash' }]}
          onChange={(method) => edit({ gitPortMethod: method })} />} />
      <SettingRow title="Pull before first chat" description="Fast-forward only, on a clean tree with an upstream." control={toggle('Pull before first chat', draft.gitPullBeforeChat, (value) => edit({ gitPullBeforeChat: value }), !draft.gitEnabled || !draft.gitAutoWorktree)} />
      <SettingRow title="Enable local Git files" description="Off means new chats edit the original folder directly." control={toggle('Enable local Git files', draft.gitSnapshotPlainFolders, (value) => edit({ gitSnapshotPlainFolders: value }))} />
    </SettingsSection>
    <SettingsSection title="SVN">
      <SettingRow title="Enable SVN" control={toggle('Enable SVN', draft.svnEnabled, (value) => edit({ svnEnabled: value }))} />
      <SettingRow title="Update before new chat" description="Run svn update before a new chat." control={toggle('Update before new chat', draft.svnUpdateBeforeChat, (value) => edit({ svnUpdateBeforeChat: value }), !draft.svnEnabled)} />
      <SettingRow title="Local snapshot Git" description="Off means new chats edit the original working copy." control={toggle('Local snapshot Git for SVN', draft.gitSnapshotSvn, (value) => edit({ gitSnapshotSvn: value }))} />
    </SettingsSection>
    <SettingsSection title="SVN repository layouts" actions={
      <Button size="sm" data-add-layout leadingIcon={<Plus size={14} aria-hidden />} disabled={saving || layouts.length >= 64} onClick={() => setLayoutEditor({ index: null, draft: { repository: '', trunk: 'trunk', branches: 'branches', tags: 'tags' } })}>Add repository</Button>}>
      <div ref={layoutListRef} className="contents">
        {layouts.length === 0 && <SettingRow title="Standard layout for every repository" description="Add a repository that keeps trunk, branches or tags elsewhere." />}
        {layouts.map((layout, index) => <SettingRow key={layout.repository} icon={<FolderTree size={15} aria-hidden />} title={layout.repository} description={`${layout.trunk} · ${layout.branches} · ${layout.tags}`}
          control={<>
            <IconButton size="sm" icon={<Pencil size={14} />} label={`Edit layout for ${layout.repository}`} disabled={saving} onClick={() => setLayoutEditor({ index, draft: layout })} />
            <IconButton size="sm" icon={<X size={14} />} label={`Remove layout for ${layout.repository}`} disabled={saving} onClick={() => removeLayout(index)} />
          </>} />)}
      </div>
    </SettingsSection>
    <Presence open={layoutEditor !== null}>
      <Overlay open zIndex={60} onBackdropMouseDown={() => setLayoutEditor(null)}>
        {layoutEditor && <div role="dialog" aria-modal="true" aria-label={layoutEditor.index === null ? 'New SVN repository layout' : 'Edit SVN repository layout'} tabIndex={-1} className="uam-mcp-dialog uam-central-dialog" ref={layoutDialogRef} onKeyDown={trapLayoutFocus}>
          <header className="uam-central-dialog__head">
            <span className="text-sm font-semibold">{layoutEditor.index === null ? 'New repository layout' : 'Edit repository layout'}</span>
            <IconButton size="sm" icon={<X size={14} />} label="Close repository layout editor" onClick={() => setLayoutEditor(null)} />
          </header>
          <div className="uam-central-dialog__body">
            {([
              ['repository', 'Repository root URL or UUID', 'https://svn.example.com/repo'],
              ['trunk', 'Main line folder', 'trunk'],
              ['branches', 'Branches folder', 'branches'],
              ['tags', 'Tags folder', 'tags'],
            ] as const).map(([field, label, placeholder]) => (
              <SettingsField key={field} label={label} htmlFor={`svn-layout-${field}`}>
                <input id={`svn-layout-${field}`} className="uam-field w-full" maxLength={512} placeholder={placeholder} spellCheck={false}
                  aria-invalid={Boolean(layoutEditorProblem) || undefined} aria-describedby={layoutEditorProblem ? 'svn-layout-problem' : undefined}
                  value={layoutEditor.draft[field]} onChange={(event) => { const value = event.currentTarget.value; setLayoutEditor((current) => current && { ...current, draft: { ...current.draft, [field]: value } }) }} />
              </SettingsField>
            ))}
            {layoutEditorProblem && <p id="svn-layout-problem" className="text-xs" style={{ color: 'var(--error)' }}>{layoutEditorProblem}</p>}
          </div>
          <footer className="uam-central-dialog__foot">
            <div className="flex items-center justify-end gap-2">
              <Button size="sm" onClick={() => setLayoutEditor(null)}>Cancel</Button>
              <Button size="sm" variant="primary" disabled={Boolean(layoutEditorProblem)} onClick={applyLayout}>{layoutEditor.index === null ? 'Add layout' : 'Apply'}</Button>
            </div>
          </footer>
        </div>}
      </Overlay>
    </Presence>
    <ConfirmDialog open={Boolean(exit)} busy={saving} title="Discard unsaved VCS changes?" confirmLabel="Discard and leave" cancelLabel="Keep editing"
      onCancel={() => setExit(null)}
      onConfirm={() => { const next = exit; setDraft(saved); setBaseline(saved); setExit(null); next?.() }}>
      Your unsaved Git, SVN and VCS changes will be lost.
    </ConfirmDialog>
  </div>
})
