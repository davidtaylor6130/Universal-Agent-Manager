import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, expect, it, vi } from 'vitest'
import { ChatBranchPicker } from './ChatBranchPicker'
import { sendToCEF } from '../../ipc/cefBridge'

vi.mock('../../ipc/cefBridge', () => ({
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

const trigger = (host: HTMLElement) => host.querySelector<HTMLButtonElement>('button[aria-haspopup="listbox"]')!
const options = () => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]'))

async function open(host: HTMLElement) {
  await act(async () => { trigger(host).click() })
  await flush()
}

beforeEach(() => {
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

it('explains that SVN cannot switch branches', async () => {
  send.mockResolvedValue({ ok: true, data: { vcsType: 'svn', branches: [] } })
  const { host, cleanup } = render()
  await open(host)
  expect(document.body.textContent).toContain("Branch switching isn't available for SVN")
  expect(options()).toHaveLength(0)
  cleanup()
})

it('does not open while the agent is working', async () => {
  const { host, cleanup } = render({ disabled: true })
  expect(trigger(host).disabled).toBe(true)
  await act(async () => { trigger(host).click() })
  expect(send).not.toHaveBeenCalled()
  cleanup()
})
