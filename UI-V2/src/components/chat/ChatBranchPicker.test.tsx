import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, expect, it, vi } from 'vitest'
import { ChatBranchPicker } from './ChatBranchPicker'
import { sendToCEF } from '../../ipc/cefBridge'
import { useAppStore } from '../../store/useAppStore'
import type { Session } from '../../types/session'

vi.mock('../../ipc/cefBridge', async (importOriginal) => ({
  ...await importOriginal<typeof import('../../ipc/cefBridge')>(),
  sendToCEF: vi.fn(),
  createRequestId: () => 'rid',
}))

const send = vi.mocked(sendToCEF)

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const flush = () => act(async () => { await new Promise((resolve) => setTimeout(resolve, 0)) })

function render(props: Partial<Parameters<typeof ChatBranchPicker>[0]> = {}) {
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  const onSwitched = vi.fn()
  act(() => root.render(
    <ChatBranchPicker chatId="chat-1" branch="main" disabled={false} disabledReason="Busy" onSwitched={onSwitched} {...props} />
  ))
  return { host, root, onSwitched, cleanup: () => { act(() => root.unmount()); host.remove() } }
}

const trigger = (host: HTMLElement) => host.querySelector<HTMLButtonElement>('button[aria-haspopup="dialog"]')!
const options = () => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]'))

async function open(host: HTMLElement) {
  await act(async () => { trigger(host).click() })
  await flush()
}

beforeEach(() => {
  useAppStore.setState({ sessions: [] })
  send.mockReset()
  send.mockImplementation(async (request) => {
    if (request.action === 'listChatBranches') {
      return { ok: true, data: { vcsType: 'git', branches: [{ name: 'main', current: true }, { name: 'feature/tabs', current: false }, { name: 'origin/release', current: false, remote: true }] } }
    }
    return { ok: true, data: { branch: (request.payload as { branch: string }).branch } }
  })
})

it('lists local and remote branches with the current one checked and filters by search', async () => {
  const { host, cleanup } = render()
  await open(host)
  expect(document.getElementById(trigger(host).getAttribute('aria-controls')!)?.getAttribute('role')).toBe('dialog')
  expect(send).toHaveBeenCalledWith(expect.objectContaining({ action: 'listChatBranches', payload: { chatId: 'chat-1' } }))
  expect(options().map((option) => option.textContent)).toEqual(expect.arrayContaining([expect.stringContaining('feature/tabs'), expect.stringContaining('remote')]))
  expect(options()[0].getAttribute('aria-checked')).toBe('true')
  const search = document.body.querySelector<HTMLInputElement>('input[aria-label="Search branches"]')!
  await act(async () => {
    const setValue = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!
    setValue.call(search, 'feat')
    search.dispatchEvent(new Event('input', { bubbles: true }))
  })
  expect(options().map((option) => option.textContent)).toEqual([expect.stringContaining('feature/tabs')])
  cleanup()
})

it('keeps native Git order with other chats\' worktree branches last', async () => {
  const chat = (id: string, folderId: string, fields: Partial<Session>) => ({ id, folderId, createdAt: new Date(0), updatedAt: new Date(0), ...fields }) as Session
  useAppStore.setState({ sessions: [
    chat('chat-1', 'project', { workspaceBranchName: 'main' }),
    chat('chat-2', 'project', { workspaceBranchName: 'feature/tabs' }),
    chat('chat-3', 'elsewhere', { workspaceBranchName: 'origin/release' }),
  ] })
  const { host, cleanup } = render()
  await open(host)
  expect(options().map((option) => option.textContent?.replace('remote', ''))).toEqual(['main', 'origin/release', 'feature/tabs'])
  cleanup()
})

it('switches to the chosen branch and reports the branch the backend returned', async () => {
  const { host, onSwitched, cleanup } = render()
  await open(host)
  await act(async () => { options()[1].click() })
  await flush()
  expect(send).toHaveBeenCalledWith(expect.objectContaining({ action: 'switchChatBranch', payload: { chatId: 'chat-1', branch: 'feature/tabs' } }))
  expect(onSwitched).toHaveBeenCalledWith('feature/tabs')
  expect(document.body.querySelector('[role="dialog"][aria-label="Switch branch"]')).toBeNull()
  cleanup()
})

