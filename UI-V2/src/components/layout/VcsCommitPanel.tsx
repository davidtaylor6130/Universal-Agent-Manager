import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { Check, CheckCircle2, GitBranch, GitCommitHorizontal, LoaderCircle, Minus, RotateCw, Sparkles, X } from 'lucide-react'
import { useAppStore, type VcsCommitStatus, type VcsType } from '../../store/useAppStore'
import { Button, IconButton, MenuSelect } from '../ui'

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

export function VcsCommitPanel() {
  const activeSessionId = useAppStore((s) => s.activeSessionId)
  const session = useAppStore((s) => s.sessions.find((candidate) => candidate.id === s.activeSessionId) ?? null)
  const getVcsCommitStatus = useAppStore((s) => s.getVcsCommitStatus)
  const commitVcsChanges = useAppStore((s) => s.commitVcsChanges)
  const generateVcsCommitMessage = useAppStore((s) => s.generateVcsCommitMessage)
  const setCommitPanelOpen = useAppStore((s) => s.setCommitPanelOpen)
  const [status, setStatus] = useState<VcsCommitStatus>(() => emptyStatus(session?.workspaceDirectory ?? ''))
  const [selectedVcsType, setSelectedVcsType] = useState<VcsType>('git')
  const [selectedFiles, setSelectedFiles] = useState<string[]>([])
  const [title, setTitle] = useState('')
  const [description, setDescription] = useState('')
  const [loading, setLoading] = useState(false)
  const [committing, setCommitting] = useState(false)
  const [generating, setGenerating] = useState(false)
  const [notice, setNotice] = useState('')
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
    setNotice(result.message || 'Commit created.')
    await refresh(selectedVcsType, true)
  }

  const noticeIsError = Boolean(status.error) || /fail|error|could not|cannot/i.test(notice)
  const selectedCount = selectedFiles.length

  return (
    <aside className="uam-commit-panel flex h-full flex-col overflow-hidden">
      <div className="uam-commit-panel__header">
        <div className="min-w-0 flex-1">
          <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>Commit</div>
          <div className="mt-1 flex min-w-0 items-center gap-1.5" title={status.workspaceDirectory || undefined}>
            {status.vcsTypes.length > 1 ? (
              <MenuSelect
                label="VCS"
                value={selectedVcsType}
                options={status.vcsTypes.map((type) => ({ value: type, label: type.toUpperCase() }))}
                onChange={(type) => { void refresh(type as VcsType) }}
              />
            ) : (
              <span className="uam-commit-chip">{status.available ? status.activeVcsType.toUpperCase() : 'No VCS'}</span>
            )}
            {status.branchOrRevision && (
              <span className="uam-commit-chip uam-commit-chip--branch min-w-0">
                <GitBranch size={11} aria-hidden />
                <span className="truncate">{status.branchOrRevision}</span>
              </span>
            )}
          </div>
        </div>
        <IconButton
          icon={<RotateCw size={15} className={loading ? 'uam-spin' : undefined} />}
          label="Refresh VCS status"
          size="sm"
          disabled={loading}
          onClick={() => { void refresh(selectedVcsType, true) }}
        />
        <IconButton
          icon={<X size={15} />}
          label="Close commit panel"
          size="sm"
          onClick={() => setCommitPanelOpen(false)}
        />
      </div>

      <div className="flex min-h-0 flex-1 flex-col overflow-hidden">
        {(status.warning || status.error || notice) && (
          <div role={noticeIsError ? 'alert' : 'status'} className="uam-commit-notice uam-reveal" data-tone={noticeIsError ? 'error' : 'info'}>
            {notice || status.error || status.warning}
          </div>
        )}

        <div className="uam-commit-list-header">
          <button
            type="button"
            role="checkbox"
            aria-label="Select all changed files"
            aria-checked={allSelected ? true : selectedCount > 0 ? 'mixed' : false}
            disabled={status.changedFiles.length === 0}
            onClick={toggleAllFiles}
            className="uam-commit-check"
          >
            {allSelected ? <Check size={11} strokeWidth={3} aria-hidden /> : selectedCount > 0 ? <Minus size={11} strokeWidth={3} aria-hidden /> : null}
          </button>
          <span className="min-w-0 flex-1 font-medium">{loading ? 'Refreshing changes' : `${status.changedFiles.length} changed file${status.changedFiles.length === 1 ? '' : 's'}`}</span>
          {(loading || (!lineStatsReady && status.changedFiles.length > 0)) && <LoaderCircle size={13} className="uam-spin" aria-label="Loading VCS status" />}
          <span style={{ color: 'var(--text-3)' }}>{selectedCount} selected</span>
        </div>
        <div className="min-h-0 flex-1 overflow-y-auto">
          {status.changedFiles.length === 0 ? (
            <div className="uam-commit-empty">
              <CheckCircle2 size={22} aria-hidden />
              <span>{status.available ? 'Working tree clean' : 'No changes to show'}</span>
            </div>
          ) : status.changedFiles.map((file) => {
            const selected = selectedFileSet.has(file.path)
            const code = file.status.trim() || 'M'
            const slash = file.path.replace(/\\/g, '/').lastIndexOf('/')
            const name = slash >= 0 ? file.path.slice(slash + 1) : file.path
            const dir = slash >= 0 ? file.path.slice(0, slash) : ''
            return (
              <button
                type="button"
                role="checkbox"
                aria-checked={selected}
                aria-label={`${selected ? 'Deselect' : 'Select'} ${file.path}`}
                key={file.path}
                onClick={() => toggleFile(file.path)}
                className="uam-commit-file"
                data-selected={selected}
                title={file.path}
              >
                <span className="uam-commit-check" aria-hidden>{selected && <Check size={11} strokeWidth={3} />}</span>
                <span className="uam-commit-status" data-code={code[0]}>{code}</span>
                <span className="min-w-0 flex-1 truncate">
                  <span className="uam-commit-file__name">{name}</span>
                  {dir && <span className="uam-commit-file__dir">{dir}</span>}
                </span>
                {!lineStatsReady ? (
                  <span className="uam-commit-file__stat" style={{ color: 'var(--text-3)' }}>…</span>
                ) : file.binary ? (
                  <span className="uam-commit-file__stat" style={{ color: 'var(--text-3)' }}>BIN</span>
                ) : (
                  <span className="uam-commit-file__stat">
                    {file.additions > 0 && <span style={{ color: 'var(--green)' }}>+{file.additions}</span>}
                    {file.deletions > 0 && <span style={{ color: 'var(--red)' }}>-{file.deletions}</span>}
                  </span>
                )}
              </button>
            )
          })}
        </div>

        <div className="uam-commit-composer">
          <div className="uam-commit-message">
            <div className="flex items-center gap-1">
              <input
                className="min-w-0 flex-1 bg-transparent px-2.5 py-2 text-sm outline-none"
                style={{ color: 'var(--text)' }}
                placeholder="Summary (required)"
                aria-label="Commit summary"
                value={title}
                onChange={(event) => setTitle(event.target.value)}
              />
              {title.length > 50 && (
                <span className="text-[11px] tabular-nums" style={{ color: title.length > 72 ? 'var(--warning)' : 'var(--text-3)' }} title="Keep summaries under about 72 characters">{title.length}</span>
              )}
              <IconButton
                icon={generating ? <LoaderCircle size={15} className="uam-spin" /> : <Sparkles size={15} />}
                label="Generate commit message"
                tooltip={selectedCount === 0 ? 'Select files to generate a message' : 'Generate commit message'}
                size="sm"
                disabled={generateDisabled}
                onClick={() => { void generateMessage() }}
              />
            </div>
            <textarea
              className="block h-20 w-full resize-none bg-transparent px-2.5 py-2 text-sm outline-none"
              style={{ color: 'var(--text)', borderTop: '1px solid var(--border)' }}
              placeholder="Description (optional)"
              aria-label="Commit description"
              value={description}
              onChange={(event) => setDescription(event.target.value)}
            />
          </div>
          <Button
            variant="primary"
            block
            disabled={commitDisabled}
            leadingIcon={committing ? <LoaderCircle size={15} className="uam-spin" /> : <GitCommitHorizontal size={15} />}
            onClick={() => { void commit() }}
          >
            {committing ? 'Committing…' : selectedCount > 0 ? `Commit ${selectedCount} file${selectedCount === 1 ? '' : 's'}` : 'Commit selected files'}
          </Button>
          {!committing && commitDisabled && status.available && (
            <div className="text-center text-[11px]" style={{ color: 'var(--text-3)' }}>
              {selectedCount === 0 ? 'Select files to commit' : 'Add a summary to commit'}
            </div>
          )}
        </div>
      </div>
    </aside>
  )
}
