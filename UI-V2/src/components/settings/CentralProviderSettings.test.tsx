import { act, createRef } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import * as bridge from '../../ipc/cefBridge'
import { CentralProviderSettings, type CentralProviderSettingsHandle } from './CentralProviderSettings'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
;(globalThis as typeof globalThis & { ResizeObserver?: typeof ResizeObserver }).ResizeObserver ??= class {
  observe() {}
  unobserve() {}
  disconnect() {}
} as unknown as typeof ResizeObserver
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

const instructionPath = '/tmp/private-AGENTS.md'
const editInstructionText = (value: string) => act(() => {
  const textarea = host.querySelector<HTMLTextAreaElement>('[aria-label="Instruction file contents"]')!
  Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(textarea, value)
  textarea.dispatchEvent(new Event('input', { bubbles: true }))
})
const openInstructions = async () => {
  useAppStore.setState({ centralProviderConfiguration: { ...defaults, instructionFiles: [instructionPath] } })
  await act(async () => root.render(<CentralProviderSettings ref={handle} />))
  await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Edit private-AGENTS.md"]')!.click())
}

it('edits a saved instruction file inline without saving unchanged content', async () => {
  const send = vi.mocked(bridge.sendToCEF)
  send.mockImplementation(async (request) => request.action === 'readCentralInstructionFile'
    ? { ok: true, data: { path: instructionPath, content: 'Keep CRLF\r\n日本語\r\n' } }
    : request.action === 'saveCentralInstructionFile'
      ? { ok: true, data: { path: instructionPath, content: request.payload.content } }
      : { ok: true, data: { agents: [], errors: [] } })
  await openInstructions()
  expect(host.querySelector<HTMLButtonElement>('[aria-label="Save instruction file"]')!.disabled).toBe(true)
  expect(document.activeElement).toBe(host.querySelector('[aria-label="Instruction file contents"]'))
  editInstructionText('Updated instructions\n')
  await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Save instruction file"]')!.click())
  expect(send).toHaveBeenCalledWith({ action: 'saveCentralInstructionFile', payload: {
    path: instructionPath, content: 'Updated instructions\n', expectedContent: 'Keep CRLF\r\n日本語\r\n',
  } })
  expect(host.querySelector<HTMLButtonElement>('[aria-label="Save instruction file"]')!.disabled).toBe(true)
  expect(useAppStore.getState().centralProviderConfiguration.instructionFiles).toEqual([instructionPath])
  expect(document.activeElement).toBe(host.querySelector('[aria-label="Instruction file contents"]'))
})

it('retains an instruction draft after conflict and guards leaving until discard', async () => {
  vi.mocked(bridge.sendToCEF).mockImplementation(async (request) => request.action === 'readCentralInstructionFile'
    ? { ok: true, data: { path: instructionPath, content: 'Original\n' } }
    : request.action === 'saveCentralInstructionFile'
      ? { ok: false, error: 'The file changed outside UAM. Reopen it before saving.' }
      : { ok: true, data: { agents: [], errors: [] } })
  await openInstructions()
  editInstructionText('Keep this unsaved draft\n')
  await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Save instruction file"]')!.click())
  expect(host.querySelector<HTMLTextAreaElement>('[aria-label="Instruction file contents"]')!.value).toBe('Keep this unsaved draft\n')
  expect(host.querySelector('[role="alert"]')?.textContent).toContain('The file changed outside UAM')
  const leave = vi.fn()
  act(() => handle.current!.requestLeave(leave))
  expect(leave).not.toHaveBeenCalled()
  await click('Keep editing')
  expect(host.querySelector<HTMLTextAreaElement>('[aria-label="Instruction file contents"]')!.value).toBe('Keep this unsaved draft\n')
  act(() => handle.current!.requestLeave(leave))
  await click('Discard')
  expect(leave).toHaveBeenCalledOnce()
})

it('requires saving a chosen instruction path before editing its file', async () => {
  await editDefaults()
  expect(host.querySelector<HTMLButtonElement>('[aria-label="Edit private-AGENTS.md"]')!.disabled).toBe(true)
  expect(vi.mocked(bridge.sendToCEF).mock.calls.some(([request]) => request.action === 'readCentralInstructionFile')).toBe(false)
})

