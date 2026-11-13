import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import type { VcsCommitStatus, VcsHistoryEntry, VcsHistoryPage, VcsOriginReply } from '../../store/cpp/types'
import { useAppStore } from '../../store/useAppStore'
import { VcsCommitPanel } from './VcsCommitPanel'
import { VcsHistory } from './VcsHistory'
import { sanitizeSvnRepositoryLayouts, sanitizeVcsOriginReply } from '../../store/cpp/sanitizers'
import { svnLayoutFor, svnLayoutLabel } from '../../utils/svnLayout'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

let host: HTMLDivElement
let root: Root

function entry(revision: number, overrides: Partial<VcsHistoryEntry> = {}): VcsHistoryEntry {
  return {
    revision: String(revision), author: 'dana', date: '2026-01-02T03:04:05.000000Z', message: `change ${revision}`,
    paths: [{ action: 'M', path: `/branches/feat/file-${revision}.txt`, kind: 'file', copyFromPath: '', copyFromRevision: '' }], pathsOmitted: 0, ...overrides,
  }
}

const firstPage: VcsHistoryPage = {
  entries: [
    entry(7),
    entry(6, { message: 'branch edit\n\nLonger explanation.' }),
    entry(5, { message: 'branch feat', paths: [{ action: 'A', path: '/branches/feat', kind: 'dir', copyFromPath: '/trunk', copyFromRevision: '4' }] }),
  ],
  next: '4', workingRevision: '6', location: '^/branches/feat', repositoryRoot: 'https://svn.example.test/repo', repositoryUuid: 'd7397596-24e3-4444-990a-c95097028d41',
}
const page = (entries: VcsHistoryEntry[], next = ''): VcsHistoryPage => ({ entries, next, workingRevision: '', location: '', repositoryRoot: '', repositoryUuid: '' })
const originReply = (overrides: Partial<VcsOriginReply> = {}): VcsOriginReply => ({ ...sanitizeVcsOriginReply({})!, ...overrides })
const summary = originReply({ branchPath: '/branches/feat', originPath: '/trunk', copyRevision: '5', originRevision: '4', behindRevisions: ['9', '8'] })

function deferred<T>() {
  let resolve!: (value: T) => void
  let reject!: (reason: Error) => void
  const promise = new Promise<T>((resolvePromise, rejectPromise) => { resolve = resolvePromise; reject = rejectPromise })
  return { promise, resolve, reject }
}

function button(name: string): HTMLButtonElement {
  const found = [...document.body.querySelectorAll<HTMLButtonElement>('button')].find((candidate) => (candidate.getAttribute('aria-label') ?? candidate.textContent ?? '').includes(name))
  if (!found) throw new Error(`No button named ${name}`)
  return found
}

beforeEach(() => {
  host = document.createElement('div')
  document.body.appendChild(host)
  root = createRoot(host)
  useAppStore.setState({
    getVcsHistory: vi.fn(async (_id, _type, options) => options?.before ? page([entry(4), entry(3)]) : firstPage),
    getVcsOrigin: vi.fn(async () => originReply()),
    svnRepositoryLayouts: [],
    getVcsRevisionDiff: vi.fn(async () => 'Index: file\n--- file\t(revision 5)\n+++ file\t(revision 6)\n@@ -1 +1,2 @@\n x\n+y\n'),
    cancelVcsRequest: vi.fn(),
  })
})

afterEach(() => {
  act(() => root.unmount())
  host.remove()
})

