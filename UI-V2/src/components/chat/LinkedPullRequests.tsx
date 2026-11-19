import { useEffect, useId, useRef, useState } from 'react'
import { Check, CircleDashed, Eye, EyeOff, GitMerge, GitPullRequest, GitPullRequestClosed, TriangleAlert, X } from 'lucide-react'
import { sendToCEF } from '../../ipc/cefBridge'
import { useAppStore } from '../../store/useAppStore'
import type { PullRequestWatch } from '../../store/cpp/types'
import { reportAppFailure } from '../../utils/appFailure'
import { Button, ConfirmDialog, IconButton, Reveal } from '../ui'
import './LinkedPullRequests.css'

type Operation = 'watch' | 'unwatch' | 'unlink'

async function managePullRequestWatch(chatId: string, operation: Operation, pullRequest: string, number?: number) {
  return sendToCEF({ action: 'managePullRequestWatch', payload: { chatId, operation, pullRequest, ...(number ? { number } : {}) } })
}

const STATE_LABEL: Record<PullRequestWatch['state'], string> = { open: 'Open', merged: 'Merged', closed: 'Closed', missing: 'Not found', unknown: 'Checking' }

function StateIcon({ state }: { state: PullRequestWatch['state'] }) {
  if (state === 'merged') return <GitMerge size={14} aria-hidden />
  if (state === 'closed') return <GitPullRequestClosed size={14} aria-hidden />
  if (state === 'missing') return <TriangleAlert size={14} aria-hidden />
  return <GitPullRequest size={14} aria-hidden />
}

const plural = (count: number, noun: string) => `${count} ${noun}${count === 1 ? '' : 's'}`

/** One short sentence for assistive technology; the row shows the same facts as icons with counts. */
export function pullRequestChecksSummary(watch: Pick<PullRequestWatch, 'checksPassed' | 'checksFailed' | 'checksPending'>): string {
  const parts = [
    watch.checksFailed > 0 ? `${plural(watch.checksFailed, 'check')} failed` : '',
    watch.checksPending > 0 ? `${plural(watch.checksPending, 'check')} running` : '',
    watch.checksPassed > 0 ? `${plural(watch.checksPassed, 'check')} passed` : '',
  ].filter(Boolean)
  return parts.length > 0 ? parts.join(', ') : 'No checks reported'
}