it('creates a branch from the current one through the create row', async () => {
  const { host, onSwitched, cleanup } = render()
  await open(host)
  await act(async () => {
    Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent?.includes('Create branch from current'))!.click()
  })
  const input = document.body.querySelector<HTMLInputElement>('input[aria-label="New branch name"]')!
  await act(async () => {
    const setValue = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!
    setValue.call(input, 'feature/new')
    input.dispatchEvent(new Event('input', { bubbles: true }))
  })
  await act(async () => { input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })) })
  await flush()
  expect(send).toHaveBeenCalledWith(expect.objectContaining({ action: 'switchChatBranch', payload: { chatId: 'chat-1', branch: 'feature/new', create: true } }))
  expect(onSwitched).toHaveBeenCalledWith('feature/new')
  cleanup()
})

it('reports a confirmed branch switch even if controls become disabled while the reply is pending', async () => {
  let finish: ((response: { ok: boolean; data: { branch: string } }) => void) | undefined
  const { host, root, onSwitched, cleanup } = render()
  try {
    await open(host)
    send.mockImplementationOnce(() => new Promise((resolve) => { finish = resolve }))
    act(() => options()[1].click())
    act(() => root.render(<ChatBranchPicker chatId="chat-1" branch="main" disabled disabledReason="Busy" onSwitched={onSwitched} />))
    await act(async () => finish!({ ok: true, data: { branch: 'feature/tabs' } }))
    expect(onSwitched).toHaveBeenCalledWith('feature/tabs')
  } finally { cleanup() }
})

it('shows a one-line error inline and keeps the picker open when the switch fails', async () => {
  send.mockImplementation(async (request) => request.action === 'listChatBranches'
    ? { ok: true, data: { vcsType: 'git', branches: [{ name: 'main', current: true }, { name: 'feature/tabs', current: false }] } }
    : { ok: false, error: 'Commit or discard 2 changed files before switching branches.' })
  const { host, onSwitched, cleanup } = render()
  await open(host)
  await act(async () => { options()[1].click() })
  await flush()
  const alert = document.body.querySelector('[role="dialog"][aria-label="Switch branch"] [role="alert"]')
  expect(alert?.textContent).toBe('Commit or discard 2 changed files before switching branches.')
  expect(onSwitched).not.toHaveBeenCalled()
  cleanup()
})

const svnContext = { directory: '/tmp/project', url: 'file:///repository/lines/main', repository: 'file:///repository', uuid: 'same-repository', revision: '42', parent: '^/lines', repositoryRevision: '45' }
const svnBranches = [{ name: 'main', path: '^/lines/main', current: true }, { name: 'feature a', path: '^/lines/feature a', current: false }]

async function typeParent(value = '^/lines') {
  const input = document.body.querySelector<HTMLInputElement>('input[aria-label="SVN repository folder"]')!
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, value)
    input.dispatchEvent(new Event('input', { bubbles: true }))
  })
}

const buttonNamed = (label: string) => Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === label)!

function svnResponses() {
  send.mockImplementation(async (request) => {
    const payload = request.payload as { svnParent?: string }
    if (request.action === 'listChatBranches') return { ok: true, data: { vcsType: 'svn', branches: payload.svnParent ? svnBranches : [], svnContext: { ...svnContext, parent: payload.svnParent || '' } } }
    return { ok: true, data: { vcsType: 'svn', branch: '45' } }
  })
}

it('requires an explicit SVN repository folder without guessing a layout', async () => {
  svnResponses()
  const { host, cleanup } = render({ vcsType: 'svn', branch: '42' })
  try {
    await open(host)
    expect(send).toHaveBeenCalledWith(expect.objectContaining({ action: 'listChatBranches', payload: { chatId: 'chat-1', vcsType: 'svn', svnParent: '' } }))
    expect(buttonNamed('Discover folders').disabled).toBe(true)
    expect(options()).toHaveLength(0)
    expect(document.body.textContent).not.toContain('Create branch from current')
    expect(document.activeElement?.getAttribute('aria-label')).toBe('SVN repository folder')
  } finally { cleanup() }
})

