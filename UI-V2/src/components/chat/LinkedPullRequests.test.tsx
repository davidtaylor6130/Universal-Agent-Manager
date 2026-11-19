import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { LinkedPullRequests, WatchPullRequestDialog, pullRequestChecksSummary } from './LinkedPullRequests'
import { useAppStore } from '../../store/useAppStore'
import type { PullRequestWatch } from '../../store/cpp/types'
import { sanitizePullRequestWatches } from '../../store/cpp/sanitizers'
import { APP_FAILURE_EVENT, type AppFailure } from '../../utils/appFailure'
import { reportPullRequestWatchNotices, resetPullRequestWatchNoticesForTests } from '../../utils/prWatchNotices'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const sendToCEF = vi.hoisted(() => vi.fn())
vi.mock('../../ipc/cefBridge', async (original) => ({ ...(await original<typeof import('../../ipc/cefBridge')>()), sendToCEF }))

const watch: PullRequestWatch = {
  chatId: 'chat-1', repository: 'acme/app', number: 12, url: 'https://github.com/acme/app/pull/12', title: 'Add the parser', state: 'open', watching: true,
  checksPassed: 3, checksFailed: 1, checksPending: 2, conflicting: false, readFailing: false, paused: false,
}
let host: HTMLDivElement, root: Root, previous: ReturnType<typeof useAppStore.getState>

beforeEach(() => {
  previous = useAppStore.getState()
  sendToCEF.mockReset().mockResolvedValue({ ok: true })
  resetPullRequestWatchNoticesForTests()
  host = document.createElement('div'); document.body.append(host); root = createRoot(host)
})

afterEach(() => {
  act(() => root.unmount()); host.remove()
  useAppStore.setState(previous, true)
  vi.restoreAllMocks()
})

const button = (label: string) => document.querySelector<HTMLButtonElement>(`button[aria-label="${label}"]`)
const show = async (watches: PullRequestWatch[]) => {
  await act(async () => {
    useAppStore.setState({ prWatches: watches })
    root.render(<LinkedPullRequests chatId="chat-1" />)
  })
}

describe('linked pull request rows', () => {
  it('shows nothing for a chat without linked pull requests', async () => {
    await show([{ ...watch, chatId: 'another-chat' }])
    expect(host.querySelector('nav')).toBeNull()
  })

  it('shows state, title and a checks summary that does not rely on colour', async () => {
    await show([watch, { ...watch, chatId: 'another-chat', number: 99 }])
    const rows = host.querySelectorAll('nav[aria-label="Linked pull requests"] li')
    expect(rows).toHaveLength(1)
    expect(rows[0].textContent).toContain('Open')
    expect(rows[0].textContent).toContain('acme/app #12')
    expect(rows[0].textContent).toContain('Add the parser')
    expect(rows[0].querySelector('[role="img"]')?.getAttribute('aria-label')).toBe('1 check failed, 2 checks running, 3 checks passed')
    expect(pullRequestChecksSummary({ checksPassed: 0, checksFailed: 0, checksPending: 0 })).toBe('No checks reported')
  })

  it('stops watching from the eye and keeps the row, then watches again', async () => {
    await show([watch])
    // One stable name; the pressed state says whether it is watching.
    const eye = button('Watch acme/app #12')!
    expect(eye.getAttribute('aria-pressed')).toBe('true')
    await act(async () => eye.click())
    expect(sendToCEF).toHaveBeenCalledWith({ action: 'managePullRequestWatch', payload: { chatId: 'chat-1', operation: 'unwatch', pullRequest: 'acme/app', number: 12 } })
    await show([{ ...watch, watching: false }])
    const resume = button('Watch acme/app #12')!
    expect(resume.getAttribute('aria-pressed')).toBeNull()
    await act(async () => resume.click())
    expect(sendToCEF).toHaveBeenLastCalledWith({ action: 'managePullRequestWatch', payload: { chatId: 'chat-1', operation: 'watch', pullRequest: 'acme/app', number: 12 } })
  })

  it('unlinks a pull request and reports a refusal through the shared notice path', async () => {
    await show([watch])
    const failures: AppFailure[] = []
    const listen = (event: Event) => failures.push((event as CustomEvent<AppFailure>).detail)
    window.addEventListener(APP_FAILURE_EVENT, listen)
    sendToCEF.mockResolvedValueOnce({ ok: false, error: 'The chat is unavailable.' })
    await act(async () => button('Unlink acme/app #12')!.click())
    window.removeEventListener(APP_FAILURE_EVENT, listen)
    expect(sendToCEF).toHaveBeenCalledWith({ action: 'managePullRequestWatch', payload: { chatId: 'chat-1', operation: 'unlink', pullRequest: 'acme/app', number: 12 } })
    expect(failures.map((failure) => failure.message)).toEqual(['Could not unlink acme/app #12. The chat is unavailable.'])
  })

  it('explains why a merged pull request cannot be watched, and names pauses and read failures', async () => {
    await show([{ ...watch, state: 'merged', watching: false }])
    const eye = button('Watch acme/app #12')!
    expect(eye.getAttribute('aria-disabled')).toBe('true')
    await act(async () => eye.click())
    expect(sendToCEF).not.toHaveBeenCalled()
    expect(host.textContent).toContain('Merged')
    await show([{ ...watch, paused: true, readFailing: true, conflicting: true }])
    expect(host.textContent).toContain("Paused by GitHub's rate limit")
    expect(host.textContent).toContain('Conflicts')
    await show([{ ...watch, readFailing: true }])
    expect(host.textContent).toContain('GitHub could not be read')
  })
})