describe('svnLayoutLabel', () => {
  it('names trunk, branches and tags from the conventional layout and leaves other paths alone', () => {
    expect(svnLayoutLabel('^/trunk')).toEqual({ kind: 'trunk', name: 'trunk' })
    expect(svnLayoutLabel('^/project/branches/feat/sub')).toEqual({ kind: 'branch', name: 'feat' })
    expect(svnLayoutLabel('/tags/v1.2')).toEqual({ kind: 'tag', name: 'v1.2' })
    expect(svnLayoutLabel('^/product/lines/main')).toEqual({ kind: 'path', name: '/product/lines/main' })
    expect(svnLayoutLabel('^/')).toEqual({ kind: 'path', name: 'Repository root' })
    expect(svnLayoutLabel('^/branches')).toEqual({ kind: 'path', name: '/branches' })
  })

  it('uses the repository\'s own folders when a layout is saved for its root URL or UUID', () => {
    const layouts = [
      { repository: 'https://svn.example.test/repo/', trunk: 'main', branches: 'lines/dev', tags: 'releases' },
      { repository: 'D7397596-24E3-4444-990A-C95097028D41', trunk: 'head', branches: 'work', tags: 'labels' },
    ]
    const byUrl = svnLayoutFor(layouts, 'https://svn.example.test/repo', '')
    expect(svnLayoutLabel('^/main/src', byUrl)).toEqual({ kind: 'trunk', name: 'main' })
    expect(svnLayoutLabel('^/shop/lines/dev/payments', byUrl)).toEqual({ kind: 'branch', name: 'payments' })
    expect(svnLayoutLabel('^/releases/2.4', byUrl)).toEqual({ kind: 'tag', name: '2.4' })
    // The standard names mean nothing in this repository.
    expect(svnLayoutLabel('^/branches/feat', byUrl)).toEqual({ kind: 'path', name: '/branches/feat' })
    expect(svnLayoutLabel('^/lines/other', byUrl)).toEqual({ kind: 'path', name: '/lines/other' })
    expect(svnLayoutLabel('^/work/x', svnLayoutFor(layouts, 'https://elsewhere.test/r', 'd7397596-24e3-4444-990a-c95097028d41'))).toEqual({ kind: 'branch', name: 'x' })
    expect(svnLayoutLabel('^/branches/feat', svnLayoutFor(layouts, 'https://other.test/repo', 'ffff'))).toEqual({ kind: 'branch', name: 'feat' })
  })

  it('accepts only complete, unique layouts with folders inside the repository', () => {
    expect(sanitizeSvnRepositoryLayouts([{ repository: ' https://svn.example.test/repo/ ', trunk: 'main', branches: ' lines/dev ', tags: 'releases' }]))
      .toEqual([{ repository: 'https://svn.example.test/repo', trunk: 'main', branches: 'lines/dev', tags: 'releases' }])
    expect(sanitizeSvnRepositoryLayouts([])).toEqual([])
    for (const bad of [
      [{ repository: '', trunk: 'trunk', branches: 'branches', tags: 'tags' }],
      [{ repository: 'r', trunk: '/trunk', branches: 'branches', tags: 'tags' }],
      [{ repository: 'r', trunk: 'trunk', branches: 'a/../b', tags: 'tags' }],
      [{ repository: 'r', trunk: 'trunk', branches: 'branches' }],
      [{ repository: 'r', trunk: 't', branches: 'b', tags: 'g' }, { repository: 'r/', trunk: 't', branches: 'b', tags: 'g' }],
      'not a list',
    ]) expect(sanitizeSvnRepositoryLayouts(bad)).toBeUndefined()
  })
})

