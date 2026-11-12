import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { ArrowDownToLine, CheckCircle2, ChevronRight, Circle, FolderGit2, GitBranch, GitCommitHorizontal, History, LoaderCircle, MessageSquareDiff, PanelRightClose, RotateCw, Sparkles, TriangleAlert } from 'lucide-react'
import { useAppStore, type VcsChangedFile, type VcsCommitStatus, type VcsType } from '../../store/useAppStore'
import type { VcsUpdateResult } from '../../store/cpp/types'
import { Button, IconButton, MenuSelect, Presence, RowButton, SectionHeader, Tab, Tabs } from '../ui'
import { RepositoryDiffDialog } from '../shared/RepositoryDiffDialog'
import { RecordedChatEdits } from './RecordedChatEdits'
import { VcsHistory } from './VcsHistory'
import { CODEX_ENABLED } from '../../config/buildFeatures'
import '../shared/review.css'

/** Stacked add/remove bar; width is relative to the largest change in the list. */
function ChangeBar({ additions, deletions, max }: { additions: number; deletions: number; max: number }) {
  return <span className="uam-rv-bar" aria-hidden>
    <span className="uam-rv-bar__fill" style={{ width: `${((additions + deletions) / max) * 100}%` }}>
      <span className="uam-rv-bar__add" style={{ flexGrow: additions }} />
      <span className="uam-rv-bar__del" style={{ flexGrow: deletions }} />
    </span>
  </span>
}

function emptyStatus(workspaceDirectory = ''): VcsCommitStatus {
  return {
    available: false,
    vcsTypes: [],
    activeVcsType: 'git',
    workspaceDirectory,
    branchOrRevision: '',
    changedFiles: [],
    lineStatsReady: true,
    warning: 'No Git or SVN repository detected for this workspace.',
    error: '',
  }
}

function statusColor(status: string) {
  const code = status.trim().charAt(0)
  return code === 'A' || code === '?' ? 'var(--success)' : code === 'D' ? 'var(--error)' : code === 'R' ? 'var(--info)' : 'var(--warning)'
}

/** Updates still running, by chat: the panel is per selected chat, but an update outlives a switch to another chat. */
const updatesInFlight = new Map<string, { requestId: string; result: Promise<VcsUpdateResult> }>()

