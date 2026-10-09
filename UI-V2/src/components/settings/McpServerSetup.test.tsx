import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { McpServerSetup, fromJsonText, toJsonText, mergeMcpImport } from './McpServerSetup'
import { sendToCEF } from '../../ipc/cefBridge'
import { useAppStore, type McpServerConfiguration } from '../../store/useAppStore'

const buildFeatures = vi.hoisted(() => ({ COMPUTER_USE_ENABLED: true }))
vi.mock('../../config/buildFeatures', () => buildFeatures)
vi.mock('../../ipc/cefBridge', async importOriginal => ({ ...await importOriginal<typeof import('../../ipc/cefBridge')>(), sendToCEF: vi.fn() }));

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
let host: HTMLDivElement
let root: Root
const server: McpServerConfiguration = { id: 'pw', name: 'playwright', executionHostId: '', workspaceDirectory: '', transport: 'stdio', command: '/usr/local/bin/npx', args: ['-y', '@playwright/mcp@latest'], url: '', environment: [{ name: 'TOKEN', environmentVariable: 'PW_TOKEN' }], headers: [], enabled: true }
const saveServers = vi.fn(async (servers: McpServerConfiguration[]) => { useAppStore.setState({ mcpServers: servers }); return { ok: true } })
const saveCentral = vi.fn(async (configuration: ReturnType<typeof useAppStore.getState>['centralProviderConfiguration']) => { useAppStore.setState({ centralProviderConfiguration: configuration }); return { ok: true } })
beforeEach(async () => {
  saveServers.mockClear(); saveCentral.mockClear(); vi.mocked(sendToCEF).mockReset(); buildFeatures.COMPUTER_USE_ENABLED = true
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
  await click('+ Add server')
  await click('Manual form')
  const inputs = () => [...host.querySelectorAll('input:not([type=checkbox])')] as HTMLInputElement[]
  await type(inputs()[0], 'docs')
  await type(inputs()[1], 'docs-server')
  await click('Save')
  expect(host.textContent).toContain('program path must be a full path')
  await type(inputs()[1], '/opt/docs/server')
  await click('+ Add argument')
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


it('imports additively and rejects conflicts without changing the source configuration', () => {
  const text = '{"mcp":{"docs":{"type":"remote","url":"https://example.com/mcp"},"uam-mcp":{"type":"builtin","enabled":false}}}'
  const imported = mergeMcpImport(text, [server])
  expect(imported).toHaveLength(2)
  expect(imported[0]).toEqual(server)
  expect(imported[1]).toMatchObject({ name: 'docs', transport: 'http' })
  expect(() => mergeMcpImport(toJsonText([server], false), [server])).toThrow('already exists')
  expect(() => mergeMcpImport('{"mcp":{"docs":{"type":"remote","url":"file:///etc/passwd"}}}', [server])).toThrow('http:// or https://')
})

it('previews an import without saving or disabling the built-in UAM service', async () => {
  await click('+ Add server')
  await click('Import configuration')
  await type(host.querySelector('textarea[aria-label="Import MCP configuration"]') as HTMLTextAreaElement, '{"mcp":{"docs":{"type":"remote","url":"https://example.com/mcp"},"uam-mcp":{"type":"builtin","enabled":false}}}')
  await click('Review import')
  expect(saveServers).not.toHaveBeenCalled()
  expect(host.textContent).toContain('Imported into the draft')
  await click('Save')
  expect(saveServers.mock.calls[0][0]).toHaveLength(2)
  expect(saveCentral).not.toHaveBeenCalled()
})

it('starts guided setup with a named connection and saves only after completing details', async () => {
  await click('+ Add server')
  await click('Step by step')
  await type(host.querySelector('input[placeholder="playwright"]') as HTMLInputElement, 'docs')
  await click('Next: connection details')
  expect(host.textContent).toContain('Step 2 of 2')
  expect(saveServers).not.toHaveBeenCalled()
  await type(host.querySelector('input[placeholder="/usr/local/bin/npx"]') as HTMLInputElement, '/opt/docs/server')
  await click('Save')
  expect(saveServers.mock.calls[0][0][1]).toMatchObject({ name: 'docs', command: '/opt/docs/server' })
})


it('round-trips same-name servers on different hosts without dropping either definition', () => {
  const remote = { ...server, id: 'remote', executionHostId: 'homelab' }
  expect(fromJsonText(toJsonText([server, remote], true), [server, remote]).servers).toEqual([server, remote])
  expect(fromJsonText(toJsonText([{ ...server, name: '__proto__' }], true), []).servers[0].name).toBe('__proto__')
})


it('preserves concurrent central instruction changes while saving MCP servers', async () => {
  let finish!: () => void
  saveServers.mockImplementationOnce(async servers => { await new Promise<void>(resolve => { finish = resolve }); useAppStore.setState({ mcpServers: servers }); return { ok: true } })
  await act(async () => (host.querySelector('input[aria-label="UAM MCP service enabled"]') as HTMLInputElement).click())
  await click('Save')
  await act(async () => {
    useAppStore.setState({ centralProviderConfiguration: { ...useAppStore.getState().centralProviderConfiguration, instructions: 'New instructions' } })
    finish()
  })
  expect(saveCentral.mock.calls[0][0]).toMatchObject({ instructions: 'New instructions', uamControlEnabled: false })
})

it.each(['import', 'json', 'manual'] as const)('preserves intentional empty arguments and order when saving through %s', async mode => {
  const args = ['--value', '', '--other', 'tail', '']
  if (mode === 'import') {
    await click('+ Add server')
    await click('Import configuration')
    await type(host.querySelector<HTMLTextAreaElement>('[aria-label="Import MCP configuration"]')!, JSON.stringify({ mcp: { empty: { type: 'local', command: ['/opt/mcp/tool', ...args] } } }))
    await click('Review import')
  } else if (mode === 'json') {
    await click('JSON')
    await type(host.querySelector<HTMLTextAreaElement>('[aria-label="MCP JSON"]')!, toJsonText([{ ...server, args }], true))
  } else {
    await click('+ Add server'); await click('Manual form')
    const inputs = [...host.querySelectorAll<HTMLInputElement>('input:not([type=checkbox])')]
    await type(inputs[0], 'empty'); await type(inputs[1], '/opt/mcp/tool')
    for (const [index, arg] of args.entries()) {
      await click('+ Add argument')
      await type(host.querySelector<HTMLInputElement>(`[aria-label="Argument ${index + 1}"]`)!, arg)
    }
  }
  await click('Save')
  expect(saveServers).toHaveBeenCalledTimes(1)
  expect(saveServers.mock.calls[0][0].at(-1)?.args).toEqual(args)
})

it.each([
  { field: 'workspace', value: 7, error: 'workspace must be text' },
  { field: 'enabled', value: 'false', error: 'enabled must be boolean' },
])('rejects malformed pasted $field before accepting a configuration', ({ field, value, error }) => {
  const text = JSON.stringify({ mcp: { malformed: { type: 'remote', url: 'https://example.test/mcp', [field]: value } } })
  expect(() => fromJsonText(text, [server])).toThrow(error)
  expect(saveServers).not.toHaveBeenCalled()
})

it.each([
  { field: 'workspace', value: 7, error: 'workspace must be text' },
  { field: 'host', value: false, error: 'host must be text' },
  { field: 'enabled', value: 'false', error: 'enabled must be boolean' },
  { field: 'sse', value: 'false', error: 'sse must be boolean' },
])('shows a validation error without saving malformed JSON $field', async ({ field, value, error }) => {
  await click('JSON')
  await type(host.querySelector<HTMLTextAreaElement>('[aria-label="MCP JSON"]')!, JSON.stringify({ mcp: { malformed: { type: 'remote', url: 'https://example.test/mcp', [field]: value } } }))
  await click('Save')
  expect(host.textContent).toContain(error)
  expect(saveServers).not.toHaveBeenCalled()
  expect(saveCentral).not.toHaveBeenCalled()
})


it.each([
  { entry: { type: 'remote', url: 'https://example.test/mcp', oauth: false }, error: 'OAuth settings need manual configuration' },
  { entry: { type: 'local', command: ['/opt/mcp/tool'], timeout: 1000 }, error: 'timeout settings need manual configuration' },
  { entry: { type: 'local', command: ['/opt/mcp/tool', '{env:SECRET}'] }, error: 'command substitutions need manual configuration' },
  { entry: { type: 'local', command: ['/opt/mcp/tool', '--token={file:/private/token}'] }, error: 'command substitutions need manual configuration' },
  { entry: { type: 'remote', url: 'https://example.test/{env:SECRET}' }, error: 'URL substitutions need manual configuration' },
  { entry: { type: 'remote', url: 'https://example.test/mcp', headers: { Authorization: 'Bearer {env:SECRET}' } }, error: 'substitutions need manual configuration' },
  { entry: { type: 'local', command: ['/opt/mcp/tool'], environment: { TOKEN: '{file:/private/token}' } }, error: 'substitutions need manual configuration' },
])('rejects unsupported import semantics: $error', ({ entry, error }) => {
  const text = JSON.stringify({ mcp: { unsupported: entry } })
  expect(() => fromJsonText(text, [server])).toThrow(error)
  expect(() => mergeMcpImport(text, [server])).toThrow(error)
  expect(server.environment).toEqual([{ name: 'TOKEN', environmentVariable: 'PW_TOKEN' }])
})

it('keeps supported root metadata, server scopes, SSE, literal secrets and whole env references', () => {
  const text = JSON.stringify({ $schema: 'https://example.test/config.schema.json', model: 'provider/model', mcp: { docs: { name: 'scoped', type: 'remote', url: 'https://example.test/mcp?token=literal-secret', sse: true, workspace: '/workspace', host: 'ssh', enabled: false, headers: { Authorization: '{env:SECRET}', Token: 'literal-secret' } } } })
  expect(mergeMcpImport(text, [server]).at(-1)).toMatchObject({ name: 'scoped', workspaceDirectory: '/workspace', executionHostId: 'ssh', transport: 'sse', enabled: false, url: 'https://example.test/mcp?token=literal-secret', headers: [{ name: 'Authorization', environmentVariable: 'SECRET' }, { name: 'Token', environmentVariable: '', value: 'literal-secret' }] })
})

it.each(['import', 'json'] as const)('keeps the draft and secrets unchanged after unsupported %s input', async mode => {
  const text = JSON.stringify({ mcp: { docs: { type: 'remote', url: 'https://example.test/mcp', oauth: { clientSecret: 'private-secret-value' } } } })
  if (mode === 'import') {
    await click('+ Add server'); await click('Import configuration')
    await type(host.querySelector<HTMLTextAreaElement>('[aria-label="Import MCP configuration"]')!, text)
    await click('Review import')
  } else {
    await click('JSON')
    await type(host.querySelector<HTMLTextAreaElement>('[aria-label="MCP JSON"]')!, text)
    await click('Save')
  }
  expect(host.querySelector('[role="status"]')?.textContent).toContain('OAuth settings need manual configuration')
  expect(host.querySelector('[role="status"]')?.textContent).not.toContain('private-secret-value')
  expect(useAppStore.getState().mcpServers).toEqual([server])
  expect(saveServers).not.toHaveBeenCalled()
  expect(saveCentral).not.toHaveBeenCalled()
})


it('shows native source paths and warnings beside import controls and accepts a global directory', async () => {
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: { servers: [{ ...server, id: 'docs', name: 'docs' }], sources: ['/config/opencode/opencode.jsonc'], warnings: ['legacy: OAuth settings need manual configuration'], requiresReview: true } })
  await click('+ Add server'); await click('Import configuration')
  const section = host.querySelector('[aria-label="MCP setup"]')!
  expect(section.textContent).toContain('file or directory')
  await type(section.querySelector<HTMLInputElement>('input')!, '/config/opencode')
  await click('Read OpenCode configuration')
  expect(sendToCEF).toHaveBeenCalledWith({ action: 'previewOpenCodeMcpImport', payload: { executionHostId: 'local', sourcePath: '/config/opencode' } })
  expect(section.textContent).toContain('/config/opencode/opencode.jsonc')
  expect(section.textContent).toContain('OAuth settings need manual configuration')
  expect(section.querySelector('[role="status"]')?.textContent).toContain('Loaded 1 servers')
  expect(button('Review import').disabled).toBe(false)
})