describe('VcsHistory', () => {
  it('lists revisions with the working copy, incoming and branch-point markers', async () => {
    await act(async () => root.render(<VcsHistory chatId="chat-a" vcsType="svn" />))
    const rows = [...host.querySelectorAll<HTMLLIElement>('.uam-vh-entry')]
    expect(rows.map((row) => row.dataset.state)).toEqual(['incoming', 'current', 'past'])
    expect(rows[0].textContent).toContain('r7')
    expect(rows[0].textContent).toContain('Not updated yet')
    expect(rows[1].textContent).toContain('Working copy')
    expect(rows[1].querySelector('.uam-vh-entry__subject')!.textContent).toBe('branch edit')
    expect(rows[2].textContent).toContain('Branch feat from trunk')
    expect(host.textContent).toContain('Working copy r6')
    expect(host.textContent).toContain('^/branches/feat')
    expect(useAppStore.getState().getVcsHistory).toHaveBeenCalledWith('chat-a', 'svn', expect.objectContaining({ limit: 50, before: undefined }))
  })

  it('marks the newest listed revision the working copy already has when its own number is not listed', async () => {
    useAppStore.setState({ getVcsHistory: vi.fn(async () => ({ ...page([entry(9), entry(7), entry(5)]), workingRevision: '8', location: '^/trunk' })) })
    await act(async () => root.render(<VcsHistory chatId="chat-a" vcsType="svn" />))
    expect([...host.querySelectorAll<HTMLLIElement>('.uam-vh-entry')].map((row) => row.dataset.state)).toEqual(['incoming', 'current', 'past'])
  })

  it('keeps the listed revisions and offers Retry beside them when an older page fails', async () => {
    const getVcsHistory = vi.fn<(id: string, type: string, options?: { before?: string }) => Promise<VcsHistoryPage>>(async (_id, _type, options) => {
      if (options?.before) throw new Error('E170013: Unable to connect to a repository')
      return firstPage
    })
    useAppStore.setState({ getVcsHistory })
    await act(async () => root.render(<VcsHistory chatId="chat-a" vcsType="svn" />))
    await act(async () => button('Load older revisions').click())
    expect(host.querySelectorAll('.uam-vh-entry')).toHaveLength(3)
    const alert = host.querySelector('[role="alert"]')!
    expect(alert.textContent).toContain('E170013')
    // Below the list, where the user asked for more.
    expect(host.querySelector('.uam-vh-list')!.compareDocumentPosition(alert) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy()
    getVcsHistory.mockImplementation(async () => page([entry(4)]))
    await act(async () => button('Retry').click())
    expect(getVcsHistory).toHaveBeenLastCalledWith('chat-a', 'svn', expect.objectContaining({ before: '4' }))
    expect(host.querySelectorAll('.uam-vh-entry')).toHaveLength(4)
  })

  it('appends the next page from the cursor and ends at the start of history', async () => {
    await act(async () => root.render(<VcsHistory chatId="chat-a" vcsType="svn" />))
    await act(async () => button('Load older revisions').click())
    expect(useAppStore.getState().getVcsHistory).toHaveBeenLastCalledWith('chat-a', 'svn', expect.objectContaining({ before: '4' }))
    expect([...host.querySelectorAll('.uam-vh-entry__revision')].map((node) => node.textContent)).toEqual(['r7', 'r6', 'r5', 'r4', 'r3'])
    expect(host.textContent).toContain('Start of history')
    expect(host.textContent).not.toContain('Load older revisions')
  })

  it('opens one changed path as that revision\'s patch and offers no diff for a folder', async () => {
    await act(async () => root.render(<VcsHistory chatId="chat-a" vcsType="svn" />))
    const rows = [...host.querySelectorAll<HTMLLIElement>('.uam-vh-entry')]
    const summary = rows[1].querySelector<HTMLButtonElement>('.uam-disclosure__summary')!
    expect(summary.getAttribute('aria-expanded')).toBe('false')
    await act(async () => summary.click())
    expect(summary.getAttribute('aria-expanded')).toBe('true')
    expect(rows[1].textContent).toContain('Longer explanation.')
    // Paths are rendered once a revision is opened; a copied folder is listed but has no patch to open.
    expect(rows[2].querySelector('.uam-vh-path')).toBeNull()
    await act(async () => rows[2].querySelector<HTMLButtonElement>('.uam-disclosure__summary')!.click())
    expect(rows[2].querySelector('.uam-vh-path')!.textContent).toContain('feat/')
    expect(rows[2].querySelector('button.uam-vh-path__open')).toBeNull()

    await act(async () => button('Review modified /branches/feat/file-6.txt in r6').click())
    expect(useAppStore.getState().getVcsRevisionDiff).toHaveBeenCalledWith('chat-a', 'svn', '6', '/branches/feat/file-6.txt', expect.stringContaining('vcs-revision-diff:chat-a'))
    const dialog = document.body.querySelector('[role="dialog"]')!
    expect(dialog.textContent).toContain('Modified in r6')
    expect(dialog.textContent).not.toContain('Inspect file')
  })

  it('stops the request in flight on Stop and when the view goes away', async () => {
    const pending = deferred<VcsHistoryPage>()
    const getVcsHistory = vi.fn(() => pending.promise)
    useAppStore.setState({ getVcsHistory })
    await act(async () => root.render(<VcsHistory chatId="chat-a" vcsType="svn" />))
    expect(host.querySelector('[role="status"]')!.textContent).toContain('Loading history')
    const requestId = getVcsHistory.mock.calls[0][2].requestId
    await act(async () => button('Stop').click())
    expect(useAppStore.getState().cancelVcsRequest).toHaveBeenCalledWith(requestId)
    expect(host.querySelector('[role="status"]')).toBeNull()
    expect(host.textContent).toContain('Loading was stopped.')
    // The stopped reply is ignored when it arrives late.
    await act(async () => pending.resolve(firstPage))
    expect(host.querySelectorAll('.uam-vh-entry')).toHaveLength(0)

    const second = deferred<VcsHistoryPage>()
    getVcsHistory.mockImplementation(() => second.promise)
    await act(async () => root.render(<VcsHistory key="again" chatId="chat-a" vcsType="svn" />))
    const secondId = getVcsHistory.mock.calls.at(-1)![2].requestId
    await act(async () => root.render(<div />))
    expect(useAppStore.getState().cancelVcsRequest).toHaveBeenCalledWith(secondId)
  })

  it('searches on submit and shows the system error with Retry', async () => {
    const getVcsHistory = vi.fn<(id: string, type: string, options?: { search?: string }) => Promise<VcsHistoryPage>>(async () => { throw new Error('E170013: Unable to connect to a repository') })
    useAppStore.setState({ getVcsHistory })
    await act(async () => root.render(<VcsHistory chatId="chat-a" vcsType="svn" />))
    expect(host.querySelector('[role="alert"]')!.textContent).toContain('E170013')

    getVcsHistory.mockImplementation(async () => ({ ...page([]), workingRevision: '6', location: '^/trunk' }))
    const input = host.querySelector<HTMLInputElement>('[aria-label="Search history"]')!
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, 'fix crash')
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => input.form!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })))
    expect(getVcsHistory).toHaveBeenLastCalledWith('chat-a', 'svn', expect.objectContaining({ search: 'fix crash' }))
    expect(host.textContent).toContain('No revisions match “fix crash”.')
    expect(host.querySelector('[role="alert"]')).toBeNull()
  })
})