it('shows the repository-root SVN path without switching', async () => {
  send.mockResolvedValue({ ok: true, data: { vcsType: 'svn', branches: [], svnContext: { ...svnContext, url: svnContext.repository, revision: '43', parent: '' } } })
  const { host, onSwitched, cleanup } = render({ vcsType: 'svn', branch: '42' })
  try {
    await flush()
    expect(trigger(host).textContent).toBe('^/ r43')
    expect(send.mock.calls.every(([request]) => request.action === 'listChatBranches')).toBe(true)
    expect(onSwitched).not.toHaveBeenCalled()
  } finally { cleanup() }
})

it('shows the verified current SVN path and revision without switching', async () => {
  send.mockResolvedValue({ ok: true, data: { vcsType: 'svn', branches: [], svnContext: { ...svnContext, revision: '43', parent: '' } } })
  const { host, onSwitched, cleanup } = render({ vcsType: 'svn', branch: '42' })
  try {
    await flush()
    expect(trigger(host).textContent).toBe('^/lines/main r43')
    expect(trigger(host).getAttribute('aria-label')).toBe('Switch SVN folder, current ^/lines/main revision 43')
    expect(send.mock.calls.every(([request]) => request.action === 'listChatBranches')).toBe(true)
    expect(onSwitched).not.toHaveBeenCalled()
  } finally { cleanup() }
})

it('discovers SVN folders and switches only after explicit confirmation with the captured context', async () => {
  svnResponses()
  const { host, onSwitched, cleanup } = render({ vcsType: 'svn', branch: '42' })
  try {
    await open(host)
    await typeParent()
    await act(async () => buttonNamed('Discover folders').click())
    expect(options().map((option) => option.textContent)).toEqual(['main', 'feature a'])
    await act(async () => options()[1].click())
    expect(send.mock.calls.filter(([request]) => request.action === 'switchChatBranch')).toHaveLength(0)
    await act(async () => { buttonNamed('Switch folder').click(); buttonNamed('Switch folder').click() })
    expect(send.mock.calls.filter(([request]) => request.action === 'switchChatBranch')).toHaveLength(1)
    expect(send).toHaveBeenCalledWith(expect.objectContaining({ action: 'switchChatBranch', payload: { chatId: 'chat-1', vcsType: 'svn', branch: '^/lines/feature a', svnContext } }))
    expect(onSwitched).toHaveBeenCalledWith('45', 'svn')
  } finally { cleanup() }
})

it('clears SVN choices and confirmation when the explicit repository mapping changes', async () => {
  svnResponses()
  const { host, cleanup } = render({ vcsType: 'svn', branch: '42' })
  try {
    await open(host); await typeParent()
    await act(async () => buttonNamed('Discover folders').click())
    await act(async () => options()[1].click())
    await typeParent('^/different')
    expect(options()).toHaveLength(0)
    expect(buttonNamed('Switch folder')).toBeUndefined()
    expect(send.mock.calls.filter(([request]) => request.action === 'switchChatBranch')).toHaveLength(0)
  } finally { cleanup() }
})

it('retains the editable SVN mapping and allows discovery retry after an error', async () => {
  svnResponses()
  const { host, cleanup } = render({ vcsType: 'svn', branch: '42' })
  try {
    await open(host); await typeParent()
    send.mockResolvedValueOnce({ ok: false, error: 'The repository folder is unavailable.' })
    await act(async () => buttonNamed('Discover folders').click())
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain('unavailable')
    await typeParent('^/lines')
    await act(async () => buttonNamed('Discover folders').click())
    expect(options()).toHaveLength(2)
  } finally { cleanup() }
})