function PullRequestRow({ watch }: { watch: PullRequestWatch }) {
  const [busy, setBusy] = useState(false)
  const name = `${watch.repository} #${watch.number}`
  const run = (operation: Operation) => {
    if (busy) return
    setBusy(true)
    void managePullRequestWatch(watch.chatId, operation, watch.repository, watch.number)
      .then((response) => {
        if (!response.ok) reportAppFailure(`${operation === 'watch' ? 'Could not watch' : operation === 'unwatch' ? 'Could not stop watching' : 'Could not unlink'} ${name}. ${response.error || 'Try again.'}`, '')
      })
      .finally(() => setBusy(false))
  }
  const finished = watch.state === 'merged' || watch.state === 'closed'
  const hasChecks = watch.checksPassed + watch.checksFailed + watch.checksPending > 0
  return (
    <li className="uam-linked-pr">
      <span className="uam-linked-pr__state"><StateIcon state={watch.state} />{STATE_LABEL[watch.state]}</span>
      <Button asLink onClick={() => window.open(watch.url, '_blank', 'noopener')} aria-label={`Open ${name} on GitHub`}>{name}</Button>
      <span className="uam-linked-pr__title" title={watch.title}>{watch.title}</span>
      <span className="uam-linked-pr__facts">
        {hasChecks && <span className="uam-linked-pr__facts" role="img" aria-label={pullRequestChecksSummary(watch)}>
          {watch.checksFailed > 0 && <span className="uam-linked-pr__fact" data-tone="error"><X size={13} aria-hidden />{watch.checksFailed}</span>}
          {watch.checksPending > 0 && <span className="uam-linked-pr__fact"><CircleDashed size={13} aria-hidden />{watch.checksPending}</span>}
          {watch.checksPassed > 0 && <span className="uam-linked-pr__fact" data-tone="success"><Check size={13} aria-hidden />{watch.checksPassed}</span>}
        </span>}
        {watch.conflicting && <span className="uam-linked-pr__fact" data-tone="warning"><TriangleAlert size={13} aria-hidden />Conflicts</span>}
        {watch.watching && watch.paused && <span className="uam-linked-pr__fact" data-tone="warning"><TriangleAlert size={13} aria-hidden />Paused by GitHub's rate limit</span>}
        {watch.watching && !watch.paused && watch.readFailing && <span className="uam-linked-pr__fact" data-tone="warning"><TriangleAlert size={13} aria-hidden />GitHub could not be read</span>}
      </span>
      <span className="uam-linked-pr__actions">
        <IconButton size="sm" icon={watch.watching ? <Eye size={14} /> : <EyeOff size={14} />} active={watch.watching} disabled={busy || (finished && !watch.watching)}
          disabledReason={finished && !watch.watching ? `This pull request is ${watch.state}, so there is nothing to watch.` : undefined}
          label={`Watch ${name}`}
          tooltip={watch.watching ? `Watching: this chat is woken when ${name} changes. Click to stop.` : `Not watching ${name}. Click to watch.`}
          onClick={() => run(watch.watching ? 'unwatch' : 'watch')} />
        <IconButton size="sm" icon={<X size={14} />} disabled={busy} label={`Unlink ${name}`} onClick={() => run('unlink')} />
      </span>
    </li>
  )
}

/** Pull requests linked to this chat: state, checks, an eye to stop or resume watching and a control to unlink. */
export function LinkedPullRequests({ chatId }: { chatId: string }) {
  const watches = useAppStore((state) => state.prWatches)
  const rows = watches.filter((watch) => watch.chatId === chatId)
  return (
    <Reveal open={rows.length > 0}>
      <nav aria-label="Linked pull requests" className="uam-linked-prs">
        <ul className="uam-linked-prs__list">
          {rows.map((watch) => <PullRequestRow key={`${watch.repository}#${watch.number}`} watch={watch} />)}
        </ul>
      </nav>
    </Reveal>
  )
}

/** Asks for the pull request to watch. GitHub is read afterwards, so a wrong link shows as "Not found" on its row. */
export function WatchPullRequestDialog({ chatId, open, onClose }: { chatId: string; open: boolean; onClose: () => void }) {
  const [value, setValue] = useState('')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const input = useRef<HTMLInputElement>(null)
  const fieldId = useId()
  useEffect(() => {
    if (!open) return
    setValue(''); setError('')
    // Runs after the dialog's own open effect, which focuses Cancel.
    input.current?.focus()
  }, [open])
  const submit = () => {
    if (busy || !value.trim()) return
    setBusy(true); setError('')
    void managePullRequestWatch(chatId, 'watch', value.trim())
      .then((response) => { if (response.ok) onClose(); else setError(response.error || 'The pull request could not be watched. Try again.') })
      .finally(() => setBusy(false))
  }
  return (
    <ConfirmDialog open={open} tone="primary" title="Watch a pull request" confirmLabel="Watch" busy={busy} error={error} confirmDisabled={!value.trim()}
      onCancel={onClose} onConfirm={submit}>
      UAM checks it on GitHub about every 2 minutes and wakes this chat when checks fail or pass, someone comments or reviews, or the branch starts to conflict.
      <span className="uam-linked-pr-field">
        <label htmlFor={fieldId} className="text-xs font-medium" style={{ color: 'var(--text)' }}>Pull request link or owner/name#number</label>
        <input ref={input} id={fieldId} className="uam-field" value={value} disabled={busy} placeholder="https://github.com/owner/name/pull/12" autoComplete="off" spellCheck={false}
          onChange={(event) => setValue(event.target.value)} onKeyDown={(event) => { if (event.key === 'Enter') { event.preventDefault(); submit() } }} />
      </span>
    </ConfirmDialog>
  )
}
