import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { CheckCircle2, ChevronRight, Circle, GitBranch, GitCommitHorizontal, LoaderCircle, RotateCw, Sparkles, X } from 'lucide-react'
import { useAppStore, type VcsChangedFile, type VcsCommitStatus, type VcsType } from '../../store/useAppStore'
import { Button, IconButton, MenuSelect } from '../ui'
import { RepositoryDiffDialog } from '../shared/RepositoryDiffDialog'

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
  return code === 'A' || code === '?' ? 'var(--green)' : code === 'D' ? 'var(--red)' : code === 'R' ? 'var(--blue)' : 'var(--yellow)'
}

export function VcsCommitPanel() {
  const activeSessionId = useAppStore((s) => s.activeSessionId)
  const session = useAppStore((s) => s.sessions.find((candidate) => candidate.id === s.activeSessionId) ?? null)
  const getVcsCommitStatus = useAppStore((s) => s.getVcsCommitStatus)
  const commitVcsChanges = useAppStore((s) => s.commitVcsChanges)
  const generateVcsCommitMessage = useAppStore((s) => s.generateVcsCommitMessage)
  const setCommitPanelOpen = useAppStore((s) => s.setCommitPanelOpen)
  const getVcsFileDiff = useAppStore((s) => s.getVcsFileDiff)
  const [diffFile, setDiffFile] = useState<VcsChangedFile | null>(null)
  const [status, setStatus] = useState<VcsCommitStatus>(() => emptyStatus(session?.workspaceDirectory ?? ''))
  const [selectedVcsType, setSelectedVcsType] = useState<VcsType>('git')
  const [selectedFiles, setSelectedFiles] = useState<string[]>([])
  const [title, setTitle] = useState('')
  const [description, setDescription] = useState('')
  const [loading, setLoading] = useState(false)
  const [committing, setCommitting] = useState(false)
  const [generating, setGenerating] = useState(false)
  const [notice, setNotice] = useState('')
  const [commitOutput, setCommitOutput] = useState('')
  const latestStatusRequestRef = useRef('')
  const latestGenerateRequestRef = useRef(0)
  const latestCommitRequestRef = useRef(0)

  useEffect(() => {
    latestStatusRequestRef.current = ''
    latestGenerateRequestRef.current += 1
    latestCommitRequestRef.current += 1
    setStatus(emptyStatus(session?.workspaceDirectory ?? ''))
    setSelectedFiles([])
    setTitle('')
    setDescription('')
    setNotice('')
    setCommitOutput('')
    setLoading(false)
    setGenerating(false)
    setCommitting(false)
  }, [activeSessionId, session?.workspaceDirectory])

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
  }, [activeSessionId, getVcsCommitStatus, selectedVcsType, session?.workspaceDirectory])

  useEffect(() => {
    void refresh()
  }, [refresh])

  const commitMessage = [title.trim(), description.trim()].filter(Boolean).join('\n\n')
  const commitDisabled = committing || !status.available || selectedFiles.length === 0 || title.trim().length === 0
  const generateDisabled = generating || !status.available || selectedFiles.length === 0
  const selectedFileSet = useMemo(() => new Set(selectedFiles), [selectedFiles])
  const allSelected = status.changedFiles.length > 0 && status.changedFiles.every((file) => selectedFileSet.has(file.path))
  const lineStatsReady = status.lineStatsReady !== false

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
    setGenerating(true)
    setNotice('')
    const suggestion = await generateVcsCommitMessage(sourceSessionId, selectedVcsType, selectedFiles)
    if (latestGenerateRequestRef.current !== request || useAppStore.getState().activeSessionId !== sourceSessionId) return
    setGenerating(false)
    if (!suggestion) {
      setNotice('Failed to generate a commit message.')
      return
    }
    setTitle(suggestion.title)
    setDescription(suggestion.description)
  }

  const commit = async () => {
    if (!activeSessionId || commitDisabled) return
    const sourceSessionId = activeSessionId
    const request = ++latestCommitRequestRef.current
    setCommitting(true)
    setNotice('')
    const result = await commitVcsChanges(sourceSessionId, selectedVcsType, commitMessage, selectedFiles)
    if (latestCommitRequestRef.current !== request || useAppStore.getState().activeSessionId !== sourceSessionId) return
    setCommitting(false)
    if (!result.ok) {
      setNotice(result.error || 'Failed to commit changes.')
      return
    }
    setTitle('')
    setDescription('')
    setSelectedFiles([])
    setNotice('Commit created.')
    setCommitOutput(result.message || '')
    await refresh(selectedVcsType, true)
  }

  return (
    <aside aria-label="Commit" className="flex min-h-0 flex-1 flex-col overflow-hidden" style={{ background: 'var(--surface)', borderLeft: '1px solid var(--border)' }}>
      <div className="flex h-12 flex-shrink-0 items-center gap-2 px-3" style={{ borderBottom: '1px solid var(--border)' }}>
        <div className="min-w-0 flex-1">
          <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>Commit</div>
          <div className="truncate text-xs" title={status.workspaceDirectory} style={{ color: 'var(--text-3)' }}>{status.workspaceDirectory.split(/[\\/]/).filter(Boolean).pop() || 'No workspace selected'}</div>
        </div>
        <IconButton
          icon={<RotateCw size={16} />}
          label="Refresh VCS status"
          disabled={loading}
          onClick={() => { void refresh(selectedVcsType, true) }}
        />
        <IconButton
          icon={<X size={16} />}
          label="Close commit panel"
          onClick={() => setCommitPanelOpen(false)}
        />
      </div>

      <div className="flex min-h-0 flex-1 flex-col overflow-hidden">
        <div className="shrink-0 px-3 pt-3">
          {(status.warning || status.error || notice) && (
            <div role="status" className="mb-3 rounded-md px-3 py-2 text-xs" style={{ background: notice === 'Commit created.' ? 'var(--success-dim)' : status.error || notice ? 'var(--error-dim)' : 'var(--warning-dim)', border: '1px solid var(--border)', color: 'var(--text)' }}>
              {notice || status.error || status.warning}
            </div>
          )}
          {commitOutput && <details className="mb-3 text-xs"><summary className="cursor-pointer">Command output</summary><pre className="mt-2 max-h-32 overflow-auto whitespace-pre-wrap break-words">{commitOutput}</pre></details>}
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
            <GitBranch size={13} aria-hidden style={{ color: 'var(--accent)' }} />
            <span className="min-w-0 truncate font-medium" style={{ color: 'var(--text)' }}>{status.branchOrRevision || '-'}</span>
          </div>
        </div>

        <div className="mx-3 flex items-center gap-2 py-2 text-xs" style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-2)' }}>
          <button
            type="button"
            role="checkbox"
            aria-label="Select all changed files"
            aria-checked={allSelected}
            disabled={status.changedFiles.length === 0}
            onClick={toggleAllFiles}
            className="uam-icon-button"
          >
            {allSelected ? <CheckCircle2 size={16} aria-hidden style={{ color: 'var(--accent)' }} /> : <Circle size={16} aria-hidden />}
          </button>
          <span className="min-w-0 flex-1 font-medium">{loading ? 'Refreshing changes' : `${status.changedFiles.length} changed file${status.changedFiles.length === 1 ? '' : 's'}`}</span>
          {(loading || (!lineStatsReady && status.changedFiles.length > 0)) && <LoaderCircle size={13} aria-label="Loading VCS status" />}
          <span>{selectedFiles.length} selected</span>
        </div>
        <div className="mx-3 min-h-0 flex-1 overflow-y-auto">
          {status.changedFiles.length === 0 ? (
            <div className="px-3 py-6 text-center text-xs" style={{ color: 'var(--text-3)' }}>No changed files.</div>
          ) : status.changedFiles.map((file) => {
            const selected = selectedFileSet.has(file.path)
            return (
              <div
                key={file.path}
                className="group flex w-full items-center gap-1 text-xs"
                style={{ color: selected ? 'var(--text)' : 'var(--text-2)', borderBottom: '1px solid var(--border)' }}
              >
                <button
                  type="button"
                  role="checkbox"
                  aria-checked={selected}
                  aria-label={`${selected ? 'Deselect' : 'Select'} ${file.path}`}
                  onClick={() => toggleFile(file.path)}
                  className="uam-icon-button shrink-0"
                >
                  {selected ? <CheckCircle2 size={15} aria-hidden style={{ color: 'var(--accent)' }} /> : <Circle size={15} aria-hidden style={{ color: 'var(--text-3)' }} />}
                </button>
                <button
                  type="button"
                  aria-label={`Review changes to ${file.path}`}
                  onClick={() => setDiffFile(file)}
                  className="uam-choice-button flex min-w-0 flex-1 items-center gap-2 rounded px-1 py-2 text-left"
                  style={{ background: 'transparent', color: 'inherit' }}
                >
                  <span className="w-6 flex-shrink-0 text-center font-mono text-[11px] font-semibold" style={{ color: statusColor(file.status) }}>{file.status.trim() || 'M'}</span>
                  <span className="min-w-0 flex-1 truncate" title={file.path}>{file.path.split('/').pop()}<span className="ml-1.5" style={{ color: 'var(--text-3)' }}>{file.path.includes('/') ? file.path.slice(0, file.path.lastIndexOf('/')) : ''}</span></span>
                  {!lineStatsReady ? (
                    <span className="flex-shrink-0 font-mono text-[11px]" style={{ color: 'var(--text-3)' }}>...</span>
                  ) : file.binary ? (
                    <span className="flex-shrink-0 font-mono text-[11px]" style={{ color: 'var(--text-3)' }}>BIN</span>
                  ) : (
                    <span className="flex flex-shrink-0 items-center gap-2 font-mono text-[11px]">
                      <span style={{ color: 'var(--green)' }}>+{file.additions}</span>
                      <span style={{ color: 'var(--red)' }}>-{file.deletions}</span>
                    </span>
                  )}
                  <ChevronRight size={13} aria-hidden className="shrink-0 opacity-0 transition-opacity group-hover:opacity-100" style={{ color: 'var(--text-3)' }} />
                </button>
              </div>
            )
          })}
        </div>

        <div className="shrink-0 p-3" style={{ borderTop: '1px solid var(--border)' }}>
          <div className="mb-2 flex items-center gap-2">
            <input
              className="min-w-0 flex-1 rounded-md px-2 py-2 text-sm"
              style={{ background: 'var(--bg)', border: '1px solid var(--border)', color: 'var(--text)' }}
              placeholder="Summary"
              aria-label="Commit summary"
              value={title}
              onChange={(event) => setTitle(event.target.value)}
            />
            <IconButton
              icon={generating ? <LoaderCircle size={16} /> : <Sparkles size={16} />}
              label="Generate commit message"
              disabled={generateDisabled}
              onClick={() => { void generateMessage() }}
            />
          </div>
          <textarea
            className="mb-2 h-16 w-full resize-none rounded-md p-2 text-sm"
            style={{ background: 'var(--bg)', border: '1px solid var(--border)', color: 'var(--text)' }}
            placeholder="Description"
            aria-label="Commit description"
            value={description}
            onChange={(event) => setDescription(event.target.value)}
          />
          <Button variant="primary" block disabled={commitDisabled} leadingIcon={committing ? <LoaderCircle size={15} /> : <GitCommitHorizontal size={15} />} onClick={() => { void commit() }}>
            {committing ? 'Committing…' : `Commit ${selectedFiles.length} file${selectedFiles.length === 1 ? '' : 's'}`}
          </Button>
        </div>
      </div>
      {diffFile && activeSessionId && (
        <RepositoryDiffDialog
          chatId={activeSessionId}
          file={diffFile}
          vcsType={selectedVcsType}
          comparisonRef={session?.workspaceIsolationKind === 'gitWorktree' ? session.workspaceBaseRef?.trim() || undefined : undefined}
          getDiff={getVcsFileDiff}
          onClose={() => setDiffFile(null)}
        />
      )}
    </aside>
  )
}