it('keeps SVN switch refusal visible without changing the composer revision', async () => {
  svnResponses()
  const { host, onSwitched, cleanup } = render({ vcsType: 'svn', branch: '42' })
  try {
    await open(host); await typeParent()
    await act(async () => buttonNamed('Discover folders').click())
    await act(async () => options()[1].click())
    send.mockResolvedValueOnce({ ok: false, error: 'The working copy changed. Discover the folders again.' })
    await act(async () => buttonNamed('Switch folder').click())
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain('working copy changed')
    expect(onSwitched).not.toHaveBeenCalled()
    expect(trigger(host).textContent).toBe('^/lines/main r42')
  } finally { cleanup() }
})

it('keeps an SVN inspection error visible after disabled controls close the pending picker', async () => {
  svnResponses()
  let finish: ((response: { ok: false; error: string }) => void) | undefined
  const { host, root, onSwitched, cleanup } = render({ vcsType: 'svn', branch: '42' })
  try {
    await open(host); await typeParent()
    await act(async () => buttonNamed('Discover folders').click())
    await act(async () => options()[1].click())
    send.mockImplementationOnce(() => new Promise((resolve) => { finish = resolve }))
    act(() => buttonNamed('Switch folder').click())
    act(() => root.render(<ChatBranchPicker chatId="chat-1" branch="42" vcsType="svn" disabled disabledReason="Busy" onSwitched={onSwitched} />))
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
    const error = 'SVN did not confirm switching. Inspect the working copy before retrying.'
    await act(async () => finish!({ ok: false, error }))
    const alert = host.querySelector<HTMLElement>('[role="alert"]')
    expect(alert?.textContent).toContain(error)
    expect(alert?.tabIndex).toBe(0)
    expect(trigger(host).textContent).toBe('^/lines/main r42')
    expect(onSwitched).not.toHaveBeenCalled()
  } finally { cleanup() }
})

it('keeps a Git snapshot label when its SVN source has no Git selection route', async () => {
  send.mockResolvedValue({ ok: true, data: { vcsType: 'svn', branches: [] } })
  const { host, onSwitched, cleanup } = render()
  try {
    await open(host)
    expect(document.body.querySelector('[role="alert"]')?.textContent).toContain('Git snapshot cannot switch its SVN source')
    expect(trigger(host).textContent).toBe('main')
    expect(document.body.querySelector('input[aria-label="SVN repository folder"]')).toBeNull()
    expect(onSwitched).not.toHaveBeenCalled()
  } finally { cleanup() }
})

it('does not open while the agent is working', async () => {
  const { host, cleanup } = render({ disabled: true })
  expect(trigger(host).disabled).toBe(true)
  await act(async () => { trigger(host).click() })
  expect(send).not.toHaveBeenCalled()
  cleanup()
})

it('focuses SVN confirmation after combobox keyboard selection', async () => {
  svnResponses()
  const { host, onSwitched, cleanup } = render({ vcsType: 'svn', branch: '42' })
  try {
    await open(host); await typeParent()
    await act(async () => buttonNamed('Discover folders').click())
    const search = document.body.querySelector<HTMLInputElement>('input[aria-label="Search SVN folders"]')!
    search.focus()
    await act(async () => {
      search.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true }))
    })
    await act(async () => search.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
    expect(buttonNamed('Switch folder')).toBeTruthy()
    expect(document.activeElement).toBe(buttonNamed('Switch folder'))
    expect(onSwitched).not.toHaveBeenCalled()
    expect(send.mock.calls.every(([request]) => request.action === 'listChatBranches')).toBe(true)
  } finally { cleanup() }
})

it('keeps SVN combobox options out of the tab sequence', async () => {
  svnResponses()
  const { host, cleanup } = render({ vcsType: 'svn', branch: '42' })
  try {
    await open(host); await typeParent()
    await act(async () => buttonNamed('Discover folders').click())
    expect(options()).toHaveLength(2)
    expect(options().every((option) => option.tabIndex === -1)).toBe(true)
  } finally { cleanup() }
})

it('restores the branch trigger after a focused SVN confirmation succeeds', async () => {
  svnResponses()
  const { host, onSwitched, cleanup } = render({ vcsType: 'svn', branch: '42' })
  try {
    await open(host); await typeParent()
    await act(async () => buttonNamed('Discover folders').click())
    await act(async () => options()[1].click())
    buttonNamed('Switch folder').focus()
    await act(async () => buttonNamed('Switch folder').click())
    await flush()
    expect(onSwitched).toHaveBeenCalledWith('45', 'svn')
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
    expect(document.activeElement).toBe(trigger(host))
  } finally { cleanup() }
})