describe('VcsHistory branch graph', () => {
  function useOrigin(overrides: Partial<VcsOriginReply> = {}) {
    const getVcsOrigin = vi.fn(async (_id: string, _type: string, view: string, options?: { before?: string; revisions?: string[] }): Promise<VcsOriginReply> => {
      if (view === 'summary') return { ...summary, ...overrides }
      if (view === 'ahead') return originReply({ page: options?.before ? page([entry(6)]) : page([entry(10), entry(7)], '6') })
      return originReply({ page: page((options?.revisions ?? []).map((revision) => entry(Number(revision), { paths: [{ action: 'M', path: `/trunk/file-${revision}.txt`, kind: 'file', copyFromPath: '', copyFromRevision: '' }] }))) })
    })
    useAppStore.setState({ getVcsOrigin })
    return getVcsOrigin
  }
  const toggle = (name: string) => host.querySelector<HTMLButtonElement>(`[aria-label="History view"] button[aria-label="${name}"]`)!
  const copiedFrom = () => host.querySelector('[aria-label="Copied from"]')
  const lanes = () => [...host.querySelectorAll<HTMLLIElement>('.uam-vh-entry')].map((row) => `${row.querySelector('.uam-vh-entry__revision')!.textContent}:${row.dataset.lane}`)

  it('shows the copy point and a List or Graph switch on one line, and nothing else about the origin', async () => {
    const getVcsOrigin = useOrigin()
    await act(async () => root.render(<VcsHistory chatId="chat-a" vcsType="svn" compareOrigin />))
    await act(async () => {})
    expect(copiedFrom()!.textContent).toBe('Copied from trunk@r4 · r5')
    expect([...host.querySelectorAll('[aria-label="History view"] button')].map((option) => option.getAttribute('aria-label'))).toEqual(['List', 'Graph'])
    expect(toggle('Graph').getAttribute('aria-pressed')).toBe('true')
    expect(host.textContent).not.toMatch(/Ahead|Behind|Diff|not here yet/)
    expect(getVcsOrigin).toHaveBeenCalledWith('chat-a', 'svn', 'summary', expect.objectContaining({ requestId: expect.stringContaining('vcs-origin:chat-a') }))
  })

  it('opens a branch on a graph: its own revisions and the origin\'s on two lanes down to the copy, then shared history', async () => {
    const getVcsOrigin = useOrigin({ mergesRecorded: true })
    const getVcsHistory = vi.fn(async (_id: string, _type: string, options?: { before?: string }) => options?.before === '5'
      ? page([entry(5, { message: 'branch feat', paths: [{ action: 'A', path: '/branches/feat', kind: 'dir', copyFromPath: '/trunk', copyFromRevision: '4' }] }), entry(4), entry(3)])
      : { ...firstPage, workingRevision: '7' })
    useAppStore.setState({ getVcsHistory })
    await act(async () => root.render(<VcsHistory chatId="chat-a" vcsType="svn" compareOrigin />))
    await act(async () => {})
    // The branch still has an older page, so nothing older than its oldest loaded revision is drawn yet.
    expect(lanes()).toEqual(['r10:branch', 'r9:origin', 'r8:origin', 'r7:branch'])
    expect(host.querySelector('.uam-vh-list')!.hasAttribute('data-graph')).toBe(true)
    expect(host.querySelector('.uam-vh-legend')!.textContent).toBe('trunkBranch feat')
    const rows = [...host.querySelectorAll<HTMLLIElement>('.uam-vh-entry')]
    expect(rows[0].dataset.state).toBe('incoming')
    // A merge from trunk is recorded, so these are known to be missing from the branch.
    expect(rows[1].textContent).toContain('Only on trunk')
    expect(rows[1].dataset.state).toBe('past')
    expect(rows[3].dataset.state).toBe('current')

    await act(async () => button('Load older revisions').click())
    expect(getVcsOrigin).toHaveBeenCalledWith('chat-a', 'svn', 'ahead', expect.objectContaining({ before: '6' }))
    expect(getVcsHistory).toHaveBeenLastCalledWith('chat-a', 'svn', expect.objectContaining({ before: '5' }))
    expect(lanes()).toEqual(['r10:branch', 'r9:origin', 'r8:origin', 'r7:branch', 'r6:branch', 'r5:copy', 'r4:shared', 'r3:shared'])
    expect(host.textContent).toContain('Branch feat from trunk')
    expect(host.textContent).toContain('Start of history')

    // Searching is over the working copy's own line, so it shows the plain list.
    const input = host.querySelector<HTMLInputElement>('[aria-label="Search history"]')!
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, 'fix')
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => input.form!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })))
    expect(getVcsHistory).toHaveBeenLastCalledWith('chat-a', 'svn', expect.objectContaining({ search: 'fix' }))
    expect(host.querySelector('.uam-vh-list')!.hasAttribute('data-graph')).toBe(false)
  })

  it('claims nothing about the origin\'s revisions unless a merge from it is recorded', async () => {
    useOrigin()
    await act(async () => root.render(<VcsHistory chatId="chat-a" vcsType="svn" compareOrigin />))
    await act(async () => {})
    const originRow = [...host.querySelectorAll<HTMLLIElement>('.uam-vh-entry')].find((row) => row.dataset.lane === 'origin')!
    expect(originRow.textContent).toContain('On trunk')
    expect(host.textContent).not.toContain('Only on trunk')

    useOrigin({ mergesRecorded: true, behindFromLog: true })
    await act(async () => root.render(<VcsHistory key="fallback" chatId="chat-a" vcsType="svn" compareOrigin />))
    await act(async () => {})
    expect(host.textContent).not.toContain('Only on trunk')
  })

  it('switches between the list and the graph, and pages both sides of the fork', async () => {
    const behindRevisions = Array.from({ length: 50 }, (_, index) => String(200 - index))
    const getVcsOrigin = useOrigin({ behindRevisions })
    await act(async () => root.render(<VcsHistory chatId="chat-a" vcsType="svn" compareOrigin />))
    await act(async () => {})
    expect(getVcsOrigin).toHaveBeenCalledWith('chat-a', 'svn', 'behind', expect.objectContaining({ revisions: behindRevisions }))
    expect(lanes().filter((lane) => lane.endsWith(':origin'))).toHaveLength(50)
    await act(async () => toggle('List').click())
    expect(host.querySelector('.uam-vh-list')!.hasAttribute('data-graph')).toBe(false)
    expect(host.querySelectorAll('.uam-vh-entry')).toHaveLength(3)
    await act(async () => toggle('Graph').click())
    expect(host.querySelector('.uam-vh-list')!.hasAttribute('data-graph')).toBe(true)
  })

  it('leaves a list the user is already reading alone, and opens far-behind branches on the list', async () => {
    const pending = deferred<VcsOriginReply>()
    const getVcsOrigin = vi.fn((_id: string, _type: string, view: string): Promise<VcsOriginReply> => view === 'summary' ? pending.promise : Promise.resolve(originReply()))
    useAppStore.setState({ getVcsOrigin })
    await act(async () => root.render(<VcsHistory chatId="chat-a" vcsType="svn" compareOrigin />))
    // The slow summary arrives after a revision was opened: the list stays, and Graph is offered.
    await act(async () => host.querySelector<HTMLButtonElement>('.uam-vh-entry .uam-disclosure__summary')!.click())
    await act(async () => pending.resolve(summary))
    expect(host.querySelector('.uam-vh-list')!.hasAttribute('data-graph')).toBe(false)
    expect(host.querySelector('.uam-vh-entry .uam-disclosure__summary')!.getAttribute('aria-expanded')).toBe('true')
    expect(toggle('List').getAttribute('aria-pressed')).toBe('true')
    await act(async () => toggle('Graph').click())
    expect(host.querySelector('.uam-vh-list')!.hasAttribute('data-graph')).toBe(true)

    useOrigin({ behindRevisions: Array.from({ length: 51 }, (_, index) => String(500 - index)) })
    await act(async () => root.render(<VcsHistory key="far" chatId="chat-a" vcsType="svn" compareOrigin />))
    await act(async () => {})
    expect(host.querySelector('.uam-vh-list')!.hasAttribute('data-graph')).toBe(false)
  })

  it('is just the list for a main line, without the capability, or when the origin cannot be read', async () => {
    const mainLine = useOrigin({ originPath: '' })
    await act(async () => root.render(<VcsHistory chatId="chat-a" vcsType="svn" compareOrigin />))
    await act(async () => {})
    expect(copiedFrom()).toBeNull()
    expect(host.querySelectorAll('.uam-vh-entry')).toHaveLength(3)
    mainLine.mockClear()
    await act(async () => root.render(<VcsHistory key="off" chatId="chat-a" vcsType="svn" />))
    expect(mainLine).not.toHaveBeenCalled()

    useAppStore.setState({ getVcsOrigin: vi.fn(async (): Promise<VcsOriginReply> => { throw new Error('E170013: Unable to connect to a repository') }) })
    await act(async () => root.render(<VcsHistory key="failed" chatId="chat-a" vcsType="svn" compareOrigin />))
    await act(async () => {})
    expect(copiedFrom()).toBeNull()
    expect(host.querySelector('[role="alert"]')).toBeNull()
    expect(host.querySelectorAll('.uam-vh-entry')).toHaveLength(3)
  })

  it('names branches from the layout saved for the repository', async () => {
    useAppStore.setState({
      svnRepositoryLayouts: [{ repository: 'https://svn.example.test/repo', trunk: 'main', branches: 'lines', tags: 'releases' }],
      getVcsHistory: vi.fn(async () => ({ ...firstPage, location: '^/lines/payments', entries: [entry(5, { paths: [{ action: 'A', path: '/lines/payments', kind: 'dir', copyFromPath: '/main', copyFromRevision: '4' }] })] })),
    })
    useOrigin({ branchPath: '/lines/payments', originPath: '/main' })
    await act(async () => root.render(<VcsHistory chatId="chat-a" vcsType="svn" compareOrigin />))
    await act(async () => {})
    expect(host.textContent).toContain('Branch payments')
    expect(host.querySelector('.uam-vh-legend')!.textContent).toBe('mainBranch payments')
    expect(copiedFrom()!.textContent).toBe('Copied from main@r4 · r5')
    // The copy row sits below both sides of the fork.
    await act(async () => button('Load older revisions').click())
    expect(host.textContent).toContain('Branch payments from main')
  })
})

