import { useCallback, useEffect, useMemo, useState } from 'react'
import { useAppStore, type VcsCommitStatus, type VcsType } from '../../store/useAppStore'

function emptyStatus(workspaceDirectory = ''): VcsCommitStatus {
  return {
    available: false,
    vcsTypes: [],
    activeVcsType: 'git',
    workspaceDirectory,
    branchOrRevision: '',
    changedFiles: [],
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

  const refresh = useCallback(async (vcsType = selectedVcsType) => {
    if (!activeSessionId) {
      setStatus(emptyStatus())
      return
    }
    setLoading(true)
    const next = await getVcsCommitStatus(activeSessionId, vcsType)
    setLoading(false)
    const effective = next ?? emptyStatus(session?.workspaceDirectory ?? '')
    setStatus(effective)
    setSelectedVcsType(effective.activeVcsType)
    setSelectedFiles((current) => current.filter((file) => effective.changedFiles.some((changed) => changed.path === file)))
  }, [activeSessionId, getVcsCommitStatus, selectedVcsType, session?.workspaceDirectory])

  useEffect(() => {
    void refresh()
  }, [refresh])

  const commitMessage = [title.trim(), description.trim()].filter(Boolean).join('\n\n')
  const commitDisabled = committing || !status.available || selectedFiles.length === 0 || title.trim().length === 0
  const generateDisabled = generating || !status.available || selectedFiles.length === 0
  const selectedFileSet = useMemo(() => new Set(selectedFiles), [selectedFiles])
  const allSelected = status.changedFiles.length > 0 && status.changedFiles.every((file) => selectedFileSet.has(file.path))

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
    await refresh(selectedVcsType)
  }

  return (
    <aside className="flex h-full flex-col overflow-hidden" style={{ background: 'var(--surface)', borderLeft: '1px solid var(--border)' }}>
      <div className="flex h-12 flex-shrink-0 items-center gap-2 px-3" style={{ borderBottom: '1px solid var(--border)' }}>
        <div className="min-w-0 flex-1">
          <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>Commit</div>
          <div className="truncate text-xs" style={{ color: 'var(--text-3)' }}>{status.workspaceDirectory || 'No workspace selected'}</div>
        </div>
        <button type="button" className="uam-icon-button" title="Refresh VCS status" aria-label="Refresh VCS status" onClick={() => { void refresh() }}>
          <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
            <path d="M13 4v4H9" />
            <path d="M3 12V8h4" />
            <path d="M4.7 5.2A4.5 4.5 0 0 1 12.4 7" />
            <path d="M11.3 10.8A4.5 4.5 0 0 1 3.6 9" />
          </svg>
        </button>
        <button type="button" className="uam-icon-button" title="Close commit panel" aria-label="Close commit panel" onClick={() => setCommitPanelOpen(false)}>
          <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" aria-hidden="true">
            <path d="M4 4l8 8M12 4l-8 8" />
          </svg>
        </button>
      </div>

      <div className="flex-1 overflow-y-auto p-3">
        {(status.warning || status.error || notice) && (
          <div className="mb-3 rounded-md px-3 py-2 text-xs" style={{ background: 'var(--surface-up)', border: '1px solid var(--border)', color: status.error || notice ? 'var(--text)' : 'var(--text-2)' }}>
            {notice || status.error || status.warning}
          </div>
        )}

        <div className="mb-3 grid grid-cols-2 gap-2 text-xs">
          <div>
            <div style={{ color: 'var(--text-3)' }}>VCS</div>
            {status.vcsTypes.length > 1 ? (
              <select
                className="mt-1 w-full rounded-md px-2 py-1"
                style={{ background: 'var(--bg)', border: '1px solid var(--border)', color: 'var(--text)' }}
                value={selectedVcsType}
                onChange={(event) => { void refresh(event.target.value as VcsType) }}
              >
                {status.vcsTypes.map((type) => <option key={type} value={type}>{type.toUpperCase()}</option>)}
              </select>
            ) : (
              <div className="mt-1 font-medium" style={{ color: 'var(--text)' }}>{status.available ? status.activeVcsType.toUpperCase() : 'None'}</div>
            )}
          </div>
          <div>
            <div style={{ color: 'var(--text-3)' }}>Branch/revision</div>
            <div className="mt-1 truncate font-medium" style={{ color: 'var(--text)' }}>{status.branchOrRevision || '-'}</div>
          </div>
        </div>

        <div className="mb-3 overflow-hidden rounded-md" style={{ border: '1px solid var(--border)', background: 'var(--bg)' }}>
          <div className="flex items-center gap-2 px-2 py-2 text-xs" style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-2)' }}>
            <input
              type="checkbox"
              aria-label="Select all changed files"
              checked={allSelected}
              disabled={status.changedFiles.length === 0}
              onChange={toggleAllFiles}
            />
            <span className="min-w-0 flex-1 font-medium">{loading ? 'Refreshing changes' : `${status.changedFiles.length} changed file${status.changedFiles.length === 1 ? '' : 's'}`}</span>
            <span>{selectedFiles.length} selected</span>
          </div>
          <div className="max-h-[360px] overflow-y-auto">
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
                {file.binary ? (
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
          <button type="button" className="uam-secondary-button" disabled={generateDisabled} onClick={() => { void generateMessage() }}>
            <span>{generating ? 'Generating...' : 'AI'}</span>
          </button>
        </div>
        <textarea
          className="mb-2 h-24 w-full resize-none rounded-md p-2 text-sm"
          style={{ background: 'var(--bg)', border: '1px solid var(--border)', color: 'var(--text)' }}
          placeholder="Description"
          value={description}
          onChange={(event) => setDescription(event.target.value)}
        />
        <button type="button" className="uam-primary-button w-full justify-center" disabled={commitDisabled} onClick={() => { void commit() }}>
          <span>{committing ? 'Committing...' : 'Commit selected files'}</span>
        </button>
      </div>
    </aside>
  )
}