it('allows enabling an unchanged legacy setup and disables Save once enabled', async () => {
  const save = vi.fn(async (configuration: typeof defaults) => { useAppStore.setState({ centralProviderConfiguration: configuration }); return { ok: true } })
  useAppStore.setState({ centralProviderConfiguration: { ...defaults, enabled: false, instructions: 'Legacy instructions' }, setCentralProviderConfiguration: save })
  await act(async () => root.render(<CentralProviderSettings ref={handle} />))
  await click('Save')
  expect(save).toHaveBeenCalledWith(expect.objectContaining({ enabled: true, instructions: 'Legacy instructions' }))
  expect([...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === 'Save')!.disabled).toBe(true)
})

it('keeps an empty-file editor mounted after clearing a failed save draft', async () => {
  vi.mocked(bridge.sendToCEF).mockImplementation(async request => request.action === 'readCentralInstructionFile'
    ? { ok: true, data: { path: instructionPath, content: '' } }
    : request.action === 'saveCentralInstructionFile' ? { ok: false, error: 'Disk full' } : { ok: true, data: { agents: [], errors: [] } })
  await openInstructions()
  editInstructionText('x')
  await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Save instruction file"]')!.click())
  editInstructionText('')
  expect(host.querySelector<HTMLTextAreaElement>('[aria-label="Instruction file contents"]')!.value).toBe('')
})

it('focuses Retry after a read failure and shows the defaults error when leaving', async () => {
  vi.mocked(bridge.sendToCEF).mockImplementation(async request => request.action === 'readCentralInstructionFile'
    ? { ok: false, error: 'Could not read the instruction file' }
    : { ok: true, data: { agents: [], errors: [] } })
  useAppStore.setState({ browseProviderAgentImport: vi.fn(async () => '/tmp/extra-instructions.md'), setCentralProviderConfiguration: vi.fn(async () => ({ ok: false, error: 'Defaults could not be saved' })) })
  await openInstructions()
  expect(document.activeElement?.textContent).toBe('Retry')
  await click('Choose file…')
  const leave = vi.fn()
  act(() => handle.current!.requestLeave(leave))
  await click('Save and leave')
  const dialog = document.querySelector('[role="alertdialog"]')!
  expect(dialog.querySelector('[role="alert"]')?.textContent).toBe('Defaults could not be saved')
  expect(leave).not.toHaveBeenCalled()
})

it('does not move focus behind the leave dialog after a failed defaults save', async () => {
  let resolve!: (result: { ok: boolean; error: string }) => void
  const pending = new Promise<{ ok: boolean; error: string }>(done => { resolve = done })
  vi.mocked(bridge.sendToCEF).mockImplementation(async request => request.action === 'readCentralInstructionFile'
    ? { ok: true, data: { path: instructionPath, content: 'Original' } }
    : { ok: true, data: { agents: [], errors: [] } })
  useAppStore.setState({ browseProviderAgentImport: vi.fn(async () => '/tmp/extra-instructions.md'), setCentralProviderConfiguration: vi.fn(async () => pending) })
  await openInstructions()
  await click('Choose file…')
  act(() => handle.current!.requestLeave(vi.fn()))
  await click('Save and leave')
  expect(host.querySelector<HTMLTextAreaElement>('[aria-label="Instruction file contents"]')!.disabled).toBe(true)
  await act(async () => resolve({ ok: false, error: 'Defaults could not be saved' }))
  expect(document.activeElement).not.toBe(host.querySelector('[aria-label="Instruction file contents"]'))
  expect(document.querySelector('[role="alertdialog"]')).not.toBeNull()
})

it('keeps focus in the leave dialog after an instruction save fails', async () => {
  let resolve!: (result: { ok: false; error: string }) => void
  const pending = new Promise<{ ok: false; error: string }>(done => { resolve = done })
  vi.mocked(bridge.sendToCEF).mockImplementation(async request => request.action === 'readCentralInstructionFile'
    ? { ok: true, data: { path: instructionPath, content: 'Original' } }
    : request.action === 'saveCentralInstructionFile' ? pending : { ok: true, data: { agents: [], errors: [] } })
  await openInstructions()
  editInstructionText('Keep this draft')
  const leave = vi.fn()
  act(() => handle.current!.requestLeave(leave))
  const dialog = document.querySelector<HTMLElement>('[role="alertdialog"]')!
  const save = [...dialog.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === 'Save and leave')!
  act(() => { save.focus(); save.click() })
  await act(async () => resolve({ ok: false, error: 'Disk full' }))
  expect(document.querySelector('[role="alertdialog"]')).toBe(dialog)
  expect(dialog.contains(document.activeElement)).toBe(true)
  expect(host.querySelector<HTMLTextAreaElement>('[aria-label="Instruction file contents"]')!.value).toBe('Keep this draft')
  expect(dialog.querySelector('[role="alert"]')?.textContent).toBe('Disk full')
  expect(leave).not.toHaveBeenCalled()
})

