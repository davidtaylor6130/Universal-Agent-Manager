import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import type { ExecutionHost, RemoteDirectoryBrowseResult } from '../../types/session'
import { useAppStore } from '../../store/useAppStore'
import { RemoteDirectoryBrowser } from './RemoteDirectoryBrowser'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const remoteHost: ExecutionHost = { id: 'lab', label: 'Home lab', transport: 'ssh', sshAlias: 'lab', runnerStatus: 'ready', runnerVersion: '1', platform: 'linux', architecture: 'x64', lastSeenAt: '' }
const listing = (directory: string): RemoteDirectoryBrowseResult => ({ ok: true, listing: { directory, parentDirectory: '/', directories: [], truncated: false } })
const originalList = useAppStore.getState().listRemoteDirectories
let element: HTMLDivElement
let root: ReturnType<typeof createRoot>
const button = (label: string) => Array.from(element.querySelectorAll<HTMLButtonElement>('button')).find((item) => item.textContent === label || item.getAttribute('aria-label') === label)!
const editPath = (value: string) => act(() => {
  const input = element.querySelector<HTMLInputElement>('#remote-directory-path')!
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, value)
  input.dispatchEvent(new Event('input', { bubbles: true }))
})
const click = async (label: string) => { await act(async () => { button(label).click(); await Promise.resolve() }) }

beforeEach(() => {
  element = document.createElement('div')
  document.body.appendChild(element)
  root = createRoot(element)
})
afterEach(() => {
  act(() => root.unmount())
  element.remove()
  useAppStore.setState({ listRemoteDirectories: originalList })
})

describe('RemoteDirectoryBrowser', () => {
  it.each(['relative', '/missing', '/rejected'])('keeps %s invalid after error dismissal and allows a successful retry', async (failedPath) => {
    const onSelect = vi.fn()
    const listRemoteDirectories = vi.fn(async (_id: string, path: string) => {
      if (path === '/missing') return { ok: false as const, error: 'Directory not found.' }
      if (path === '/rejected') throw new Error('SSH connection lost.')
      return listing(path)
    })
    useAppStore.setState({ listRemoteDirectories })
    await act(async () => { root.render(<RemoteDirectoryBrowser host={remoteHost} initialPath="/valid" onCancel={() => {}} onSelect={onSelect} />) })
    expect(button('Use this directory').disabled).toBe(false)
    editPath(failedPath)
    expect(button('Use this directory').disabled).toBe(true)
    await click('Go')
    const error = element.querySelector<HTMLElement>('[role="alert"]')!
    expect(error.textContent).toContain(failedPath === 'relative' ? 'Enter an absolute Unix path.' : failedPath === '/missing' ? 'Directory not found.' : 'SSH connection lost.')
    expect(error.getAttribute('style')).toContain('var(--red)')
    if (failedPath === 'relative') expect(listRemoteDirectories).not.toHaveBeenCalledWith('lab', 'relative')
    await click('Dismiss directory error')
    expect(element.querySelector('[role="alert"]')).toBeNull()
    expect(button('Use this directory').disabled).toBe(true)
    await click('Use this directory')
    expect(onSelect).not.toHaveBeenCalled()
    editPath('/retry')
    await click('Go')
    await click('Use this directory')
    expect(onSelect).toHaveBeenCalledWith('/retry')
  })

  it('rejects a malformed successful listing', async () => {
    useAppStore.setState({ listRemoteDirectories: vi.fn().mockResolvedValue(listing('relative')) })
    await act(async () => { root.render(<RemoteDirectoryBrowser host={remoteHost} initialPath="/valid" onCancel={() => {}} onSelect={() => {}} />) })
    expect(element.textContent).toContain('invalid directory path')
    await click('Dismiss directory error')
    expect(button('Use this directory').disabled).toBe(true)
  })

  it('ignores stale replies after editing and loading a newer path', async () => {
    let finishOld: (result: RemoteDirectoryBrowseResult) => void = () => {}
    useAppStore.setState({ listRemoteDirectories: vi.fn((_id, path) => path === '/old'
      ? new Promise<RemoteDirectoryBrowseResult>((resolve) => { finishOld = resolve })
      : Promise.resolve(listing(path))) })
    const onSelect = vi.fn()
    await act(async () => { root.render(<RemoteDirectoryBrowser host={remoteHost} initialPath="/old" onCancel={() => {}} onSelect={onSelect} />) })
    expect(button('Use this directory').disabled).toBe(true)
    editPath('/new')
    await click('Go')
    await act(async () => { finishOld(listing('/old')) })
    expect(element.querySelector<HTMLInputElement>('input')!.value).toBe('/new')
    await click('Use this directory')
    expect(onSelect).toHaveBeenCalledWith('/new')
  })

  it('invalidates listings when the selected computer changes or goes offline', async () => {
    const onSelect = vi.fn()
    let finishOld: (result: RemoteDirectoryBrowseResult) => void = () => {}
    const listRemoteDirectories = vi.fn((id: string, path: string) => id === 'lab'
      ? new Promise<RemoteDirectoryBrowseResult>((resolve) => { finishOld = resolve })
      : Promise.resolve(listing(path)))
    useAppStore.setState({ listRemoteDirectories })
    await act(async () => { root.render(<RemoteDirectoryBrowser host={remoteHost} initialPath="/old" onCancel={() => {}} onSelect={onSelect} />) })
    const windows = { ...remoteHost, id: 'windows', platform: 'windows' }
    await act(async () => { root.render(<RemoteDirectoryBrowser host={windows} initialPath={'C:\\work'} onCancel={() => {}} onSelect={onSelect} />) })
    await act(async () => { finishOld(listing('/old')) })
    expect(listRemoteDirectories).toHaveBeenLastCalledWith('windows', 'C:\\work')
    expect(element.querySelector<HTMLInputElement>('input')!.value).toBe('C:\\work')
    expect(button('Use this directory').disabled).toBe(false)
    await act(async () => { root.render(<RemoteDirectoryBrowser host={{ ...windows, runnerStatus: 'offline' }} initialPath={'C:\\work'} onCancel={() => {}} onSelect={onSelect} />) })
    expect(button('Use this directory').disabled).toBe(true)
    await click('Use this directory')
    expect(onSelect).not.toHaveBeenCalled()
  })
})
