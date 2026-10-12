import { act, createRef } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import * as bridge from '../../ipc/cefBridge'
import { CentralProviderSettings, type CentralProviderSettingsHandle } from './CentralProviderSettings'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const defaults = { ...useAppStore.getState().centralProviderConfiguration, instructionFiles: [], defaultSkills: [], defaultAgents: [], defaultAgentId: 'build' }
let root: Root
let host: HTMLDivElement
const handle = createRef<CentralProviderSettingsHandle>()

beforeEach(() => {
  vi.restoreAllMocks()
  vi.spyOn(bridge, 'sendToCEF').mockResolvedValue({ ok: true, data: { agents: [{ id: 'build', description: 'Build', mode: 'primary', workspaceAccess: 'write', skills: [], delegates: [], instructions: '', builtIn: true }], errors: [] } })
  useAppStore.setState({ centralProviderConfiguration: defaults, markdownStoreEntries: [], refreshMarkdownStore: vi.fn(async () => true), browseProviderAgentImport: vi.fn(async () => '/tmp/private-AGENTS.md') })
  host = document.createElement('div')
  document.body.appendChild(host)
  root = createRoot(host)
})
afterEach(() => { act(() => root.unmount()); host.remove() })
const click = async (text: string) => {
  const button = [...document.querySelectorAll<HTMLButtonElement>('button')].find(item => item.textContent === text)
  expect(button).toBeTruthy()
  await act(async () => button!.click())
}
const editDefaults = async () => {
  await act(async () => root.render(<CentralProviderSettings ref={handle} />))
  await click('Choose file…')
}

it('guards changed defaults and retains the saved configuration on discard', async () => {
  await editDefaults()
  const leave = vi.fn()
  act(() => handle.current!.requestLeave(leave))
  expect(document.querySelector('[aria-label="Save central setup changes?"]')).toBeTruthy()
  await click('Keep editing')
  expect(leave).not.toHaveBeenCalled()
  expect(host.textContent).toContain('private-AGENTS.md')
  act(() => handle.current!.requestLeave(leave))
  await click('Discard')
  expect(leave).toHaveBeenCalledOnce()
  expect(useAppStore.getState().centralProviderConfiguration.instructionFiles).toEqual([])
})

it('keeps a rejected save guarded and leaves only after successful retry', async () => {
  const save = vi.fn(async (configuration: typeof defaults) => { useAppStore.setState({ centralProviderConfiguration: configuration }); return { ok: true } })
  save.mockImplementationOnce(async () => ({ ok: false, error: 'Private save rejection' }))
  useAppStore.setState({ setCentralProviderConfiguration: save })
  await editDefaults()
  const leave = vi.fn()
  act(() => handle.current!.requestLeave(leave))
  await click('Save and leave')
  expect(leave).not.toHaveBeenCalled()
  expect(document.querySelector('[role="alert"]')?.textContent).toContain('Private save rejection')
  expect(host.textContent).toContain('private-AGENTS.md')
  await click('Save and leave')
  expect(save).toHaveBeenCalledTimes(2)
  expect(leave).toHaveBeenCalledOnce()
  expect(useAppStore.getState().centralProviderConfiguration.instructionFiles).toEqual(['/tmp/private-AGENTS.md'])
})

it('does not leave while a settings save is pending', async () => {
  let resolve!: (result: { ok: boolean }) => void
  const pending = new Promise<{ ok: boolean }>(done => { resolve = done })
  useAppStore.setState({ setCentralProviderConfiguration: vi.fn(async (configuration) => { const result = await pending; if (result.ok) useAppStore.setState({ centralProviderConfiguration: configuration }); return result }) })
  await editDefaults()
  await click('Save')
  const leave = vi.fn()
  act(() => handle.current!.requestLeave(leave))
  expect(leave).not.toHaveBeenCalled()
  await act(async () => resolve({ ok: true }))
  act(() => handle.current!.requestLeave(leave))
  expect(leave).toHaveBeenCalledOnce()
})