it('keeps a file save single-flight and blocks leaving until it settles', async () => {
  let resolve!: (result: { ok: boolean; data: { path: string; content: string } }) => void
  const pending = new Promise<{ ok: boolean; data: { path: string; content: string } }>(done => { resolve = done })
  const send = vi.mocked(bridge.sendToCEF)
  send.mockImplementation(async request => request.action === 'readCentralInstructionFile'
    ? { ok: true, data: { path: instructionPath, content: 'Original' } }
    : request.action === 'saveCentralInstructionFile' ? pending : { ok: true, data: { agents: [], errors: [] } })
  await openInstructions()
  editInstructionText('Changed')
  const save = host.querySelector<HTMLButtonElement>('[aria-label="Save instruction file"]')!
  const leave = vi.fn()
  act(() => { save.click(); save.click(); handle.current!.requestLeave(leave) })
  expect(send.mock.calls.filter(([request]) => request.action === 'saveCentralInstructionFile')).toHaveLength(1)
  expect(leave).not.toHaveBeenCalled()
  await act(async () => resolve({ ok: true, data: { path: instructionPath, content: 'Changed' } }))
  act(() => handle.current!.requestLeave(leave))
  expect(leave).toHaveBeenCalledOnce()
})

it('retains a draft when its file is removed from the saved configuration', async () => {
  const send = vi.mocked(bridge.sendToCEF)
  send.mockImplementation(async request => request.action === 'readCentralInstructionFile'
    ? { ok: true, data: { path: instructionPath, content: 'Original' } }
    : { ok: true, data: { agents: [], errors: [] } })
  await openInstructions()
  editInstructionText('Retain me')
  act(() => useAppStore.setState({ centralProviderConfiguration: defaults }))
  await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Save instruction file"]')!.click())
  expect(send.mock.calls.some(([request]) => request.action === 'saveCentralInstructionFile')).toBe(false)
  expect(host.querySelector<HTMLTextAreaElement>('[aria-label="Instruction file contents"]')!.value).toBe('Retain me')
  expect(host.querySelector('[role="alert"]')?.textContent).toContain('no longer in Central Setup')
})

it('disables clean custom agent Save and enables only changed drafts', async () => {
  const send = vi.mocked(bridge.sendToCEF)
  send.mockResolvedValue({ ok: true, data: { agents: [{ id: 'reviewer', description: 'Review', mode: 'primary', workspaceAccess: 'read', skills: [], delegates: [], instructions: '', builtIn: false }], errors: [] } })
  await act(async () => root.render(<CentralProviderSettings ref={handle} />))
  await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Edit reviewer"]')!.click())
  const save = [...document.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === 'Save agent')!
  const description = document.querySelector<HTMLInputElement>('[aria-label="Agent description"]')!
  expect(save.disabled).toBe(true)
  await act(async () => save.click())
  expect(send.mock.calls.some(([request]) => request.action === 'saveAgentDefinition')).toBe(false)
  const change = (value: string) => act(() => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(description, value)
    description.dispatchEvent(new Event('input', { bubbles: true }))
  })
  change('Changed review')
  expect(save.disabled).toBe(false)
  change('Review')
  expect(save.disabled).toBe(true)
})

it('keeps new agent Save disabled for invalid names and allows a valid new draft', async () => {
  await act(async () => root.render(<CentralProviderSettings ref={handle} />))
  await click('Add agent')
  const save = [...document.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === 'Save agent')!
  const name = document.querySelector<HTMLInputElement>('[aria-label="Agent name"]')!
  const change = (value: string) => act(() => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(name, value)
    name.dispatchEvent(new Event('input', { bubbles: true }))
  })
  expect(save.disabled).toBe(true)
  change('bad name')
  expect(save.disabled).toBe(true)
  change('build')
  expect(save.disabled).toBe(true)
  change('reviewer')
  expect(save.disabled).toBe(false)
})