describe('watch pull request dialog', () => {
  const input = () => document.querySelector<HTMLInputElement>('[role="alertdialog"] input')!
  const confirm = () => [...document.querySelectorAll<HTMLButtonElement>('[role="alertdialog"] button')].find((candidate) => candidate.textContent === 'Watch')!
  const type = async (value: string) => act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input(), value)
    input().dispatchEvent(new Event('input', { bubbles: true }))
  })

  it('focuses a labelled field and sends the link', async () => {
    const onClose = vi.fn()
    await act(async () => root.render(<WatchPullRequestDialog chatId="chat-1" open onClose={onClose} />))
    expect(document.activeElement).toBe(input())
    expect(document.querySelector(`label[for="${input().id}"]`)?.textContent).toBe('Pull request link or owner/name#number')
    expect(confirm().disabled).toBe(true)
    await type(' https://github.com/acme/app/pull/12 ')
    await act(async () => confirm().click())
    expect(sendToCEF).toHaveBeenCalledWith({ action: 'managePullRequestWatch', payload: { chatId: 'chat-1', operation: 'watch', pullRequest: 'https://github.com/acme/app/pull/12' } })
    expect(onClose).toHaveBeenCalledTimes(1)
  })

  it('keeps the dialog open with the reason when the link is refused', async () => {
    const onClose = vi.fn()
    sendToCEF.mockResolvedValue({ ok: false, error: 'Enter a github.com pull request link, or owner/name#number.' })
    await act(async () => root.render(<WatchPullRequestDialog chatId="chat-1" open onClose={onClose} />))
    await type('not a link')
    await act(async () => input().dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
    expect(document.querySelector('[role="alertdialog"] [role="alert"]')?.textContent).toBe('Enter a github.com pull request link, or owner/name#number.')
    expect(onClose).not.toHaveBeenCalled()
  })
})

describe('pull request watch state from the native app', () => {
  it('drops malformed rows and rebuilds the link so it can only point at github.com', () => {
    const rows = sanitizePullRequestWatches([
      { chatId: 'chat-1', repository: 'acme/app', number: 12, url: 'https://evil.example/x', title: 'Add the parser', state: 'open', watching: true, checksFailed: 1,
        notice: { serial: 2, message: 'acme/app #12: checks failed', tone: 'warning' } },
      { chatId: 'chat-1', repository: 'acme/app/../x', number: 1 }, { chatId: '', repository: 'acme/app', number: 1 }, { chatId: 'chat-1', repository: 'acme/app', number: 0 }, 'junk',
      { chatId: 'chat-2', repository: 'acme/app', number: 3, state: 'reopened', notice: { serial: 0, message: 'x' } },
    ])
    expect(rows).toHaveLength(2)
    expect(rows[0]).toMatchObject({ url: 'https://github.com/acme/app/pull/12', state: 'open', watching: true, checksFailed: 1, checksPassed: 0, notice: { serial: 2, tone: 'warning' } })
    expect(rows[1]).toMatchObject({ state: 'unknown', watching: false })
    expect(rows[1].notice).toBeUndefined()
    expect(sanitizePullRequestWatches(undefined)).toEqual([])
  })

  it('reports each notice once in the notifications panel, with the chat named', () => {
    const notices: AppFailure[] = []
    const listen = (event: Event) => notices.push((event as CustomEvent<AppFailure>).detail)
    window.addEventListener(APP_FAILURE_EVENT, listen)
    const name = (chatId: string) => chatId === 'chat-1' ? 'Parser work' : undefined
    const failed = { ...watch, notice: { serial: 1, message: 'acme/app #12: checks failed', tone: 'warning' as const } }
    reportPullRequestWatchNotices([watch, failed], name)
    reportPullRequestWatchNotices([failed], name)
    reportPullRequestWatchNotices([{ ...watch, notice: { serial: 2, message: 'acme/app #12: checks passed', tone: 'success' } }], name)
    reportPullRequestWatchNotices([{ ...watch, chatId: 'gone', notice: { serial: 1, message: 'acme/app #12 was merged. UAM stopped watching it.', tone: 'info' } }], name)
    window.removeEventListener(APP_FAILURE_EVENT, listen)
    expect(notices.map(({ message, tone }) => ({ message, tone }))).toEqual([
      { message: 'acme/app #12: checks failed (chat “Parser work”)', tone: undefined },
      { message: 'acme/app #12: checks passed (chat “Parser work”)', tone: 'success' },
      { message: 'acme/app #12 was merged. UAM stopped watching it.', tone: 'info' },
    ])
  })
})
