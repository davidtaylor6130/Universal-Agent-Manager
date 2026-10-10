import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { McpServerSetup, fromJsonText, toJsonText } from './McpServerSetup'
import { useAppStore, type McpServerConfiguration } from '../../store/useAppStore'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
let host: HTMLDivElement
let root: Root
const server: McpServerConfiguration = { id: 'pw', name: 'playwright', executionHostId: '', workspaceDirectory: '', transport: 'stdio', command: '/usr/local/bin/npx', args: ['-y', '@playwright/mcp@latest'], url: '', environment: [{ name: 'TOKEN', environmentVariable: 'PW_TOKEN' }], headers: [], enabled: true }
const saveServers = vi.fn(async (servers: McpServerConfiguration[]) => { useAppStore.setState({ mcpServers: servers }); return { ok: true } })
const saveCentral = vi.fn(async (configuration: ReturnType<typeof useAppStore.getState>['centralProviderConfiguration']) => { useAppStore.setState({ centralProviderConfiguration: configuration }); return { ok: true } })
beforeEach(async () => {
  saveServers.mockClear(); saveCentral.mockClear()
  useAppStore.setState({ mcpServers: [server], sessions: [], activeSessionId: null, executionHosts: [], centralProviderConfiguration: { enabled: true, instructions: '', instructionFiles: [], skillDirectories: [], defaultAgentId: 'build', uamControlEnabled: true }, setMcpServers: saveServers, setCentralProviderConfiguration: saveCentral })
  host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host)
  await act(async () => root.render(<McpServerSetup onBusyChange={() => {}} onDirtyChange={() => {}} />))
})
afterEach(async () => { await act(async () => root.unmount()); host.remove() })
const button = (text: string) => [...host.querySelectorAll('button')].find(item => item.textContent?.trim() === text)!
async function click(text: string) { await act(async () => button(text).click()) }
async function type(field: HTMLInputElement | HTMLTextAreaElement, value: string) {
  await act(async () => {
    Object.getOwnPropertyDescriptor(field instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype, 'value')!.set!.call(field, value)
    field.dispatchEvent(new Event('input', { bubbles: true }))
  })
}

it('round-trips OpenCode-style JSON with the built-in UAM service and env references', () => {
  const text = toJsonText([server], false)
  expect(JSON.parse(text).mcp).toEqual({ 'uam-mcp': { type: 'builtin', enabled: false }, playwright: { type: 'local', command: ['/usr/local/bin/npx', '-y', '@playwright/mcp@latest'], environment: { TOKEN: '{env:PW_TOKEN}' }, enabled: true } })
  expect(fromJsonText(text, [server])).toEqual({ servers: [server], uamEnabled: false })
  const literal = fromJsonText('{"mcp":{"x":{"type":"remote","url":"http://main.homelab.com:9001/mcp?userToken=abc","headers":{"Authorization":"Bearer t"}}}}', [])
  expect(literal.servers[0]).toMatchObject({ transport: 'http', url: 'http://main.homelab.com:9001/mcp?userToken=abc', headers: [{ name: 'Authorization', environmentVariable: '', value: 'Bearer t' }] })
  expect(JSON.parse(toJsonText(literal.servers, true)).mcp.x.headers).toEqual({ Authorization: 'Bearer t' })
})

it('shows built-ins without delete buttons and deletes a user server only after save', async () => {
  expect(host.textContent).toContain('UAM MCP service')
  expect(host.textContent).toContain('UAM Computer Use')
  expect(host.querySelectorAll('button[aria-label^="Delete"]')).toHaveLength(1)
  await click('Delete')
  expect(saveServers).not.toHaveBeenCalled()
  await click('Save')
  expect(saveServers).toHaveBeenCalledWith([])
})

it('adds a server through the form and blocks a relative program path', async () => {
  await click('Add server')
  const inputs = () => [...host.querySelectorAll('input:not([type=checkbox])')] as HTMLInputElement[]
  await type(inputs()[0], 'docs')
  await type(inputs()[1], 'docs-server')
  await click('Save')
  expect(host.textContent).toContain('program path must be a full path')
  await type(inputs()[1], '/opt/docs/server')
  await click('Add argument')
  await type(host.querySelector('input[aria-label="Argument 1"]') as HTMLInputElement, '--stdio')
  await click('Save')
  expect(saveServers.mock.calls[0][0][1]).toMatchObject({ name: 'docs', transport: 'stdio', command: '/opt/docs/server', args: ['--stdio'] })
})

it('edits in JSON, toggles the built-in UAM service and saves both', async () => {
  await click('JSON')
  const area = host.querySelector('textarea[aria-label="MCP JSON"]') as HTMLTextAreaElement
  await type(area, area.value.replace('"enabled": true\n    },\n    "playwright"', '"enabled": false\n    },\n    "playwright"'))
  await click('Save')
  expect(saveServers).toHaveBeenCalledWith([server])
  expect(saveCentral.mock.calls[0][0].uamControlEnabled).toBe(false)
  await type(area, '{ not json')
  await click('Form')
  expect(host.textContent).toContain('not valid')
})
