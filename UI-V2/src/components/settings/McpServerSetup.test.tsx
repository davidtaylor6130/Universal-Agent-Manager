import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { McpServerSetup } from './McpServerSetup'
import { CentralProviderSettings } from './CentralProviderSettings'
import { useAppStore, type McpServerConfiguration } from '../../store/useAppStore'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const nativeSaveServers = useAppStore.getState().setMcpServers
let host: HTMLDivElement
let root: Root
const entry = (): McpServerConfiguration => ({ id: 'existing', name: 'Existing', executionHostId: '', workspaceDirectory: '', transport: 'stdio', command: '/bin/server', args: [], url: '', environment: [], headers: [], enabled: true })
const saveServers = vi.fn(async (servers: McpServerConfiguration[]) => { useAppStore.setState({ mcpServers: servers }); return { ok: true } })
beforeEach(async () => {
  window.cefQuery = undefined
  saveServers.mockClear()
  useAppStore.setState({ mcpServers: [], sessions: [], activeSessionId: null, executionHosts: [{ id: 'local', label: 'This Mac', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' }, { id: 'homelab', label: 'Homelab', transport: 'ssh', sshAlias: 'homelab', runnerStatus: 'ready', runnerVersion: '', platform: 'linux', architecture: 'x86_64', lastSeenAt: '' }], centralProviderConfiguration: { enabled: false, instructions: '', instructionFiles: [], skillDirectories: [], defaultAgentId: 'build', uamControlEnabled: true }, setMcpServers: saveServers })
  host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host)
  await act(async () => root.render(<McpServerSetup onBusyChange={() => {}} />))
})
afterEach(async () => { await act(async () => root.unmount()); host.remove() })
async function click(text: string) {
  const button = [...host.querySelectorAll('button')].find(item => item.textContent?.trim() === text)
  expect(button, text).toBeDefined()
  await act(async () => button!.click())
}
async function fill(label: string, value: string) {
  const field = [...host.querySelectorAll('label')].find(item => item.firstChild?.textContent === label)?.querySelector('input,textarea,select') as HTMLInputElement
  expect(field, label).toBeDefined()
  await act(async () => {
    const prototype = field.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : field.tagName === 'SELECT' ? HTMLSelectElement.prototype : HTMLInputElement.prototype
    Object.getOwnPropertyDescriptor(prototype, 'value')!.set!.call(field, value)
    field.dispatchEvent(new Event(field.tagName === 'SELECT' ? 'change' : 'input', { bubbles: true }))
  })
}
async function connection() { await click('Add MCP server'); await fill('Server name', 'Docs'); await fill('Executable path', '/bin/server'); await fill('Arguments, one per line', '--path\n/a path/with spaces\n'); await click('Next') }

it('saves a single global launch configuration and preserves concurrently added servers', async () => {
  await connection(); await click('Next')
  await act(async () => useAppStore.setState({ mcpServers: [entry()] }))
  await click('Save for all providers')
  expect(saveServers).toHaveBeenCalledTimes(1)
  const saved = useAppStore.getState().mcpServers
  expect(saved).toHaveLength(2)
  expect(saved[1]).toMatchObject({ name: 'Docs', workspaceDirectory: '', executionHostId: '', args: ['--path', '/a path/with spaces'], enabled: true })
  expect(host.textContent).toContain('Server saved for all five providers')
})
it('saves SSH HTTP headers as secret references without command fields', async () => {
  await click('Add MCP server'); await fill('Server name', 'Remote docs'); await fill('Connection type', 'http'); await fill('Server URL', 'http://localhost:8080/mcp'); await click('Next')
  await fill('Execution host', 'homelab'); await fill('Workspace path (leave empty for all workspaces)', '/srv/project'); await click('Add header'); await fill('Header name 1', 'Authorization'); await fill('Secret variable 1', 'MCP_TOKEN'); await click('Next'); await click('Save for all providers')
  expect(useAppStore.getState().mcpServers[0]).toMatchObject({ executionHostId: 'homelab', workspaceDirectory: '/srv/project', transport: 'http', command: '', args: [], environment: [], headers: [{ name: 'Authorization', environmentVariable: 'MCP_TOKEN' }] })
})
it('blocks invalid paths and unsupported remote URLs before saving', async () => {
  await click('Add MCP server'); await fill('Server name', 'Docs'); await fill('Executable path', 'npx'); await click('Next')
  expect(host.textContent).toContain('absolute executable path')
  await fill('Connection type', 'http'); await fill('Server URL', 'https://example.com/mcp'); await click('Next')
  expect(host.textContent).toContain('localhost HTTP or HTTPS URL'); expect(saveServers).not.toHaveBeenCalled()
})
it('keeps the wizard open when the backend rejects configuration', async () => {
  saveServers.mockImplementationOnce(async () => ({ ok: false, error: 'Secret variable is unavailable.' }))
  await connection(); await click('Next'); await click('Save for all providers')
  expect(host.textContent).toContain('Secret variable is unavailable.'); expect(host.textContent).toContain('Step 3 of 3'); expect(host.textContent).not.toContain('Server saved for all five')
})
it('rejects stale edits rather than replacing a changed server', async () => {
  await act(async () => useAppStore.setState({ mcpServers: [entry()] })); await click('Edit Existing'); await click('Next'); await click('Next')
  await act(async () => useAppStore.setState({ mcpServers: [{ ...entry(), command: '/bin/new-server' }] })); await click('Save for all providers')
  expect(host.textContent).toContain('changed while you were editing'); expect(saveServers).not.toHaveBeenCalled()
})
it('edits, disables and removes the selected server without affecting other entries', async () => {
  await act(async () => useAppStore.setState({ mcpServers: [entry(), { ...entry(), id: 'other', name: 'Other' }] }))
  await click('Edit Existing'); await fill('Server name', 'Updated'); await click('Next'); await click('Next'); await click('Save for all providers'); await click('Disable Updated')
  expect(useAppStore.getState().mcpServers.find(server => server.id === 'existing')?.enabled).toBe(false)
  await click('Remove Updated'); expect(useAppStore.getState().mcpServers).toHaveLength(2); await click('Confirm removal'); expect(useAppStore.getState().mcpServers.map(server => server.id)).toEqual(['other'])
})
it('preserves the visible UAM service default while saving dirty native resource fields', async () => {
  const saveCentral = vi.fn(async configuration => { useAppStore.setState({ centralProviderConfiguration: configuration }); return { ok: true } })
  useAppStore.setState({ setCentralProviderConfiguration: saveCentral })
  await act(async () => root.render(<CentralProviderSettings showUamTools={false} />))
  const instructions = host.querySelector<HTMLTextAreaElement>('[aria-label="Central instructions"]')!
  await act(async () => { Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(instructions, 'Native launch instructions'); instructions.dispatchEvent(new Event('input', { bubbles: true })) })
  await act(async () => useAppStore.setState({ centralProviderConfiguration: { ...useAppStore.getState().centralProviderConfiguration, uamControlEnabled: false } }))
  await click('Save')
  expect(saveCentral).toHaveBeenCalledWith(expect.objectContaining({ instructions: 'Native launch instructions', uamControlEnabled: false }))
})

it('submits the wizard configuration through the native settings bridge without sending a chat message', async () => {
  const requests: Array<{ action: string; payload: { servers: McpServerConfiguration[] } }> = []
  useAppStore.setState({ setMcpServers: nativeSaveServers })
  window.cefQuery = request => { requests.push(JSON.parse(request.request)); request.onSuccess(JSON.stringify({ ok: true })) }
  await connection(); await click('Next'); await click('Save for all providers')
  expect(requests).toHaveLength(1)
  expect(requests[0].action).toBe('setMcpServers')
  expect(requests[0].payload.servers[0]).toMatchObject({ name: 'Docs', executionHostId: '', args: ['--path', '/a path/with spaces'] })
  window.cefQuery = undefined
})