it('returns to SVN search when confirmation is cancelled and to the trigger on Escape', async () => {
  svnResponses()
  const { host, cleanup } = render({ vcsType: 'svn', branch: '42' })
  try {
    await open(host); await typeParent()
    await act(async () => buttonNamed('Discover folders').click())
    await act(async () => options()[1].click())
    await act(async () => buttonNamed('Cancel').click())
    const search = document.body.querySelector<HTMLInputElement>('input[aria-label="Search SVN folders"]')!
    expect(document.activeElement).toBe(search)
    await act(async () => search.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
    expect(document.activeElement).toBe(trigger(host))
    expect(send.mock.calls.every(([request]) => request.action === 'listChatBranches')).toBe(true)
  } finally { cleanup() }
})

it('backs out of SVN confirmation on Escape without closing the picker', async () => {
  svnResponses()
  const { host, cleanup } = render({ vcsType: 'svn', branch: '42' })
  try {
    await open(host); await typeParent()
    await act(async () => buttonNamed('Discover folders').click())
    await act(async () => options()[1].click())
    await act(async () => buttonNamed('Switch folder').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.querySelector('[role="dialog"]')).toBeTruthy()
    expect(document.body.querySelector('button[aria-label], button') && [...document.body.querySelectorAll('button')].some((b) => b.textContent === 'Switch folder')).toBe(false)
    expect(document.activeElement).toBe(document.body.querySelector('input[aria-label="Search SVN folders"]'))
    expect(options()).toHaveLength(2)
  } finally { cleanup() }
})

it('clears a stale SVN confirmation when the search changes', async () => {
  svnResponses()
  const { host, cleanup } = render({ vcsType: 'svn', branch: '42' })
  try {
    await open(host); await typeParent()
    await act(async () => buttonNamed('Discover folders').click())
    await act(async () => options()[1].click())
    const search = document.body.querySelector<HTMLInputElement>('input[aria-label="Search SVN folders"]')!
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(search, 'x')
      search.dispatchEvent(new Event('input', { bubbles: true }))
    })
    expect([...document.body.querySelectorAll('button')].some((b) => b.textContent === 'Switch folder')).toBe(false)
  } finally { cleanup() }
})

it('closes the picker when keyboard focus leaves it', async () => {
  svnResponses()
  const { host, cleanup } = render({ vcsType: 'svn', branch: '42' })
  const outside = document.createElement('button')
  document.body.appendChild(outside)
  try {
    await open(host)
    const dialog = document.body.querySelector<HTMLElement>('[role="dialog"]')!
    await act(async () => {
      dialog.querySelector<HTMLElement>('input')!.dispatchEvent(new FocusEvent('focusout', { bubbles: true, relatedTarget: outside }))
    })
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
    await open(host)
    await act(async () => {
      trigger(host).dispatchEvent(new FocusEvent('focusout', { bubbles: true, relatedTarget: outside }))
    })
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()
  } finally { outside.remove(); cleanup() }
})

it('preserves focus moved outside while a confirmed SVN switch is pending', async () => {
  svnResponses()
  let finish: ((response: { ok: true; data: { branch: string; vcsType: 'svn' } }) => void) | undefined
  const { host, onSwitched, cleanup } = render({ vcsType: 'svn', branch: '42' })
  const outside = document.createElement('button')
  document.body.append(outside)
  try {
    await open(host); await typeParent()
    await act(async () => buttonNamed('Discover folders').click())
    await act(async () => options()[1].click())
    send.mockImplementationOnce(() => new Promise((resolve) => { finish = resolve }))
    act(() => buttonNamed('Switch folder').click())
    outside.focus()
    await act(async () => finish!({ ok: true, data: { branch: '45', vcsType: 'svn' } }))
    expect(onSwitched).toHaveBeenCalledWith('45', 'svn')
    expect(document.activeElement).toBe(outside)
  } finally { outside.remove(); cleanup() }
})

