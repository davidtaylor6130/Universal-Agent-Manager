import { useEffect, useId, useState } from 'react'
import { LoaderCircle } from 'lucide-react'
import { sendToCEF } from '../../ipc/cefBridge'
import { sanitizeGitBaseBranch } from '../../store/cpp/sanitizers'
import { Button, MenuSelect } from '../ui'

interface BranchContext {
  vcs: string
  currentBranch: string
  detached: boolean
  localBranches: Array<{ name: string; commit: string }>
}

function branchContext(value: unknown): BranchContext | undefined {
  if (!value || typeof value !== 'object') return undefined
  const context = value as Record<string, unknown>
  if (context.vcs === '' || context.vcs === 'svn') return { vcs: context.vcs, currentBranch: '', detached: false, localBranches: [] }
  if (context.vcs !== 'git' || typeof context.detached !== 'boolean' || typeof context.currentBranch !== 'string') return undefined
  if (context.detached) return { vcs: 'git', currentBranch: '', detached: true, localBranches: [] }
  const isCommit = (commit: unknown): commit is string => typeof commit === 'string' && /^(?:[0-9a-f]{40}|[0-9a-f]{64})$/.test(commit)
  if (!context.currentBranch || !isCommit(context.currentCommit) || !Array.isArray(context.localBranches) || context.localBranches.length > 4096) return undefined
  const branches: BranchContext['localBranches'] = []
  const seen = new Set<string>()
  for (const entry of context.localBranches) {
    if (!entry || typeof entry !== 'object') return undefined
    const { name, commit } = entry as Record<string, unknown>
    if (typeof name !== 'string' || !name || sanitizeGitBaseBranch(name) !== name || !isCommit(commit) || seen.has(name)) return undefined
    seen.add(name)
    branches.push({ name, commit })
  }
  if (!branches.some((branch) => branch.name === context.currentBranch && branch.commit === context.currentCommit)) return undefined
  return { vcs: 'git', currentBranch: context.currentBranch, detached: false, localBranches: branches }
}

/** Read-only choices; the native launch/send path owns validation and isolation. Mount per workspace. */
export function NewChatBranchPicker({ folderId, defaultBranch, value, onChange, unavailableReason = '', disabled = false }: {
  folderId: string
  defaultBranch: string
  value: string | undefined
  onChange: (branch: string | undefined) => void
  unavailableReason?: string
  disabled?: boolean
}) {
  const [context, setContext] = useState<BranchContext>()
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState('')
  const [attempt, setAttempt] = useState(0)
  const id = useId()

  useEffect(() => {
    if (unavailableReason) return
    let cancelled = false
    setLoading(true)
    setError('')
    void sendToCEF({ action: 'getWorkspaceVcsContext', payload: { folderId } }).then((response) => {
      if (cancelled) return
      const next = response.ok ? branchContext(response.data) : undefined
      setContext(next)
      setError(next ? '' : response.error || 'The workspace branch list could not be read. Try again.')
      setLoading(false)
    }).catch(() => {
      if (cancelled) return
      setError('The workspace branch list could not be read. Try again.')
      setLoading(false)
    })
    return () => { cancelled = true }
  }, [folderId, unavailableReason, attempt])

  const ready = !unavailableReason && !loading && !error && context?.vcs === 'git' && !context.detached
  const attachedContext = context?.vcs === 'git' && !context.detached ? context : undefined
  const selectedValue = value === undefined ? 'default' : value === '' ? 'head' : `branch:${value}`
  // Keep the chosen policy visible while a read is pending, including the render
  // before a completed invalid result clears the parent's override.
  const options = [
    { value: 'default', label: `Use default (${defaultBranch || 'HEAD'})` },
    ...(attachedContext ? [
      { value: 'head', label: `Current branch (${attachedContext.currentBranch})`, description: 'Use HEAD instead of the default base branch.' },
      ...attachedContext.localBranches.map((branch) => ({ value: `branch:${branch.name}`, label: branch.name })),
    ] : []),
  ]
  if (value !== undefined && !options.some((option) => option.value === selectedValue)) {
    options.push({ value: selectedValue, label: value === '' ? 'Current branch (HEAD)' : value })
  }
  useEffect(() => {
    if (loading || unavailableReason || value === undefined || (!context && !error)) return
    if (!ready || (value !== '' && !attachedContext?.localBranches.some((branch) => branch.name === value))) onChange(undefined)
  }, [loading, unavailableReason, value, context, error, ready, attachedContext, onChange])
  const message = unavailableReason || (loading ? 'Loading branches…' : context?.vcs === 'svn'
    ? 'SVN workspaces do not support Git branch selection.'
    : context?.vcs === '' ? 'This workspace has no enabled Git repository.'
    : context?.detached ? 'Branch selection requires an attached Git branch.'
    : error ? '' : "The chat's isolated worktree starts from the chosen branch.")

  return (
    <div className="space-y-1.5" aria-busy={loading || undefined}>
      <label htmlFor={`${id}-select`} className="block text-xs font-medium" style={{ color: 'var(--text-2)' }}>Starting branch</label>
      <MenuSelect
        id={`${id}-select`}
        describedBy={`${id}-message`}
        label="Starting branch"
        matchTriggerWidth
        disabled={disabled || !ready}
        value={selectedValue}
        options={options}
        onChange={(next) => onChange(next === 'default' ? undefined : next === 'head' ? '' : next.slice('branch:'.length))}
      />
      <div id={`${id}-message`} className="text-xs">
        <p role="status" className="flex items-center gap-1.5" style={{ color: 'var(--text-3)' }}>
          {loading && <LoaderCircle size={12} aria-hidden className="animate-spin motion-reduce:animate-none" />}
          {message}
        </p>
        {error && !loading && <p role="alert" style={{ color: 'var(--error)' }}>{error}</p>}
      </div>
      {!unavailableReason && (
        <Button
          variant="secondary"
          size="sm"
          disabled={disabled}
          aria-disabled={loading || undefined}
          aria-busy={loading || undefined}
          onClick={() => { if (!disabled && !loading) setAttempt((previous) => previous + 1) }}
        >
          {loading && attempt > 0 ? 'Refreshing branches…' : error ? 'Retry branches' : 'Refresh branches'}
        </Button>
      )}
    </div>
  )
}
