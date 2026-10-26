import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { sendToCEF } from '../../ipc/cefBridge'
import { NewChatBranchPicker } from './NewChatBranchPicker'

vi.mock('../../ipc/cefBridge', () => ({ sendToCEF: vi.fn() }))

const send = vi.mocked(sendToCEF)
const context = {
  vcs: 'git', currentBranch: 'main', currentCommit: 'a'.repeat(40), detached: false,
  localBranches: [{ name: 'main', commit: 'a'.repeat(40) }, { name: 'feature/base', commit: 'b'.repeat(40) }],
}

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('NewChatBranchPicker', () => {
  let host: HTMLDivElement
  let root: ReturnType<typeof createRoot>
  const onChange = vi.fn()
  const render = (props: Partial<Parameters<typeof NewChatBranchPicker>[0]> = {}) => root.render(
    <NewChatBranchPicker folderId="project" defaultBranch="main" value={undefined} onChange={onChange} {...props} />,
  )
  const trigger = () => host.querySelector<HTMLButtonElement>('[aria-label="Starting branch"]')!
  const choose = (text: string) => {
    act(() => trigger().click())
    const option = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((entry) => entry.textContent?.startsWith(text))
    expect(option).toBeDefined()
    act(() => option!.click())
  }

  beforeEach(() => {
    send.mockReset().mockResolvedValue({ ok: true, data: context })
    onChange.mockReset()
    host = document.createElement('div')
    document.body.appendChild(host)
    root = createRoot(host)
  })
  afterEach(() => { act(() => root.unmount()); host.remove() })

  it('reads local refs only and distinguishes inherited, HEAD and named bases without changing a branch', async () => {
    await act(async () => render())
    expect(send).toHaveBeenCalledOnce()
    expect(send).toHaveBeenCalledWith({ action: 'getWorkspaceVcsContext', payload: { folderId: 'project' } })
    expect(onChange).not.toHaveBeenCalled()
    choose('feature/base')
    expect(onChange).toHaveBeenLastCalledWith('feature/base')
    choose('Current branch (main)')
    expect(onChange).toHaveBeenLastCalledWith('')
    choose('Use default (main)')
    expect(onChange).toHaveBeenLastCalledWith(undefined)
    expect(send).toHaveBeenCalledTimes(1)
  })

  it('labels the selector with its explanation and offers Refresh after a successful read', async () => {
    await act(async () => render())
    const describedBy = trigger().getAttribute('aria-describedby')!
    expect(document.getElementById(describedBy)?.textContent).toContain('isolated worktree')
    expect(host.querySelector('label')?.htmlFor).toBe(trigger().id)
    expect(host.querySelector('[role="alert"]')).toBeNull()
    await act(async () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Refresh branches')!.click())
    expect(send).toHaveBeenCalledTimes(2)
  })

  it.each([
    [{ vcs: '', localBranches: [] }, 'no enabled Git repository'],
    [{ vcs: 'svn' }, 'SVN workspaces'],
    [{ ...context, detached: true, currentBranch: '' }, 'attached Git branch'],
    [{ ...context, localBranches: [] }, 'could not be read'],
    [{ ...context, localBranches: [{ name: 'main', commit: 'invalid' }] }, 'could not be read'],
    [{ ...context, currentCommit: 'b'.repeat(40) }, 'could not be read'],
  ])('keeps unsupported or uncertain context display-only: %j', async (data, message) => {
    send.mockResolvedValue({ ok: true, data })
    await act(async () => render())
    expect(trigger().disabled).toBe(true)
    expect(host.textContent).toContain(message)
    expect(onChange).not.toHaveBeenCalled()
  })

  it('shows loading, reports an unborn/read failure and retries without requesting creation', async () => {
    let reply!: (response: { ok: boolean; error: string }) => void
    send.mockImplementationOnce(() => new Promise((resolve) => { reply = resolve }))
    act(() => render())
    expect(trigger().disabled).toBe(true)
    expect(host.textContent).toContain('Loading branches')
    expect(host.querySelector('[role="status"] .animate-spin')).not.toBeNull()
    expect(host.querySelector('[aria-busy="true"]')).not.toBeNull()
    await act(async () => reply({ ok: false, error: 'Could not read local Git branches.' }))
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Could not read local Git branches.')
    await act(async () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Retry branches')!.click())
    expect(trigger().disabled).toBe(false)
    expect(send.mock.calls.every(([request]) => request.action === 'getWorkspaceVcsContext')).toBe(true)
    expect(onChange).not.toHaveBeenCalled()
  })

  it('ignores a late result from the previous workspace', async () => {
    let reply!: (response: { ok: boolean; data: typeof context }) => void
    send.mockImplementationOnce(() => new Promise((resolve) => { reply = resolve }))
    act(() => render())
    await act(async () => render({ folderId: 'other' }))
    await act(async () => reply({ ok: true, data: { ...context, currentBranch: 'obsolete', localBranches: [{ name: 'obsolete', commit: 'a'.repeat(40) }] } }))
    act(() => trigger().click())
    expect(document.body.querySelector('[role="listbox"]')?.textContent).toContain('feature/base')
    expect(document.body.querySelector('[role="listbox"]')?.textContent).not.toContain('obsolete')
  })

  it.each([false, true])('retains Retry focus during loading and after success=%s', async (success) => {
    let reply!: (response: { ok: boolean; error?: string; data?: typeof context }) => void
    send.mockResolvedValueOnce({ ok: false, error: 'Could not read branches.' })
      .mockImplementationOnce(() => new Promise((resolve) => { reply = resolve }))
    await act(async () => render())
    const retry = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Retry branches')!
    retry.focus()
    act(() => retry.click())
    expect(document.activeElement).toBe(retry)
    expect(retry.isConnected).toBe(true)
    expect(retry.getAttribute('aria-disabled')).toBe('true')
    expect(trigger().disabled).toBe(true)
    act(() => retry.click())
    expect(send).toHaveBeenCalledTimes(2)

    await act(async () => reply(success ? { ok: true, data: context } : { ok: false, error: 'Still unavailable.' }))
    expect(document.activeElement).toBe(retry)
    expect(retry.textContent).toBe(success ? 'Refresh branches' : 'Retry branches')
    expect(trigger().disabled).toBe(!success)
    await act(async () => retry.click())
    expect(send).toHaveBeenCalledTimes(3)
    expect(document.activeElement).toBe(retry)
    expect(onChange).not.toHaveBeenCalled()
    expect(send.mock.calls.every(([request]) => request.action === 'getWorkspaceVcsContext')).toBe(true)
  })

  it('does not steal focus when the user leaves Retry while discovery is pending', async () => {
    let reply!: (response: { ok: boolean; data: typeof context }) => void
    send.mockResolvedValueOnce({ ok: false, error: 'Could not read branches.' })
      .mockImplementationOnce(() => new Promise((resolve) => { reply = resolve }))
    await act(async () => render())
    const retry = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Retry branches')!
    retry.focus()
    act(() => retry.click())
    const other = document.createElement('button')
    host.appendChild(other)
    other.focus()
    await act(async () => reply({ ok: true, data: context }))
    expect(document.activeElement).toBe(other)
    expect(trigger().disabled).toBe(false)
  })

  it('ignores a stale retry without moving focus in the replacement workspace', async () => {
    let reply!: (response: { ok: boolean; error: string }) => void
    send.mockResolvedValueOnce({ ok: false, error: 'Could not read branches.' })
      .mockImplementationOnce(() => new Promise((resolve) => { reply = resolve }))
    await act(async () => render())
    const retry = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Retry branches')!
    retry.focus()
    act(() => retry.click())
    await act(async () => render({ folderId: 'other' }))
    trigger().focus()
    await act(async () => reply({ ok: false, error: 'Obsolete workspace failure.' }))
    expect(document.activeElement).toBe(trigger())
    expect(host.querySelector('[role="alert"]')).toBeNull()
    expect(send).toHaveBeenLastCalledWith({ action: 'getWorkspaceVcsContext', payload: { folderId: 'other' } })
    expect(onChange).not.toHaveBeenCalled()
  })

  it('does not probe when the selected execution context cannot choose a base', async () => {
    await act(async () => render({ unavailableReason: 'Starting branch selection is available for local workspaces.' }))
    expect(trigger().disabled).toBe(true)
    expect(send).not.toHaveBeenCalled()
  })

  it('preserves the complete long local ref when selecting a wrapped branch label', async () => {
    const branch = 'feature/' + 'x'.repeat(180)
    send.mockResolvedValue({ ok: true, data: { ...context, localBranches: [...context.localBranches, { name: branch, commit: 'c'.repeat(40) }] } })
    await act(async () => render())
    choose(branch)
    expect(onChange).toHaveBeenLastCalledWith(branch)
    expect(send).toHaveBeenCalledOnce()
  })
})
