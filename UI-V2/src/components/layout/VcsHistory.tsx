import { useCallback, useEffect, useMemo, useRef, useState, type FormEvent } from 'react'
import { ChevronRight, GitBranch, GitFork, GitGraph, List, LoaderCircle, Search, Tag, X } from 'lucide-react'
import type { VcsChangedFile, VcsHistoryEntry, VcsHistoryPath, VcsOriginSummary, VcsType } from '../../store/cpp/types'
import { useAppStore } from '../../store/useAppStore'
import { svnLayoutFor, svnLayoutLabel, svnLayoutText } from '../../utils/svnLayout'
import { Button, Disclosure, IconButton, Notice, Presence, RowButton, SegmentedControl, Tooltip } from '../ui'
import { RepositoryDiffDialog } from '../shared/RepositoryDiffDialog'
import '../shared/review.css'

const PAGE_SIZE = 50

/**
 * `history` is the one line of the working copy's path. `graph`, for a branch or tag, draws that line beside the path
 * it was copied from, down to the copy, then the history they share.
 */
type HistoryMode = 'history' | 'graph'
type Lane = 'branch' | 'origin' | 'copy' | 'shared'
type GraphRow = { entry: VcsHistoryEntry; lane: Lane }

/** What the graph has fetched so far: each side of the fork pages on its own, then the shared history below the copy. */
type GraphData = { ahead: VcsHistoryEntry[]; aheadNext: string; behind: VcsHistoryEntry[]; behindOffset: number; shared: VcsHistoryEntry[]; sharedNext: string; sharedLoaded: boolean }
const EMPTY_GRAPH: GraphData = { ahead: [], aheadNext: '', behind: [], behindOffset: 0, shared: [], sharedNext: '', sharedLoaded: false }

/** Same colour rule as the workspace changed-files list. */
function actionColor(action: string): string {
  return action === 'A' ? 'var(--success)' : action === 'D' ? 'var(--error)' : action === 'R' ? 'var(--info)' : 'var(--warning)'
}

const ACTION_NAMES: Record<string, string> = { A: 'Added', D: 'Deleted', M: 'Modified', R: 'Replaced' }

function relativeTime(date: string, now: number): string {
  const time = Date.parse(date)
  if (!Number.isFinite(time)) return ''
  const minutes = Math.floor(Math.max(0, now - time) / 60_000)
  if (minutes < 1) return 'just now'
  if (minutes < 60) return `${minutes} min ago`
  const hours = Math.floor(minutes / 60)
  if (hours < 24) return `${hours} h ago`
  const days = Math.floor(hours / 24)
  if (days < 31) return `${days} d ago`
  return new Date(time).toLocaleDateString([], { dateStyle: 'medium' })
}

function absoluteTime(date: string): string {
  const time = Date.parse(date)
  return Number.isFinite(time) ? new Date(time).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' }) : ''
}

/**
 * Paged revision history for the chat's workspace: revisions, authors, messages and changed paths, with each path's
 * patch on demand. A branch or tag can also be drawn as a graph beside the path it was copied from. Requests never
 * block the interface and the one in flight is stopped on Stop, a new search, another view or leaving.
 */