export function VcsCommitPanel() {
  const gitEnabled = useAppStore((s) => s.gitEnabled)
  const svnEnabled = useAppStore((s) => s.svnEnabled)
  const activeSessionId = useAppStore((s) => s.activeSessionId)
  const session = useAppStore((s) => s.sessions.find((candidate) => candidate.id === s.activeSessionId) ?? null)
  const getVcsCommitStatus = useAppStore((s) => s.getVcsCommitStatus)
  const commitVcsChanges = useAppStore((s) => s.commitVcsChanges)
  const generateVcsCommitMessage = useAppStore((s) => s.generateVcsCommitMessage)
  const setCommitPanelOpen = useAppStore((s) => s.setCommitPanelOpen)
  const getVcsFileDiff = useAppStore((s) => s.getVcsFileDiff)
  const updateVcsWorkspace = useAppStore((s) => s.updateVcsWorkspace)
  const cancelVcsRequest = useAppStore((s) => s.cancelVcsRequest)
  const [diffSelection, setDiffSelection] = useState<{ file: VcsChangedFile; chatId: string; workspaceDirectory: string | undefined; workspaceWorktreeDirectory: string | undefined; executionHostId: string | undefined } | null>(null)
  const [reviewScope, setReviewScope] = useState<'workspace' | 'recorded' | 'history'>('workspace')
  const [status, setStatus] = useState<VcsCommitStatus>(() => emptyStatus(session?.workspaceDirectory ?? ''))
  const [selectedVcsType, setSelectedVcsType] = useState<VcsType>('git')
  const [selectedFiles, setSelectedFiles] = useState<string[]>([])
  const [title, setTitle] = useState('')
  const [description, setDescription] = useState('')
  const [loading, setLoading] = useState(false)
  const [committing, setCommitting] = useState(false)
  const [generating, setGenerating] = useState(false)
  const [notice, setNotice] = useState('')
  const [noticeTone, setNoticeTone] = useState<'success' | 'warning' | 'error'>('error')
  const [updating, setUpdating] = useState(false)
  // Bumped after an update so History reloads against the new working-copy revision.
  const [historyEpoch, setHistoryEpoch] = useState(0)
  const latestUpdateRequestRef = useRef(0)
  const updateRequestIdRef = useRef('')
  const [commitOutput, setCommitOutput] = useState('')
  const latestStatusRequestRef = useRef('')
  const latestGenerateRequestRef = useRef(0)
  const latestCommitRequestRef = useRef(0)
  const draftRevisionRef = useRef(0)

  useEffect(() => {
    latestStatusRequestRef.current = ''
    latestGenerateRequestRef.current += 1
    latestCommitRequestRef.current += 1
    latestUpdateRequestRef.current += 1
    updateRequestIdRef.current = ''
    setUpdating(false)
    draftRevisionRef.current += 1
    setStatus(emptyStatus(session?.workspaceDirectory ?? ''))
    setDiffSelection(null)
    setSelectedFiles([])
    setTitle('')
    setDescription('')
    setNotice('')
    setCommitOutput('')
    setLoading(false)
    setGenerating(false)
    setCommitting(false)
  }, [activeSessionId, session?.workspaceDirectory, session?.workspaceWorktreeDirectory, session?.executionHostId])

  useEffect(() => {
    latestStatusRequestRef.current = ''
    latestGenerateRequestRef.current += 1
    setStatus(emptyStatus(session?.workspaceDirectory ?? ''))
    setDiffSelection(null)
    setLoading(false)
    setGenerating(false)
  }, [gitEnabled, svnEnabled])

  const refresh = useCallback(async (vcsType = selectedVcsType, includeLineStats = false) => {
    if (!activeSessionId) {
      setStatus(emptyStatus())
      return
    }
    const requestKey = `${activeSessionId}:${vcsType}:${Date.now()}`
    latestStatusRequestRef.current = requestKey
    setLoading(true)
    const next = await getVcsCommitStatus(activeSessionId, vcsType, { includeLineStats, requestId: requestKey })
    if (latestStatusRequestRef.current !== requestKey) return
    setLoading(false)
    const effective = next ?? emptyStatus(session?.workspaceDirectory ?? '')
    setStatus(effective)
    setSelectedVcsType(effective.activeVcsType)
    setSelectedFiles((current) => current.filter((file) => effective.changedFiles.some((changed) => changed.path === file)))
    if (!includeLineStats && effective.available && effective.changedFiles.length > 0 && !effective.lineStatsReady) {
      const detailed = await getVcsCommitStatus(activeSessionId, effective.activeVcsType, {
        includeLineStats: true,
        requestId: `${requestKey}:stats`,
      })
      if (latestStatusRequestRef.current !== requestKey) return
      if (detailed) {
        setStatus(detailed)
        setSelectedFiles((current) => current.filter((file) => detailed.changedFiles.some((changed) => changed.path === file)))
      }
    }
  }, [activeSessionId, getVcsCommitStatus, selectedVcsType, session?.workspaceDirectory, session?.workspaceWorktreeDirectory, session?.executionHostId, gitEnabled, svnEnabled])

  useEffect(() => {
    void refresh()
  }, [refresh])

  const commitMessage = [title.trim(), description.trim()].filter(Boolean).join('\n\n')
  const systemEnabled = selectedVcsType === 'git' ? gitEnabled : svnEnabled
  const commitDisabled = !systemEnabled || committing || updating || !status.available || selectedFiles.length === 0 || title.trim().length === 0
  const generateDisabled = !systemEnabled || generating || !status.available || selectedFiles.length === 0
  const selectedFileSet = useMemo(() => new Set(selectedFiles), [selectedFiles])
  const allSelected = status.changedFiles.length > 0 && status.changedFiles.every((file) => selectedFileSet.has(file.path))
  const lineStatsReady = status.lineStatsReady !== false
  const maxChange = Math.max(1, ...status.changedFiles.map((file) => file.additions + file.deletions))

  const toggleFile = (path: string) => {
    setSelectedFiles((current) =>
      current.includes(path) ? current.filter((candidate) => candidate !== path) : [...current, path]
    )
  }

  const toggleAllFiles = () => {
    setSelectedFiles(allSelected ? [] : status.changedFiles.map((file) => file.path))
  }

  const generateMessage = async () => {
    if (!activeSessionId || generateDisabled) return
    const sourceSessionId = activeSessionId
    const request = ++latestGenerateRequestRef.current
    const draftRevision = draftRevisionRef.current
    setGenerating(true)
    setNotice('')
    const suggestion = await generateVcsCommitMessage(sourceSessionId, selectedVcsType, selectedFiles)
    if (latestGenerateRequestRef.current !== request || useAppStore.getState().activeSessionId !== sourceSessionId) return
    setGenerating(false)
    if (!suggestion) {
      setNoticeTone('error')
      setNotice('Failed to generate a commit message.')
      return
    }
    if (draftRevisionRef.current === draftRevision) {
      draftRevisionRef.current += 1
      setTitle(suggestion.title)
      setDescription(suggestion.description)
    }
  }

  const commit = async () => {
    if (!activeSessionId || commitDisabled) return
    const sourceSessionId = activeSessionId
    const request = ++latestCommitRequestRef.current
    const draftRevision = draftRevisionRef.current
    setCommitting(true)
    setNotice('')
    const result = await commitVcsChanges(sourceSessionId, selectedVcsType, commitMessage, selectedFiles)
    if (latestCommitRequestRef.current !== request || useAppStore.getState().activeSessionId !== sourceSessionId) return
    setCommitting(false)
    if (!result.ok) {
      setNoticeTone('error')
      setNotice(result.error || 'Failed to commit changes.')
      return
    }
    if (draftRevisionRef.current === draftRevision) {
      draftRevisionRef.current += 1
      setTitle('')
      setDescription('')
    }
    setSelectedFiles([])
    setNoticeTone('success')
    setNotice('Commit created.')
    setCommitOutput(result.message || '')
    setHistoryEpoch((epoch) => epoch + 1)
    await refresh(selectedVcsType, true)
  }

  const capabilities = status.available ? status.capabilities ?? [] : []
  const historyAvailable = systemEnabled && capabilities.includes('history')
  const updateAvailable = systemEnabled && capabilities.includes('update')
  // A scope the current workspace cannot show falls back to the workspace list instead of an empty panel.
  const scope = (reviewScope === 'history' && !historyAvailable) || (reviewScope === 'recorded' && !CODEX_ENABLED) ? 'workspace' : reviewScope
  const revisionPrefix = selectedVcsType === 'svn' ? 'r' : ''

  /** Shows an update's progress and outcome while its chat stays selected; switching away and back picks it up again. */
  const followUpdate = useCallback(async (sourceSessionId: string, vcsType: VcsType, running: { requestId: string; result: Promise<VcsUpdateResult> }) => {
    const request = ++latestUpdateRequestRef.current
    updateRequestIdRef.current = running.requestId
    setUpdating(true)
    const result = await running.result
    if (latestUpdateRequestRef.current !== request || useAppStore.getState().activeSessionId !== sourceSessionId) return
    updateRequestIdRef.current = ''
    setUpdating(false)
    setCommitOutput(result.output)
    if (!result.ok) {
      setNoticeTone('error')
      setNotice(result.error || 'Failed to update the working copy.')
    } else {
      setNoticeTone(result.conflicts ? 'warning' : 'success')
      const reached = result.revision ? ` to ${vcsType === 'svn' ? 'r' : ''}${result.revision}` : ''
      setNotice(result.conflicts ? `Updated${reached} with conflicts. Resolve them before committing.` : `Updated${reached}.`)
    }
    setHistoryEpoch((epoch) => epoch + 1)
    await refresh(vcsType, true)
  }, [refresh])

  useEffect(() => {
    const running = activeSessionId ? updatesInFlight.get(activeSessionId) : undefined
    if (running && activeSessionId) void followUpdate(activeSessionId, selectedVcsType, running)
    // Only on arriving at a chat; followUpdate itself changes with every refresh.
  }, [activeSessionId]) // eslint-disable-line react-hooks/exhaustive-deps

  const update = async () => {
    if (!activeSessionId || updating || committing) return
    const sourceSessionId = activeSessionId
    const requestId = `vcs-update:${sourceSessionId}:${Date.now()}`
    const running = { requestId, result: updateVcsWorkspace(sourceSessionId, selectedVcsType, requestId) }
    updatesInFlight.set(sourceSessionId, running)
    void running.result.finally(() => { if (updatesInFlight.get(sourceSessionId) === running) updatesInFlight.delete(sourceSessionId) })
    setNotice('')
    await followUpdate(sourceSessionId, selectedVcsType, running)
  }

  const shownTone = notice ? noticeTone : status.error ? 'error' : 'warning'

  return (
    <aside aria-label="Commit" className="uam-rv-dialog flex min-h-0 flex-1 flex-col overflow-hidden" style={{ borderRadius: 0, border: 0, borderLeft: '1px solid var(--border)', boxShadow: 'none' }}>
      <div className="uam-rv-dialog__header" style={{ minHeight: 48 }}>
        <div className="min-w-0 flex-1">
          <div className="uam-rv-title">Commit</div>
          <div className="truncate text-xs" title={status.workspaceDirectory} style={{ color: 'var(--text-3)' }}>{status.workspaceDirectory.split(/[\\/]/).filter(Boolean).pop() || 'No workspace selected'}</div>
        </div>
        <IconButton
          icon={<RotateCw size={16} />}
          label="Refresh VCS status"
          disabled={loading || updating || committing}
          onClick={() => { void refresh(selectedVcsType, true) }}
        />
        <IconButton
          icon={<PanelRightClose size={16} aria-hidden />}
          label="Close commit panel"
          tooltip="Hide panel"
          tooltipSide="bottom"
          onClick={() => setCommitPanelOpen(false)}
        />
      </div>

      {/* Recorded chat edits are Codex receipts; builds without Codex keep only the workspace scope. */}
      {(CODEX_ENABLED || historyAvailable) && <div className="shrink-0 px-3" style={{ borderBottom: '1px solid var(--border)' }}>
        <Tabs aria-label="Change review scope" variant="underline" className="uam-rv-scopes" value={scope} onChange={(next) => { setReviewScope(next); setDiffSelection(null) }}>
          <Tab value="workspace" icon={<FolderGit2 size={14} aria-hidden />}>Workspace</Tab>
          {CODEX_ENABLED && <Tab value="recorded" icon={<MessageSquareDiff size={14} aria-hidden />}>Recorded chat edits</Tab>}
          {historyAvailable && <Tab value="history" icon={<History size={14} aria-hidden />}>History</Tab>}
        </Tabs>
      </div>}
      {scope === 'history' && activeSessionId ? (
        <VcsHistory
          key={JSON.stringify([activeSessionId, session?.workspaceDirectory, session?.workspaceWorktreeDirectory, session?.executionHostId, selectedVcsType, historyEpoch])}
          chatId={activeSessionId}
          vcsType={selectedVcsType}
          compareOrigin={capabilities.includes('originComparison')}
        />
      ) : CODEX_ENABLED && scope === 'recorded' ? (
        /* The constant comes first so builds without Codex drop RecordedChatEdits from the bundle. */
        activeSessionId && session ? (
          <RecordedChatEdits
            key={JSON.stringify([activeSessionId, session?.workspaceDirectory, session?.workspaceWorktreeDirectory, session?.executionHostId, gitEnabled, svnEnabled])}
            chatId={activeSessionId}
            workspaceDirectory={session?.workspaceDirectory}
            workspaceWorktreeDirectory={session?.workspaceWorktreeDirectory}
            executionHostId={session?.executionHostId}
          />
        ) : <p className="px-3 py-3 text-xs">Select a chat to review recorded edits.</p>
      ) : <div className="flex min-h-0 flex-1 flex-col overflow-y-auto">
        <div className="shrink-0 px-3 pt-3">
          {(status.warning || status.error || notice) && (
            <div role="status" className="uam-rv-alert mb-3 flex items-start gap-2" style={{ background: `var(--${shownTone}-dim)`, border: '1px solid var(--border)', color: 'var(--text)' }}>
              {shownTone === 'success'
                ? <CheckCircle2 size={14} aria-hidden className="mt-0.5 shrink-0" style={{ color: 'var(--success)' }} />
                : <TriangleAlert size={14} aria-hidden className="mt-0.5 shrink-0" style={{ color: `var(--${shownTone})` }} />}
              <span>{notice || status.error || status.warning}</span>
            </div>
          )}
          {commitOutput && <details className="mb-3 text-xs"><summary className="cursor-pointer" style={{ color: 'var(--text-3)' }}>Command output</summary><pre className="mt-2 max-h-32 overflow-auto whitespace-pre-wrap break-words font-mono text-[11px]" style={{ color: 'var(--text-2)' }}>{commitOutput}</pre></details>}
          <div className="mb-2 flex items-center gap-2 text-xs" style={{ color: 'var(--text-2)' }}>
            {status.vcsTypes.length > 1 ? (
              <MenuSelect
                label="VCS"
                value={selectedVcsType}
                options={status.vcsTypes.map((type) => ({ value: type, label: type.toUpperCase() }))}
                onChange={(type) => { void refresh(type as VcsType) }}
              />
            ) : (
              <span className="rounded px-1.5 py-0.5 font-mono text-[10px] font-semibold" style={{ background: 'var(--surface-up)', color: 'var(--text-2)' }}>{status.available ? status.activeVcsType.toUpperCase() : 'NONE'}</span>
            )}
            <GitBranch size={13} aria-hidden style={{ color: 'var(--text-3)' }} />
            <span className="min-w-0 flex-1 truncate font-mono" style={{ color: 'var(--text)' }}>{status.branchOrRevision || '-'}</span>
            {updateAvailable && <Button size="sm" variant="secondary" loading={updating} disabled={committing} leadingIcon={<ArrowDownToLine size={14} />} onClick={() => { void update() }}>
              {updating ? 'Updating…' : 'Update'}
            </Button>}
            {updateAvailable && updating && <Button size="sm" variant="ghost" onClick={() => cancelVcsRequest(updateRequestIdRef.current)}>Stop</Button>}
          </div>
        </div>

        <div className="mx-3 flex items-center gap-2 py-2 text-xs" style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-2)' }}>
          {/* ui: raw button kept — role=checkbox icon toggle; IconButton adds uam-iconbtn chrome over uam-rv-check-btn */}
          <button
            type="button"
            role="checkbox"
            aria-label="Select all changed files"
            aria-checked={allSelected}
            disabled={status.changedFiles.length === 0}
            onClick={toggleAllFiles}
            className="uam-rv-check-btn"
          >
            {allSelected ? <CheckCircle2 size={16} aria-hidden style={{ color: 'var(--accent)' }} /> : <Circle size={16} aria-hidden />}
          </button>
          <span className="min-w-0 flex-1 font-medium">{loading ? 'Refreshing changes' : `${status.changedFiles.length} changed file${status.changedFiles.length === 1 ? '' : 's'}`}</span>
          {(loading || (!lineStatsReady && status.changedFiles.length > 0)) && <LoaderCircle size={13} role="img" aria-label="Loading VCS status" className="animate-spin motion-reduce:animate-none" />}
          <span style={{ color: 'var(--text-3)' }}>{selectedFiles.length} selected</span>
        </div>
        {/* Sized to its rows (scrolling once full) so the form stays attached directly below; the bottom padding keeps the last row's focus ring inside this clip. */}
        <div className="mx-3 min-h-16 flex-initial overflow-y-auto pb-1">
          {status.changedFiles.length === 0 ? (
            <div className="px-3 py-6 text-center text-xs" style={{ color: 'var(--text-3)' }}>No changed files.</div>
          ) : status.changedFiles.map((file) => {
            const selected = selectedFileSet.has(file.path)
            return (
              <div key={file.path} className="uam-rv-file group" data-selected={selected} style={{ color: selected ? 'var(--text)' : 'var(--text-2)' }}>
                {/* ui: raw button kept — role=checkbox icon toggle; IconButton adds uam-iconbtn chrome over uam-rv-check-btn */}
                <button
                  type="button"
                  role="checkbox"
                  aria-checked={selected}
                  aria-label={`${selected ? 'Deselect' : 'Select'} ${file.path}`}
                  onClick={() => toggleFile(file.path)}
                  className="uam-rv-check-btn"
                >
                  {selected ? <CheckCircle2 size={15} aria-hidden style={{ color: 'var(--accent)' }} /> : <Circle size={15} aria-hidden style={{ color: 'var(--text-3)' }} />}
                </button>
                <RowButton
                  type="button"
                  aria-label={`Review changes to ${file.path}`}
                  onClick={() => { if (activeSessionId) setDiffSelection({ file, chatId: activeSessionId, workspaceDirectory: session?.workspaceDirectory, workspaceWorktreeDirectory: session?.workspaceWorktreeDirectory, executionHostId: session?.executionHostId }) }}
                  className="uam-rv-file__open"
                  style={{ color: 'inherit' }}
                >
                  <span className="uam-rv-file__status" style={{ color: statusColor(file.status) }}>{file.status.trim() || 'M'}</span>
                  <span className="uam-rv-file__path" title={file.path}>
                    <span className="uam-rv-file__name">{file.path.split('/').pop()}</span>
                    {file.path.includes('/') && <span className="uam-rv-file__dir">{file.path.slice(0, file.path.lastIndexOf('/'))}</span>}
                  </span>
                  <span className="uam-rv-file__stat">
                    {!lineStatsReady ? (
                      <span className="font-mono text-[11px]" style={{ color: 'var(--text-3)' }}>...</span>
                    ) : file.binary ? (
                      <span className="font-mono text-[11px]" style={{ color: 'var(--text-3)' }}>BIN</span>
                    ) : (
                      <span className="uam-rv-stat">
                        <ChangeBar additions={file.additions} deletions={file.deletions} max={maxChange} />
                        <span className="uam-rv-stat__add" style={{ color: 'var(--success)' }}>+{file.additions}</span>
                        <span className="uam-rv-stat__del" style={{ color: 'var(--error)' }}>-{file.deletions}</span>
                      </span>
                    )}
                  </span>
                  <ChevronRight size={13} aria-hidden className="uam-rv-file__chevron" style={{ color: 'var(--text-3)' }} />
                </RowButton>
              </div>
            )
          })}
        </div>

        {/* The one heading in the panel: it is what separates the form from the list of files above it. */}
        <SectionHeader className="uam-rv-section" icon={<GitCommitHorizontal size={14} aria-hidden />} title="Commit" description="Publishes only the selected files" />
        <div className="flex flex-1 flex-col p-3">
          <div className="mb-2 flex items-center gap-2">
            <input
              className="uam-rv-field min-w-0 flex-1"
              placeholder="Summary"
              aria-label="Commit summary"
              value={title}
              onChange={(event) => { draftRevisionRef.current += 1; setTitle(event.target.value) }}
            />
            <IconButton
              icon={generating ? <LoaderCircle size={16} className="animate-spin motion-reduce:animate-none" /> : <Sparkles size={16} />}
              label="Generate commit message"
              disabled={generateDisabled}
              onClick={() => { void generateMessage() }}
            />
          </div>
          <textarea
            className="uam-rv-field mb-2 min-h-16 max-h-40 w-full flex-1 resize-none"
            placeholder="Description"
            aria-label="Commit description"
            value={description}
            onChange={(event) => { draftRevisionRef.current += 1; setDescription(event.target.value) }}
          />
          <Button variant="primary" block disabled={commitDisabled} leadingIcon={committing ? <LoaderCircle size={15} className="animate-spin motion-reduce:animate-none" /> : <GitCommitHorizontal size={15} />} onClick={() => { void commit() }}>
            {committing ? 'Committing…' : `Commit ${selectedFiles.length} file${selectedFiles.length === 1 ? '' : 's'}`}
          </Button>
        </div>
      </div>}
      {(() => { const diffOpen = Boolean(scope === 'workspace' && diffSelection && diffSelection.chatId === activeSessionId && diffSelection.workspaceDirectory === session?.workspaceDirectory && diffSelection.workspaceWorktreeDirectory === session?.workspaceWorktreeDirectory && diffSelection.executionHostId === session?.executionHostId); return <Presence open={diffOpen}>{diffOpen && diffSelection && (
        <RepositoryDiffDialog
          chatId={diffSelection.chatId}
          file={diffSelection.file}
          vcsType={selectedVcsType}
          comparisonRef={session?.workspaceIsolationKind === 'gitWorktree' ? session.workspaceBaseRef?.trim() || undefined : undefined}
          getDiff={getVcsFileDiff}
          onClose={() => setDiffSelection(null)}
        />
      )}</Presence> })()}
    </aside>
  )
}