it('clears a previous native preview on read failure and blocks stale review', async () => {
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: { servers: [{ ...server, name: 'docs' }], sources: ['/config/opencode.jsonc'], warnings: [] } }).mockResolvedValueOnce({ ok: false, error: 'No configuration exists in the selected directory.' })
  await click('+ Add server'); await click('Import configuration'); await click('Read OpenCode configuration')
  expect(host.querySelector<HTMLTextAreaElement>('[aria-label="Import MCP configuration"]')!.value).toContain('docs')
  await click('Read OpenCode configuration')
  const section = host.querySelector('[aria-label="MCP setup"]')!
  expect(section.querySelector('[role="status"]')?.textContent).toContain('No configuration exists')
  expect(section.textContent).not.toContain('/config/opencode.jsonc')
  expect(section.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('')
  expect(button('Review import').disabled).toBe(true)
  expect(saveServers).not.toHaveBeenCalled()
})

it('distinguishes zero external servers and keeps warnings and source visible without a UAM-only preview', async () => {
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: { servers: [], sources: ['/config/opencode.jsonc'], warnings: ['legacy: executable unavailable'], requiresReview: true } })
  await click('+ Add server'); await click('Import configuration'); await click('Read OpenCode configuration')
  const section = host.querySelector('[aria-label="MCP setup"]')!
  expect(section.querySelector('[role="status"]')?.textContent).toContain('No external MCP servers found')
  expect(section.textContent).toContain('executable unavailable')
  expect(section.textContent).toContain('/config/opencode.jsonc')
  expect(section.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('')
  expect(button('Review import').disabled).toBe(true)
})

it.each([true, false])('shows Computer Use server build availability %s without a chat permission toggle', async enabled => {
  buildFeatures.COMPUTER_USE_ENABLED = enabled
  await act(async () => root.render(<McpServerSetup onBusyChange={() => {}} onDirtyChange={() => {}} />))
  expect(host.textContent).toContain(enabled ? 'Available in this build' : 'Unavailable in this build')
  expect(host.querySelector('[aria-label="UAM Computer Use for current chat"]')).toBeNull()
  expect(host.querySelectorAll('button[aria-label^="Delete"]')).toHaveLength(1)
})