export function VcsHistory({ chatId, vcsType, compareOrigin = false }: { chatId: string; vcsType: VcsType; compareOrigin?: boolean }) {
  const getVcsHistory = useAppStore((state) => state.getVcsHistory)
  const getVcsRevisionDiff = useAppStore((state) => state.getVcsRevisionDiff)
  const getVcsOrigin = useAppStore((state) => state.getVcsOrigin)
  const cancelVcsRequest = useAppStore((state) => state.cancelVcsRequest)
  const layouts = useAppStore((state) => state.svnRepositoryLayouts)
  const [mode, setMode] = useState<HistoryMode>('history')
  const [entries, setEntries] = useState<VcsHistoryEntry[]>([])
  const [graph, setGraph] = useState<GraphData>(EMPTY_GRAPH)
  const graphRef = useRef<GraphData>(EMPTY_GRAPH)
  const autoGraph = useRef(false)
  const [next, setNext] = useState('')
  const [workingRevision, setWorkingRevision] = useState('')
  const [location, setLocation] = useState('')
  const [repository, setRepository] = useState({ root: '', uuid: '' })
  const [loading, setLoading] = useState<'first' | 'more' | null>('first')
  const [error, setError] = useState('')
  const [stopped, setStopped] = useState(false)
  const [searchDraft, setSearchDraft] = useState('')
  const [search, setSearch] = useState('')
  const [origin, setOrigin] = useState<VcsOriginSummary | null>(null)
  const [selected, setSelected] = useState<{ revision: string; path: VcsHistoryPath } | null>(null)
  // A revision's changed paths are rendered once it has been opened, so a page of large commits stays light.
  const [opened, setOpened] = useState<ReadonlySet<string>>(new Set())
  const inFlight = useRef('')
  const diffRequest = useRef('')
  const originRequest = useRef('')
  const originRef = useRef<VcsOriginSummary | null>(null)
  const sequence = useRef(0)
  const callSequence = useRef(0)
  const loadToken = useRef(0)
  const now = Date.now()
  const prefix = vcsType === 'svn' ? 'r' : ''
  const layout = svnLayoutFor(layouts, repository.root, repository.uuid)

  const stop = useCallback(() => {
    if (!inFlight.current) return
    if (inFlight.current !== 'pending') cancelVcsRequest(inFlight.current)
    inFlight.current = ''
    setLoading(null)
    setStopped(true)
  }, [cancelVcsRequest])

  const load = useCallback(async (before: string, query: string, view: HistoryMode) => {
    if (inFlight.current && inFlight.current !== 'pending') cancelVcsRequest(inFlight.current)
    const token = ++sequence.current
    // Each bridge call gets its own id so Stop reaches whichever one is running; a newer load makes this one stale.
    const call = async <T,>(run: (requestId: string) => Promise<T>): Promise<T> => {
      const requestId = `vcs-history:${chatId}:${Date.now()}:${++callSequence.current}`
      inFlight.current = requestId
      loadToken.current = token
      return run(requestId)
    }
    const stale = () => loadToken.current !== token || inFlight.current === ''
    loadToken.current = token
    inFlight.current = 'pending'
    setLoading(before ? 'more' : 'first')
    setError('')
    setStopped(false)
    if (!before) { setEntries([]); setNext(''); setOpened(new Set()); graphRef.current = EMPTY_GRAPH; setGraph(EMPTY_GRAPH) }
    try {
      let pageEntries: VcsHistoryEntry[] = []
      let pageNext = ''
      if (view === 'history') {
        const page = await call((requestId) => getVcsHistory(chatId, vcsType, { before: before || undefined, limit: PAGE_SIZE, search: query || undefined, requestId }))
        if (stale()) return
        pageEntries = page.entries
        pageNext = page.next
        if (!before) { setWorkingRevision(page.workingRevision); setLocation(page.location); setRepository({ root: page.repositoryRoot, uuid: page.repositoryUuid }) }
      } else {
        const summary = originRef.current
        const data = { ...graphRef.current }
        const first = !before
        let fetchedFork = false
        const behindLeft = () => (summary?.behindRevisions.length ?? 0) > data.behindOffset
        if (first || data.aheadNext) {
          const reply = await call((requestId) => getVcsOrigin(chatId, vcsType, 'ahead', { before: data.aheadNext || undefined, limit: PAGE_SIZE, requestId }))
          if (stale()) return
          data.ahead = [...data.ahead, ...reply.page.entries]
          data.aheadNext = reply.page.next
          fetchedFork = !first
        }
        if (behindLeft()) {
          // The summary already holds every revision number; details are fetched a page of them at a time.
          const revisions = summary!.behindRevisions.slice(data.behindOffset, data.behindOffset + PAGE_SIZE)
          const reply = await call((requestId) => getVcsOrigin(chatId, vcsType, 'behind', { revisions, requestId }))
          if (stale()) return
          data.behind = [...data.behind, ...reply.page.entries]
          data.behindOffset += PAGE_SIZE
          fetchedFork = fetchedFork || !first
        }
        // Once both sides of the fork are complete, the list continues with the copy and the history they share.
        const forkComplete = !data.aheadNext && !behindLeft()
        if (forkComplete && summary && (!data.sharedLoaded || (!first && !fetchedFork && data.sharedNext))) {
          const page = await call((requestId) => getVcsHistory(chatId, vcsType, { before: data.sharedLoaded ? data.sharedNext : summary.copyRevision, limit: PAGE_SIZE, requestId }))
          if (stale()) return
          data.shared = [...data.shared, ...page.entries]
          data.sharedNext = page.next
          data.sharedLoaded = true
        }
        graphRef.current = data
        setGraph(data)
        pageNext = data.aheadNext || behindLeft() || !data.sharedLoaded || data.sharedNext ? 'more' : ''
      }
      setEntries((current) => before ? [...current, ...pageEntries] : pageEntries)
      setNext(pageNext)
    } catch (reason) {
      if (stale()) return
      setError(reason instanceof Error ? reason.message : 'Failed to load history.')
    }
    inFlight.current = ''
    setLoading(null)
  }, [cancelVcsRequest, chatId, getVcsHistory, getVcsOrigin, vcsType])

  // A search is over the working copy's own line, so the graph gives way to the plain list while one is active.
  const view: HistoryMode = mode === 'graph' && search ? 'history' : mode
  useEffect(() => {
    void load('', search, view)
    return () => { if (inFlight.current) { if (inFlight.current !== 'pending') cancelVcsRequest(inFlight.current); inFlight.current = '' } }
  }, [cancelVcsRequest, load, search, view])
  // An open patch belongs to the dialog, not to the list's view: only leaving History stops it.
  useEffect(() => () => { if (diffRequest.current) { cancelVcsRequest(diffRequest.current); diffRequest.current = '' } }, [cancelVcsRequest])

  // Where this line was copied from. It loads beside the first page and never holds the list up: without it, or if
  // it fails, History is simply the plain list.
  useEffect(() => {
    if (!compareOrigin) return
    const requestId = `vcs-origin:${chatId}:${Date.now()}:${++callSequence.current}`
    originRequest.current = requestId
    void getVcsOrigin(chatId, vcsType, 'summary', { requestId }).then((summary) => {
      if (originRequest.current !== requestId) return
      originRequest.current = ''
      originRef.current = summary
      setOrigin(summary)
    }).catch(() => { if (originRequest.current === requestId) originRequest.current = '' })
    return () => { if (originRequest.current) { cancelVcsRequest(originRequest.current); originRequest.current = '' } }
  }, [cancelVcsRequest, chatId, compareOrigin, getVcsOrigin, vcsType])

  const submitSearch = (event: FormEvent) => {
    event.preventDefault()
    const query = searchDraft.trim()
    if (query === search) void load('', query, mode === 'graph' && query ? 'history' : mode)
    else setSearch(query)
  }

  const working = Number(workingRevision)
  const rows = useMemo<GraphRow[]>(() => {
    if (view !== 'graph') return entries.map((entry) => ({ entry, lane: 'shared' as const }))
    // A side that still has older pages hides nothing newer than what it has loaded, so the two lines interleave
    // correctly: rows older than that wait until the side catches up.
    const oldest = (list: VcsHistoryEntry[]) => list.length ? Number(list[list.length - 1].revision) : 0
    const behindLeft = (originRef.current?.behindRevisions.length ?? 0) > graph.behindOffset
    const floor = Math.max(graph.aheadNext ? oldest(graph.ahead) : 0, behindLeft ? oldest(graph.behind) : 0)
    const fork = [...graph.ahead.map((entry) => ({ entry, lane: 'branch' as const })), ...graph.behind.map((entry) => ({ entry, lane: 'origin' as const }))]
      .filter((row) => Number(row.entry.revision) >= floor)
      .sort((left, right) => Number(right.entry.revision) - Number(left.entry.revision))
    return [...fork, ...graph.shared.map((entry) => ({ entry, lane: entry.revision === originRef.current?.copyRevision ? 'copy' as const : 'shared' as const }))]
  }, [entries, graph, view])
  // The working copy sits at the newest listed revision of its own line that is not ahead of it; its own number is
  // often absent because that revision changed another path.
  const currentRevision = workingRevision ? rows.find((row) => row.lane !== 'origin' && Number(row.entry.revision) <= working)?.entry.revision : undefined
  const getDiff = useCallback(async (id: string, path: string) => {
    const requestId = `vcs-revision-diff:${id}:${Date.now()}:${++callSequence.current}`
    diffRequest.current = requestId
    try { return await getVcsRevisionDiff(id, vcsType, selected?.revision ?? '', path, requestId) }
    finally { if (diffRequest.current === requestId) diffRequest.current = '' }
  }, [getVcsRevisionDiff, selected?.revision, vcsType])
  const closeDiff = useCallback(() => {
    // Closing the dialog stops a patch that is still being fetched.
    if (diffRequest.current) cancelVcsRequest(diffRequest.current)
    diffRequest.current = ''
    setSelected(null)
  }, [cancelVcsRequest])

  const hasOrigin = Boolean(origin?.originPath)
  const originName = origin ? svnLayoutText(origin.originPath, layout) : ''
  // The origin's rows are known to be missing here only when a merge from it is recorded; otherwise they are just
  // what the origin has done since the copy, so nothing is claimed about them.
  const behindCertain = Boolean(origin?.mergesRecorded && !origin.behindFromLog)
  // A branch or tag opens on the graph once its origin is known and the first page has told us where we are, but
  // never over a list the user has started reading, and not when the origin is so far ahead that the branch's own
  // revisions would sit pages down: then Graph is one click away instead.
  const untouched = opened.size === 0 && !selected && !error && !stopped && entries.length <= PAGE_SIZE && Boolean(workingRevision)
  useEffect(() => {
    if (!hasOrigin || autoGraph.current || mode !== 'history' || loading !== null || search) return
    autoGraph.current = true
    if (untouched && (origin?.behindRevisions.length ?? 0) <= PAGE_SIZE) setMode('graph')
  }, [hasOrigin, loading, mode, origin, search, untouched])

  const errorNotice = <Notice tone="error" title="History unavailable" dismissible={false} actions={<Button size="sm" variant="secondary" onClick={() => { void load(rows.length ? next : '', search, view) }}>Retry</Button>}>{error}</Notice>
  const locationLabel = svnLayoutLabel(location, layout)

  return (
    <div className="flex min-h-0 flex-1 flex-col">
      <div className="shrink-0 px-3 pt-3">
        {location && <div className="mb-2 flex items-center gap-2 text-xs" style={{ color: 'var(--text-2)' }}>
          {locationLabel.kind === 'tag' ? <Tag size={14} aria-hidden style={{ color: 'var(--text-3)' }} /> : <GitBranch size={14} aria-hidden style={{ color: 'var(--text-3)' }} />}
          <span className="shrink-0 font-medium" style={{ color: 'var(--text)' }}>{svnLayoutText(location, layout)}</span>
          <Tooltip label={[location, repository.root && `in ${repository.root}`, locationLabel.kind === 'path' && "Set this repository's trunk, branches and tags folders in Settings, Version control."].filter(Boolean).join(' ')}><span className="min-w-0 truncate font-mono" style={{ color: 'var(--text-3)' }}>{location}</span></Tooltip>
          {workingRevision && <span className="ml-auto shrink-0 font-mono">{`Working copy ${prefix}${workingRevision}`}</span>}
        </div>}

        {origin && hasOrigin && <div className="uam-vh-origin" role="group" aria-label="Copied from">
          <GitFork size={14} aria-hidden className="shrink-0" style={{ color: 'var(--text-3)' }} />
          <span className="min-w-0 flex-1 truncate" title={`${origin.branchPath} was copied from ${origin.originPath}@${origin.originRevision} in ${prefix}${origin.copyRevision}`}>
            Copied from <span className="font-medium" style={{ color: 'var(--text)' }}>{originName}</span>
            <span className="font-mono">{`@${prefix}${origin.originRevision} · ${prefix}${origin.copyRevision}`}</span>
          </span>
          <SegmentedControl
            aria-label="History view"
            value={mode}
            onChange={(chosen) => { autoGraph.current = true; setSearchDraft(''); setSearch(''); setMode(chosen) }}
            options={[
              { value: 'history', label: 'List', icon: <List size={14} aria-hidden />, iconOnly: true, tooltip: 'List: this line on its own' },
              { value: 'graph', label: 'Graph', icon: <GitGraph size={14} aria-hidden />, iconOnly: true, tooltip: `Graph: beside ${originName}` },
            ]}
          />
        </div>}

        <form role="search" className="mb-2 flex items-center gap-2" onSubmit={submitSearch}>
          <div className="relative min-w-0 flex-1">
            <Search size={14} aria-hidden className="pointer-events-none absolute left-2 top-1/2 -translate-y-1/2" style={{ color: 'var(--text-3)' }} />
            <input
              type="search"
              className="uam-rv-field w-full"
              style={{ paddingLeft: 28 }}
              placeholder="Search author, message or path"
              aria-label="Search history"
              value={searchDraft}
              onChange={(event) => setSearchDraft(event.target.value)}
            />
          </div>
          {search && <IconButton icon={<X size={14} />} label="Clear history search" onClick={() => { setSearchDraft(''); setSearch('') }} />}
        </form>
      </div>

      <div className="min-h-0 flex-1 overflow-y-auto px-3 pb-3">
        {error && rows.length === 0 && errorNotice}
        {loading === 'first' ? (
          <div role="status" className="flex items-center justify-center gap-2 py-6 text-xs" style={{ color: 'var(--text-3)' }}>
            <LoaderCircle size={14} aria-hidden className="animate-spin motion-reduce:animate-none" />
            <span>Loading history…</span>
            <Button size="sm" variant="ghost" onClick={stop}>Stop</Button>
          </div>
        ) : rows.length === 0 && stopped ? (
          <div className="flex items-center justify-center gap-2 py-6 text-xs" style={{ color: 'var(--text-3)' }}>
            <span>Loading was stopped.</span>
            <Button size="sm" variant="secondary" onClick={() => { void load('', search, view) }}>Load history</Button>
          </div>
        ) : rows.length === 0 && error ? null : rows.length === 0 ? (
          <div className="px-3 py-6 text-center text-xs" style={{ color: 'var(--text-3)' }}>{search ? `No revisions match “${search}”.` : 'No revisions yet.'}</div>
        ) : (
          <>
          {view === 'graph' && <p className="uam-vh-legend" aria-label={`Left rail ${originName}, right rail ${svnLayoutText(origin?.branchPath ?? '', layout)}`}>
            <span data-lane="origin" aria-hidden>{originName}</span>
            <span data-lane="branch" aria-hidden>{svnLayoutText(origin?.branchPath ?? '', layout)}</span>
          </p>}
          <ol className="uam-vh-list" data-graph={view === 'graph' || undefined} aria-label="Revisions">
            {rows.map(({ entry, lane }) => {
              const revision = Number(entry.revision)
              const state = entry.revision === currentRevision ? 'current' : lane !== 'origin' && workingRevision && revision > working ? 'incoming' : 'past'
              const copy = entry.paths.find((path) => path.copyFromPath && path.kind !== 'file')
              const [subject, ...rest] = entry.message.split('\n')
              const body = rest.join('\n').trim()
              return (
                <li key={`${lane}:${entry.revision}`} className="uam-vh-entry" data-state={state} data-lane={view === 'graph' ? lane : undefined}>
                  <span className="uam-vh-entry__dot" aria-hidden />
                  <Disclosure
                    className="uam-vh-entry__disclosure"
                    onOpenChange={(open) => { if (open) setOpened((current) => new Set(current).add(entry.revision)) }}
                    summary={<span className="uam-vh-entry__summary">
                      <span className="uam-vh-entry__line">
                        <span className="uam-vh-entry__revision">{prefix}{entry.revision}</span>
                        <span className="uam-vh-entry__subject">{subject || 'No message'}</span>
                      </span>
                      <span className="uam-vh-entry__meta">
                        <span className="truncate">{entry.author || 'Unknown author'}</span>
                        <span className="shrink-0" title={absoluteTime(entry.date)}>{relativeTime(entry.date, now)}</span>
                        {state === 'current' && <span className="uam-vh-tag" data-tone="accent">Working copy</span>}
                        {state === 'incoming' && <span className="uam-vh-tag" data-tone="info">Not updated yet</span>}
                        {view === 'graph' && lane === 'origin' && <span className="uam-vh-tag" data-tone={behindCertain ? 'info' : undefined}>{behindCertain ? `Only on ${originName}` : `On ${originName}`}</span>}
                        {copy && <span className="uam-vh-tag">{`${svnLayoutText(copy.path, layout)} from ${svnLayoutText(copy.copyFromPath, layout)}`}</span>}
                      </span>
                    </span>}
                  >
                    {opened.has(entry.revision) && <div className="uam-vh-entry__details">
                      {body && <p className="uam-vh-entry__body">{body}</p>}
                      <div className="text-xs" style={{ color: 'var(--text-3)' }}>{absoluteTime(entry.date)}</div>
                      <ul aria-label={`Paths changed in ${prefix}${entry.revision}`}>
                        {entry.paths.map((path) => {
                          const name = path.path.split('/').pop() || path.path
                          const directory = path.path.slice(0, path.path.length - name.length - 1)
                          const row = <>
                            <span className="uam-rv-file__status" style={{ color: actionColor(path.action) }} title={ACTION_NAMES[path.action] ?? path.action}>{path.action || 'M'}</span>
                            <span className="uam-rv-file__path" title={path.copyFromPath ? `${path.path} (copied from ${path.copyFromPath}@${path.copyFromRevision})` : path.path}>
                              <span className="uam-rv-file__name">{name}{path.kind === 'dir' ? '/' : ''}</span>
                              {directory && <span className="uam-rv-file__dir">{directory}</span>}
                            </span>
                          </>
                          return (
                            <li key={`${path.action}:${path.path}`} className="uam-rv-file uam-vh-path">
                              {path.kind === 'dir'
                                ? <span className="uam-vh-path__open">{row}</span>
                                : <RowButton type="button" className="uam-vh-path__open" aria-label={`Review ${ACTION_NAMES[path.action]?.toLowerCase() ?? 'changed'} ${path.path} in ${prefix}${entry.revision}`} onClick={() => setSelected({ revision: entry.revision, path })}>
                                    {row}
                                    <ChevronRight size={14} aria-hidden className="uam-rv-file__chevron" style={{ color: 'var(--text-3)' }} />
                                  </RowButton>}
                            </li>
                          )
                        })}
                      </ul>
                      {entry.pathsOmitted > 0 && <div className="pt-1 text-xs" style={{ color: 'var(--text-3)' }}>
                        {entry.paths.length ? `and ${entry.pathsOmitted} more changed path${entry.pathsOmitted === 1 ? '' : 's'}` : 'This revision changed too many paths to list here.'}
                      </div>}
                    </div>}
                  </Disclosure>
                </li>
              )
            })}
          </ol>
          </>
        )}
        {error && rows.length > 0 && <div className="pt-2">{errorNotice}</div>}
        {loading !== 'first' && rows.length > 0 && !error && (next
          ? <div className="flex items-center gap-2 pt-2">
              <Button variant="secondary" block loading={loading === 'more'} onClick={() => { void load(next, search, view) }}>{loading === 'more' ? 'Loading older revisions…' : 'Load older revisions'}</Button>
              {loading === 'more' && <Button variant="ghost" onClick={stop}>Stop</Button>}
            </div>
          : <div className="pt-3 text-center text-xs" style={{ color: 'var(--text-3)' }}>Start of history</div>)}
      </div>

      <Presence open={Boolean(selected)}>{selected && (
        <RepositoryDiffDialog
          chatId={chatId}
          file={{ path: selected.path.path, status: selected.path.action, staged: false, additions: 0, deletions: 0, binary: false } satisfies VcsChangedFile}
          vcsType={vcsType}
          comparisonLabel={`${ACTION_NAMES[selected.path.action] ?? 'Changed'} in ${prefix}${selected.revision}`}
          showFileStats={false}
          inspectable={false}
          getDiff={getDiff}
          onClose={closeDiff}
        />
      )}</Presence>
    </div>
  )
}
