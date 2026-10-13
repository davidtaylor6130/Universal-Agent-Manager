import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import * as bridge from '../../ipc/cefBridge'
import { ProviderAgentImport } from './ProviderAgentImport'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const reviewer = { id: 'reviewer', name: 'Reviewer', description: 'Reviews diffs', sourcePath: '/home/u/.config/opencode/agent/reviewer.md' }
const writer = { id: 'writer', name: 'Writer', sourcePath: '/home/u/.config/opencode/agent/writer.md', alreadyImported: true }
let root: Root
let host: HTMLDivElement

beforeEach(() => {
  vi.restoreAllMocks()
  host = document.createElement('div')
  document.body.appendChild(host)
  root = createRoot(host)
})
afterEach(() => { act(() => root.unmount()); host.remove() })

const button = (text: string) => [...host.querySelectorAll<HTMLButtonElement>('button')].find(item => item.textContent === text)
const click = async (text: string) => {
  expect(button(text)).toBeTruthy()
  await act(async () => button(text)!.click())
}
const toggle = async (label: string) => {
  const input = host.querySelector<HTMLInputElement>(`input[aria-label="${label}"]`)
  expect(input).toBeTruthy()
  await act(async () => input!.click())
}

it('lists provider agents, imports only the ticked ones, and refreshes the list', async () => {
  const send = vi.spyOn(bridge, 'sendToCEF')
    .mockResolvedValueOnce({ ok: true, data: { agents: [reviewer, writer] } })
    .mockResolvedValueOnce({ ok: true, data: { imported: 1 } })
    .mockResolvedValueOnce({ ok: true, data: { agents: [{ ...reviewer, alreadyImported: true }, writer] } })
  await act(async () => root.render(<ProviderAgentImport />))
  await click('Find agents')

  expect(send).toHaveBeenCalledWith({ action: 'listProviderAgents', payload: { providerId: 'opencode-cli' } })
  expect(host.textContent).toContain('Reviews diffs')
  expect(host.textContent).toContain('/home/u/.config/opencode/agent/reviewer.md')
  expect(host.textContent).toContain('Already in UAM')
  expect(button('Import')!.disabled).toBe(true)

  await toggle('Import Reviewer')
  await click('Import 1')
  expect(send).toHaveBeenNthCalledWith(2, { action: 'importProviderAgents', payload: { providerId: 'opencode-cli', agentIds: ['reviewer'] } })
  expect(host.textContent).toContain('Imported 1 agent')
  expect(send).toHaveBeenCalledTimes(3)
})

it('keeps failed agents ticked and shows the reason', async () => {
  vi.spyOn(bridge, 'sendToCEF')
    .mockResolvedValueOnce({ ok: true, data: { agents: [reviewer, writer] } })
    .mockResolvedValueOnce({ ok: true, data: { imported: 1, failed: [{ id: 'writer', error: 'Name already used.' }] } })
    .mockResolvedValueOnce({ ok: true, data: { agents: [{ ...reviewer, alreadyImported: true }, writer] } })
  await act(async () => root.render(<ProviderAgentImport />))
  await click('Find agents')
  await act(async () => button('Select all')!.click())
  await click('Import 2')

  expect(host.textContent).toContain('Name already used.')
  expect(host.querySelector<HTMLInputElement>('input[aria-label="Import writer"]')!.checked).toBe(true)
  expect(host.querySelector<HTMLInputElement>('input[aria-label="Import Reviewer"]')!.checked).toBe(false)
})

it('shows a plain empty state when the provider has no agents', async () => {
  vi.spyOn(bridge, 'sendToCEF').mockResolvedValueOnce({ ok: true, data: { agents: [] } })
  await act(async () => root.render(<ProviderAgentImport />))
  await click('Find agents')
  expect(host.textContent).toContain('OpenCode has no agent files on this computer.')
})

it('shows the failure to read agents instead of an empty list', async () => {
  vi.spyOn(bridge, 'sendToCEF').mockResolvedValueOnce({ ok: false, error: 'Provider config is unreadable.' })
  await act(async () => root.render(<ProviderAgentImport />))
  await click('Find agents')
  expect(document.querySelector('[role="alert"]')?.textContent).toContain('Provider config is unreadable.')
  expect(host.textContent).not.toContain('Agents found')
})

it('shows a moving loading skeleton while agents are being looked for', async () => {
  let resolve!: (value: unknown) => void
  vi.spyOn(bridge, 'sendToCEF').mockReturnValueOnce(new Promise(done => { resolve = done }) as ReturnType<typeof bridge.sendToCEF>)
  await act(async () => root.render(<ProviderAgentImport />))
  await click('Find agents')
  expect(host.querySelector('[aria-label="Looking for agents"]')).toBeTruthy()
  await act(async () => resolve({ ok: true, data: { agents: [] } }))
  expect(host.querySelector('[aria-label="Looking for agents"]')).toBeNull()
})
