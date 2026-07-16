import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { RotateCw, X } from 'lucide-react'
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

  useEffect(() => {
    latestStatusRequestRef.current = ''
    setStatus(emptyStatus(session?.workspaceDirectory ?? ''))
    setSelectedFiles([])
    setTitle('')
    setDescription('')
    setNotice('')
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
    setGenerating(true)
    setNotice('')
    const suggestion = await generateVcsCommitMessage(activeSessionId, selectedVcsType, selectedFiles)
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
    setCommitting(true)
    setNotice('')
    const result = await commitVcsChanges(activeSessionId, selectedVcsType, commitMessage, selectedFiles)
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

  return (
    <aside className="flex h-full flex-col overflow-hidden" style={{ background: 'var(--surface)', borderLeft: '1px solid var(--border)' }}>
      <div className="flex h-12 flex-shrink-0 items-center gap-2 px-3" style={{ borderBottom: '1px solid var(--border)' }}>
        <div className="min-w-0 flex-1">
          <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>Commit</div>
          <div className="truncate text-xs" style={{ color: 'var(--text-3)' }}>{status.workspaceDirectory || 'No workspace selected'}</div>
        </div>
        <IconButton
          icon={<RotateCw size={16} />}
          label="Refresh VCS status"
          onClick={() => { void refresh(selectedVcsType, true) }}
        />
        <IconButton
          icon={<X size={16} />}
          label="Close commit panel"
          onClick={() => setCommitPanelOpen(false)}
        />
      </div>

      <div className="flex min-h-0 flex-1 flex-col overflow-hidden p-3">
        {(status.warning || status.error || notice) && (
          <div className="mb-3 rounded-md px-3 py-2 text-xs" style={{ background: 'var(--surface-up)', border: '1px solid var(--border)', color: status.error || notice ? 'var(--text)' : 'var(--text-2)' }}>
            {notice || status.error || status.warning}
          </div>
        )}

        <div className="mb-3 grid grid-cols-2 gap-2 text-xs">
          <div>
            <div style={{ color: 'var(--text-3)' }}>VCS</div>
            {status.vcsTypes.length > 1 ? (
              <div className="mt-1">
                <MenuSelect
                  label="VCS"
                  value={selectedVcsType}
                  options={status.vcsTypes.map((type) => ({ value: type, label: type.toUpperCase() }))}
                  onChange={(type) => { void refresh(type as VcsType) }}
                />
              </div>
            ) : (
              <div className="mt-1 font-medium" style={{ color: 'var(--text)' }}>{status.available ? status.activeVcsType.toUpperCase() : 'None'}</div>
            )}
          </div>
          <div>
            <div style={{ color: 'var(--text-3)' }}>Branch/revision</div>
            <div className="mt-1 truncate font-medium" style={{ color: 'var(--text)' }}>{status.branchOrRevision || '-'}</div>
          </div>
        </div>

        <div className="mb-3 flex min-h-0 w-full flex-1 flex-col overflow-hidden rounded-md" style={{ border: '1px solid var(--border)', background: 'var(--bg)' }}>
          <div className="flex items-center gap-2 px-2 py-2 text-xs" style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-2)' }}>
            <input
              type="checkbox"
              aria-label="Select all changed files"
              checked={allSelected}
              disabled={status.changedFiles.length === 0}
              onChange={toggleAllFiles}
            />
            <span className="min-w-0 flex-1 font-medium">{loading ? 'Refreshing changes' : `${status.changedFiles.length} changed file${status.changedFiles.length === 1 ? '' : 's'}`}</span>
            {!lineStatsReady && status.changedFiles.length > 0 && <span>Stats loading</span>}
            <span>{selectedFiles.length} selected</span>
          </div>
          <div className="min-h-0 flex-1 overflow-y-auto">
            {status.changedFiles.length === 0 ? (
              <div className="px-3 py-4 text-xs" style={{ color: 'var(--text-3)' }}>No changed files.</div>
            ) : status.changedFiles.map((file) => (
              <label
                key={file.path}
                className="flex w-full cursor-pointer items-center gap-2 px-2 py-2 text-left text-xs"
                style={{ color: selectedFileSet.has(file.path) ? 'var(--text)' : 'var(--text-2)', background: selectedFileSet.has(file.path) ? 'var(--surface-up)' : 'transparent', borderBottom: '1px solid var(--border)' }}
              >
                <input type="checkbox" checked={selectedFileSet.has(file.path)} onChange={() => toggleFile(file.path)} />
                <span className="w-8 flex-shrink-0 text-center font-mono text-[11px]" style={{ color: 'var(--text-3)' }}>{file.status.trim() || 'M'}</span>
                <span className="min-w-0 flex-1 truncate">{file.path}</span>
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
              </label>
            ))}
          </div>
        </div>

        <div className="mb-2 flex items-center gap-2">
          <input
            className="min-w-0 flex-1 rounded-md px-2 py-2 text-sm"
            style={{ background: 'var(--bg)', border: '1px solid var(--border)', color: 'var(--text)' }}
            placeholder="Summary"
            value={title}
            onChange={(event) => setTitle(event.target.value)}
          />
          <Button variant="secondary" size="md" disabled={generateDisabled} onClick={() => { void generateMessage() }}>
            {generating ? 'Generating...' : 'AI'}
          </Button>
        </div>
        <textarea
          className="mb-2 h-24 w-full resize-none rounded-md p-2 text-sm"
          style={{ background: 'var(--bg)', border: '1px solid var(--border)', color: 'var(--text)' }}
          placeholder="Description"
          value={description}
          onChange={(event) => setDescription(event.target.value)}
        />
        <Button variant="primary" block disabled={commitDisabled} onClick={() => { void commit() }}>
          {committing ? 'Committing...' : 'Commit selected files'}
        </Button>
      </div>
    </aside>
  )
}
