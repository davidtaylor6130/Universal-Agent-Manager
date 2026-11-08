import { useEffect, useId, useRef, useState } from 'react'
import { LoaderCircle } from 'lucide-react'
import { sendToCEF } from '../../ipc/cefBridge'
import { sanitizeGitBaseBranch } from '../../store/cpp/sanitizers'
import { useAppStore } from '../../store/useAppStore'
import { chatWorktreeBranches, preferredStartingBranch, rankBranches } from '../../utils/branchRecency'
import { Button, MenuSelect } from '../ui'

interface BranchContext {
  vcs: string
  currentBranch: string
  detached: boolean
  localBranches: Array<{ name: string; commit: string }>
  preferred?: string
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
  const [settled, setSettled] = useState(false)
  const [attempt, setAttempt] = useState(0)
  // Refresh appears only after a real list was shown, then stays mounted to keep keyboard focus.
  const [listed, setListed] = useState(false)
  // The branch this dialog preselected; the reason shows only while that automatic choice stands.
  const [autoPicked, setAutoPicked] = useState<string>()
  const id = useId()
  const busyRetried = useRef(false)

  useEffect(() => {
    if (unavailableReason) return
    let cancelled = false
    let busyRetry: ReturnType<typeof setTimeout> | undefined
    setLoading(true)
    setSettled(false)
    void sendToCEF({ action: 'getWorkspaceVcsContext', payload: { folderId } }).then((response) => {
      if (cancelled) return
      if (response.code === 429 && !busyRetried.current) {
        // Native shares four context-read slots; one silent re-read covers a momentary busy reply.
        busyRetried.current = true
        busyRetry = setTimeout(() => setAttempt((previous) => previous + 1), 1000)
        return
      }
      busyRetried.current = false
      const next = response.ok ? branchContext(response.data) : undefined
      if (next?.vcs === 'git' && !next.detached) {
        // Native lists newest commit first; other chats' worktree branches trail and are never preferred.
        const worktrees = chatWorktreeBranches(useAppStore.getState().sessions, folderId)
        next.localBranches = rankBranches(next.localBranches, worktrees)
        next.preferred = preferredStartingBranch(next.localBranches, worktrees, defaultBranch, next.currentBranch)
      }
      // Branch choice is optional: an unreadable result keeps any list already shown and otherwise stays quiet.
      setContext((previous) => next ?? previous)
      if (next?.vcs === 'git' && !next.detached) setListed(true)
      setSettled(true)
      setLoading(false)
    }).catch(() => {
      if (cancelled) return
      setSettled(true)
      setLoading(false)
    })
    return () => { cancelled = true; clearTimeout(busyRetry) }
  }, [folderId, unavailableReason, attempt, defaultBranch])

  const ready = !unavailableReason && !loading && context?.vcs === 'git' && !context.detached
  const attachedContext = context?.vcs === 'git' && !context.detached ? context : undefined
  const selectedValue = value === undefined ? 'default' : value === '' ? 'head' : `branch:${value}`
  // Keep the chosen policy visible while a read is pending, including the render
  // before a completed invalid result clears the parent's override. Entries that repeat
  // the default (HEAD with no configured default, or the default branch itself) are omitted.
  const options = [
    { value: 'default', label: defaultBranch ? `Use default (${defaultBranch})` : 'Use default branch' },
    ...(attachedContext ? [
      ...(defaultBranch && attachedContext.currentBranch !== defaultBranch
        ? [{ value: 'head', label: `Current branch (${attachedContext.currentBranch})`, description: 'Start from the checked-out branch.' }]
        : []),
      ...attachedContext.localBranches.filter((branch) => branch.name !== defaultBranch).map((branch) => ({ value: `branch:${branch.name}`, label: branch.name })),
    ] : []),
  ]
  if (value !== undefined && !options.some((option) => option.value === selectedValue)) {
    options.push({ value: selectedValue, label: value === '' ? (context?.detached ? 'Current branch (detached)' : 'Current branch') : value })
  }
  // Preselect the preferred branch once, on the first completed read; refreshes keep the user's choice.
  const preselected = useRef(false)
  useEffect(() => {
    if (preselected.current) return
    // Once creation has started, a later read must not change the choice or clear a creation error.
    if (disabled) { preselected.current = true; return }
    if (!ready || !attachedContext) return
    preselected.current = true
    const preferred = attachedContext.preferred
    if (value === undefined && preferred && preferred !== (defaultBranch || attachedContext.currentBranch)) {
      setAutoPicked(preferred)
      onChange(preferred)
    }
  }, [disabled, ready, attachedContext, value, defaultBranch, onChange])
  useEffect(() => {
    if (loading || unavailableReason || value === undefined || (!context && !settled)) return
    if (!ready || (value !== '' && !attachedContext?.localBranches.some((branch) => branch.name === value))) onChange(undefined)
  }, [loading, unavailableReason, value, context, settled, ready, attachedContext, onChange])
  const message = unavailableReason || (loading ? 'Loading branches…' : context?.vcs === 'svn'
    ? 'SVN workspaces do not support Git branch selection.'
    : context?.vcs === '' ? 'This workspace has no enabled Git repository.'
    : attachedContext ? "The chat's isolated worktree starts from the chosen branch."
    : 'This chat starts from the default or current branch.')

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
        onChange={(next) => {
          setAutoPicked(undefined)
          onChange(next === 'default' ? undefined : next === 'head' ? '' : next.slice('branch:'.length))
        }}
      />
      <div id={`${id}-message`} className="text-xs">
        <p role="status" className="flex items-center gap-1.5" style={{ color: 'var(--text-3)' }}>
          {loading && <LoaderCircle size={12} aria-hidden className="animate-spin motion-reduce:animate-none" />}
          {message}
        </p>
        {!loading && ready && value !== undefined && value === autoPicked && (
          <p style={{ color: 'var(--text-3)' }}>Preselected: furthest ahead (newest commit).</p>
        )}
      </div>
      {!unavailableReason && listed && (
        <Button
          variant="secondary"
          size="sm"
          disabled={disabled}
          aria-disabled={loading || undefined}
          aria-busy={loading || undefined}
          onClick={() => { if (!disabled && !loading) setAttempt((previous) => previous + 1) }}
        >
          {loading && attempt > 0 ? 'Refreshing branches…' : 'Refresh branches'}
        </Button>
      )}
    </div>
  )
}