it('preserves external focus after closing a pending SVN switch that later refuses', async () => {
  svnResponses()
  let finish: ((response: { ok: false; error: string }) => void) | undefined
  const { host, onSwitched, cleanup } = render({ vcsType: 'svn', branch: '42' })
  const outside = document.createElement('button')
  document.body.append(outside)
  try {
    await open(host); await typeParent()
    await act(async () => buttonNamed('Discover folders').click())
    await act(async () => options()[1].click())
    send.mockImplementationOnce(() => new Promise((resolve) => { finish = resolve }))
    act(() => buttonNamed('Switch folder').click())
    const search = document.body.querySelector<HTMLInputElement>('input[aria-label="Search SVN folders"]')!
    search.focus()
    await act(async () => search.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    outside.focus()
    await act(async () => finish!({ ok: false, error: 'Inspect the working copy before retrying.' }))
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Inspect the working copy')
    expect(onSwitched).not.toHaveBeenCalled()
    expect(document.activeElement).toBe(outside)
  } finally { outside.remove(); cleanup() }
})

it('restores the trigger after an escaped pending refusal when focus was not moved elsewhere', async () => {
  svnResponses()
  let finish: ((response: { ok: false; error: string }) => void) | undefined
  const { host, onSwitched, cleanup } = render({ vcsType: 'svn', branch: '42' })
  try {
    await open(host); await typeParent()
    await act(async () => buttonNamed('Discover folders').click())
    await act(async () => options()[1].click())
    send.mockImplementationOnce(() => new Promise((resolve) => { finish = resolve }))
    act(() => buttonNamed('Switch folder').click())
    const search = document.body.querySelector<HTMLInputElement>('input[aria-label="Search SVN folders"]')!
    search.focus()
    await act(async () => search.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    await act(async () => finish!({ ok: false, error: 'Inspect the working copy before retrying.' }))
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Inspect the working copy')
    expect(document.activeElement).toBe(trigger(host))
    expect(onSwitched).not.toHaveBeenCalled()
  } finally { cleanup() }
})

it('accepts a pending first-message branch without reporting a switch', async () => {
  send.mockImplementation(async (request) => request.action === 'listChatBranches'
    ? { ok: true, data: { vcsType: 'git', branches: [{ name: 'main', current: true }] } }
    : { ok: true, data: { branch: 'empty/new', pending: true } })
  const { host, onSwitched, cleanup } = render()
  await open(host)
  await act(async () => {
    Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent?.includes('Create branch from current'))!.click()
  })
  const input = document.body.querySelector<HTMLInputElement>('input[aria-label="New branch name"]')!
  await act(async () => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, 'empty/new')
    input.dispatchEvent(new Event('input', { bubbles: true }))
  })
  await act(async () => { input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })) })
  await flush()
  expect(send).toHaveBeenCalledWith(expect.objectContaining({ action: 'switchChatBranch', payload: { chatId: 'chat-1', branch: 'empty/new', create: true } }))
  expect(onSwitched).not.toHaveBeenCalled()
  expect(document.body.querySelector('[role="dialog"]')).toBeNull()
  cleanup()
})

it('shows a pending branch and withdraws it when the checked-out branch is chosen', async () => {
  const { host, onSwitched, cleanup } = render({ pendingBranch: 'empty/new' })
  expect(trigger(host).textContent).toBe('empty/new')
  expect(trigger(host).getAttribute('aria-label')).toBe('Switch branch, empty/new pending, created from main on first message')
  await open(host)
  const dialog = document.body.querySelector('[role="dialog"]')!
  expect(document.getElementById(dialog.getAttribute('aria-describedby')!)?.textContent).toBe('empty/new will be created from main when you send your first message. Select main above to cancel.')
  await act(async () => { options()[0].click() })
  await flush()
  expect(send).toHaveBeenCalledWith(expect.objectContaining({ action: 'switchChatBranch', payload: { chatId: 'chat-1', branch: 'main' } }))
  expect(onSwitched).toHaveBeenCalledWith('main')
  cleanup()
})