describe('commit panel SVN actions', () => {
  const svnStatus: VcsCommitStatus = {
    available: true, vcsTypes: ['svn'], activeVcsType: 'svn', workspaceDirectory: '/tmp/a', branchOrRevision: 'r6',
    changedFiles: [], lineStatsReady: true, capabilities: ['status', 'diff', 'commit', 'history', 'update', 'originComparison'], warning: '', error: '',
  }

  beforeEach(() => {
    useAppStore.setState({
      sessions: [{ id: 'chat-a', name: 'A', viewMode: 'chat', folderId: null, workspaceDirectory: '/tmp/a', createdAt: new Date(), updatedAt: new Date() }],
      activeSessionId: 'chat-a', gitEnabled: true, svnEnabled: true,
      getVcsCommitStatus: vi.fn(async () => svnStatus),
      updateVcsWorkspace: vi.fn(async () => ({ ok: true, conflicts: false, revision: '7', output: 'Updated to revision 7.', error: '' })),
    })
  })

  it('offers History and Update only when the active system declares them', async () => {
    await act(async () => root.render(<VcsCommitPanel />))
    expect(host.querySelector('[role="tablist"]')!.textContent).toContain('History')
    expect(button('Update')).toBeTruthy()

    await act(async () => { useAppStore.setState({ getVcsCommitStatus: vi.fn(async () => ({ ...svnStatus, vcsTypes: ['git'], activeVcsType: 'git', branchOrRevision: 'main', capabilities: ['status', 'diff', 'commit'] }) as VcsCommitStatus) }) })
    await act(async () => button('Refresh VCS status').click())
    expect([...host.querySelectorAll('[role="tab"]')].map((tab) => tab.textContent)).not.toContain('History')
    expect([...host.querySelectorAll('button')].some((candidate) => candidate.textContent === 'Update')).toBe(false)
  })

  it('sets the commit form apart with the panel\'s one heading and adds no heading to History', async () => {
    await act(async () => root.render(<VcsCommitPanel />))
    const headings = () => [...host.querySelectorAll('.uam-rv-section .uam-section-header__title')].map((node) => node.textContent)
    expect(headings()).toEqual(['Commit'])
    await act(async () => [...host.querySelectorAll<HTMLButtonElement>('[role="tab"]')].find((tab) => tab.textContent === 'History')!.click())
    expect(headings()).toEqual([])
  })

  it('shows the history when its tab is chosen', async () => {
    await act(async () => root.render(<VcsCommitPanel />))
    await act(async () => [...host.querySelectorAll<HTMLButtonElement>('[role="tab"]')].find((tab) => tab.textContent === 'History')!.click())
    expect(useAppStore.getState().getVcsHistory).toHaveBeenCalledWith('chat-a', 'svn', expect.anything())
    expect(host.querySelectorAll('.uam-vh-entry')).toHaveLength(3)
  })

  it('updates the working copy, reports the revision reached and warns about conflicts', async () => {
    await act(async () => root.render(<VcsCommitPanel />))
    await act(async () => button('Update').click())
    expect(useAppStore.getState().updateVcsWorkspace).toHaveBeenCalledWith('chat-a', 'svn', expect.stringContaining('vcs-update:chat-a'))
    expect(host.querySelector('[role="status"]')!.textContent).toContain('Updated to r7.')

    await act(async () => { useAppStore.setState({ updateVcsWorkspace: vi.fn(async () => ({ ok: true, conflicts: true, revision: '8', output: '', error: '' })) }) })
    await act(async () => button('Update').click())
    expect(host.querySelector('[role="status"]')!.textContent).toContain('Updated to r8 with conflicts. Resolve them before committing.')

    await act(async () => { useAppStore.setState({ updateVcsWorkspace: vi.fn(async () => ({ ok: false, conflicts: false, revision: '', output: '', error: 'Stop the active chat runtime before updating the working copy.' })) }) })
    await act(async () => button('Update').click())
    expect(host.querySelector('[role="status"]')!.textContent).toContain('Stop the active chat runtime')
  })

  it('offers Stop while an update runs and stops that request', async () => {
    const pending = deferred<{ ok: boolean; conflicts: boolean; revision: string; output: string; error: string }>()
    const updateVcsWorkspace = vi.fn((_id: string, _type: string, _requestId?: string) => pending.promise)
    await act(async () => { useAppStore.setState({ updateVcsWorkspace }) })
    await act(async () => root.render(<VcsCommitPanel />))
    await act(async () => button('Update').click())
    expect(button('Refresh VCS status').disabled).toBe(true)
    await act(async () => button('Stop').click())
    expect(useAppStore.getState().cancelVcsRequest).toHaveBeenCalledWith(updateVcsWorkspace.mock.calls[0][2])
    await act(async () => pending.resolve({ ok: false, conflicts: false, revision: '', output: '', error: 'The update was stopped.' }))
    expect(host.querySelector('[role="status"]')!.textContent).toContain('The update was stopped.')
  })

  it('keeps a running update visible with Stop after switching to another chat and back', async () => {
    const pending = deferred<{ ok: boolean; conflicts: boolean; revision: string; output: string; error: string }>()
    const updateVcsWorkspace = vi.fn((_id: string, _type: string, _requestId?: string) => pending.promise)
    await act(async () => { useAppStore.setState({
      sessions: [
        { id: 'chat-a', name: 'A', viewMode: 'chat', folderId: null, workspaceDirectory: '/tmp/a', createdAt: new Date(), updatedAt: new Date() },
        { id: 'chat-b', name: 'B', viewMode: 'chat', folderId: null, workspaceDirectory: '/tmp/b', createdAt: new Date(), updatedAt: new Date() },
      ],
      updateVcsWorkspace,
    }) })
    await act(async () => root.render(<VcsCommitPanel />))
    await act(async () => button('Update').click())
    await act(async () => { useAppStore.setState({ activeSessionId: 'chat-b' }) })
    expect([...host.querySelectorAll('button')].some((candidate) => candidate.textContent === 'Stop')).toBe(false)
    await act(async () => { useAppStore.setState({ activeSessionId: 'chat-a' }) })
    expect(button('Stop')).toBeTruthy()
    await act(async () => button('Stop').click())
    expect(useAppStore.getState().cancelVcsRequest).toHaveBeenCalledWith(updateVcsWorkspace.mock.calls[0][2])
    await act(async () => pending.resolve({ ok: true, conflicts: false, revision: '7', output: '', error: '' }))
    expect(host.querySelector('[role="status"]')!.textContent).toContain('Updated to r7.')
    expect(updateVcsWorkspace).toHaveBeenCalledTimes(1)
  })
})
