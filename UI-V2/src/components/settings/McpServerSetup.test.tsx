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
  useAppStore.setState({ computerUseEnabled: undefined, mcpServers: [server], sessions: [], activeSessionId: null, executionHosts: [], centralProviderConfiguration: { enabled: true, instructions: '', instructionFiles: [], skillDirectories: [], defaultAgentId: 'build', uamControlEnabled: true }, setMcpServers: saveServers, setCentralProviderConfiguration: saveCentral })
  host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host)
  await act(async () => root.render(<McpServerSetup onBusyChange={() => {}} onDirtyChange={() => {}} />))
})
afterEach(async () => { await act(async () => root.unmount()); host.remove() })
const button = (text: string) => [...document.querySelectorAll('button')].find(item => item.textContent?.trim() === text || item.getAttribute('aria-label') === text)!
async function click(text: string) { await act(async () => button(text).click()) }
async function type(field: HTMLInputElement | HTMLTextAreaElement, value: string) {
  await act(async () => {
    Object.getOwnPropertyDescriptor(field instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype, 'value')!.set!.call(field, value)
    field.dispatchEvent(new Event('input', { bubbles: true }))
  })
}

it('round-trips OpenCode-style JSON with the built-in UAM service and env references', () => {
  const text = toJsonText([server], false)
  expect(JSON.parse(text).mcp).toEqual({ 'uam-mcp': { type: 'builtin', enabled: false }, playwright: { uamId: server.id, type: 'local', command: ['/usr/local/bin/npx', '-y', '@playwright/mcp@latest'], environment: { TOKEN: '{env:PW_TOKEN}' }, enabled: true } })
  expect(fromJsonText(text, [server])).toEqual({ servers: [server], uamEnabled: false })
  const literal = fromJsonText('{"mcp":{"x":{"type":"remote","url":"http://main.homelab.com:9001/mcp?userToken=abc","headers":{"Authorization":"Bearer t"}}}}', [])
  expect(literal.servers[0]).toMatchObject({ transport: 'http', url: 'http://main.homelab.com:9001/mcp?userToken=abc', headers: [{ name: 'Authorization', environmentVariable: '', value: 'Bearer t' }] })
  expect(JSON.parse(toJsonText(literal.servers, true)).mcp.x.headers).toEqual({ Authorization: 'Bearer t' })
})

it('shows built-ins without delete buttons and deletes a user server only after save', async () => {
  expect(document.body.textContent).toContain('UAM MCP service')
  expect(document.body.textContent).toContain('UAM Computer Use')
  expect(document.querySelectorAll('button[aria-label^="Delete"]')).toHaveLength(1)
  await act(async () => document.querySelector<HTMLButtonElement>('button[aria-label="Delete playwright"]')!.click())
  expect(saveServers).not.toHaveBeenCalled()
  await click('Save')
  expect(saveServers).toHaveBeenCalledWith([], '*')
})

it('adds a server through the form and blocks a relative program path', async () => {
  await click('Add server')
  await click('Manual form')
  const inputs = () => [...document.querySelectorAll('input:not([type=checkbox])')] as HTMLInputElement[]
  await type(inputs()[0], 'docs')
  await type(inputs()[1], 'docs-server')
  await click('Save')
  expect(document.body.textContent).toContain('program path must be a full path')
  await type(inputs()[1], '/opt/docs/server')
  await click('+ Add argument')
  await type(document.querySelector('input[aria-label="Argument 1"]') as HTMLInputElement, '--stdio')
  await click('Save')
  expect(saveServers.mock.calls[0][0][1]).toMatchObject({ name: 'docs', transport: 'stdio', command: '/opt/docs/server', args: ['--stdio'] })
})

it('edits in JSON, toggles the built-in UAM service and saves both', async () => {
  await click('JSON')
  const area = document.querySelector('textarea[aria-label="MCP JSON"]') as HTMLTextAreaElement
  await type(area, area.value.replace('"enabled": true\n    },\n    "playwright"', '"enabled": false\n    },\n    "playwright"'))
  await click('Save')
  expect(saveServers).toHaveBeenCalledWith([server], '*')
  expect(saveCentral.mock.calls[0][0].uamControlEnabled).toBe(false)
  await type(area, '{ not json')
  await click('Form')
  expect(document.body.textContent).toContain('not valid')
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
  await click('Add server')
  await click('Import configuration')
  await type(document.querySelector('textarea[aria-label="Import MCP configuration"]') as HTMLTextAreaElement, '{"mcp":{"docs":{"type":"remote","url":"https://example.com/mcp"},"uam-mcp":{"type":"builtin","enabled":false}}}')
  await click('Review import')
  expect(saveServers).not.toHaveBeenCalled()
  expect(document.body.textContent).toContain('Imported into the draft')
  await click('Save')
  expect(saveServers.mock.calls[0][0]).toHaveLength(2)
  expect(saveCentral).not.toHaveBeenCalled()
})

it('starts guided setup with a named connection and saves only after completing details', async () => {
  await click('Add server')
  await click('Step by step')
  await type(document.querySelector('input[placeholder="playwright"]') as HTMLInputElement, 'docs')
  await click('Next: connection details')
  expect(document.body.textContent).toContain('Step 2 of 2')
  expect(saveServers).not.toHaveBeenCalled()
  await type(document.querySelector('input[placeholder="/usr/local/bin/npx"]') as HTMLInputElement, '/opt/docs/server')
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
  await click('JSON')
  await type(document.querySelector<HTMLTextAreaElement>('[aria-label="MCP JSON"]')!, toJsonText(useAppStore.getState().mcpServers, false))
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
    await click('Add server')
    await click('Import configuration')
    await type(document.querySelector<HTMLTextAreaElement>('[aria-label="Import MCP configuration"]')!, JSON.stringify({ mcp: { empty: { type: 'local', command: ['/opt/mcp/tool', ...args] } } }))
    await click('Review import')
  } else if (mode === 'json') {
    await click('JSON')
    await type(document.querySelector<HTMLTextAreaElement>('[aria-label="MCP JSON"]')!, toJsonText([{ ...server, args }], true))
  } else {
    await click('Add server'); await click('Manual form')
    const inputs = [...document.querySelectorAll<HTMLInputElement>('input:not([type=checkbox])')]
    await type(inputs[0], 'empty'); await type(inputs[1], '/opt/mcp/tool')
    for (const [index, arg] of args.entries()) {
      await click('+ Add argument')
      await type(document.querySelector<HTMLInputElement>(`[aria-label="Argument ${index + 1}"]`)!, arg)
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
  await type(document.querySelector<HTMLTextAreaElement>('[aria-label="MCP JSON"]')!, JSON.stringify({ mcp: { malformed: { type: 'remote', url: 'https://example.test/mcp', [field]: value } } }))
  await click('Save')
  expect(document.body.textContent).toContain(error)
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
    await click('Add server'); await click('Import configuration')
    await type(document.querySelector<HTMLTextAreaElement>('[aria-label="Import MCP configuration"]')!, text)
    await click('Review import')
  } else {
    await click('JSON')
    await type(document.querySelector<HTMLTextAreaElement>('[aria-label="MCP JSON"]')!, text)
    await click('Save')
  }
  expect(document.querySelector('[role="status"]')?.textContent).toContain('OAuth settings need manual configuration')
  expect(document.querySelector('[role="status"]')?.textContent).not.toContain('private-secret-value')
  expect(useAppStore.getState().mcpServers).toEqual([server])
  expect(saveServers).not.toHaveBeenCalled()
  expect(saveCentral).not.toHaveBeenCalled()
})


it('shows native source paths and warnings beside import controls and accepts a global directory', async () => {
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: { servers: [{ ...server, id: 'docs', name: 'docs' }], sources: ['/config/opencode/opencode.jsonc'], warnings: ['legacy: OAuth settings need manual configuration'], requiresReview: true } })
  await click('Add server'); await click('Import configuration')
  const section = document.querySelector('[aria-label="MCP setup"]')!
  expect(section.textContent).toContain('file or directory')
  await type(section.querySelector<HTMLInputElement>('input')!, '/config/opencode')
  await click("Read this computer's OpenCode configuration")
  expect(sendToCEF).toHaveBeenCalledWith({ action: 'previewOpenCodeMcpImport', payload: { executionHostId: 'local', sourcePath: '/config/opencode' } })
  expect(section.textContent).toContain('/config/opencode/opencode.jsonc')
  expect(section.textContent).toContain('OAuth settings need manual configuration')
  expect(section.querySelector('[role="status"]')?.textContent).toContain('Loaded 1 server.')
  expect(button('Review import').disabled).toBe(false)
})

it('clears a previous native preview on read failure and blocks stale review', async () => {
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: { servers: [{ ...server, name: 'docs' }], sources: ['/config/opencode.jsonc'], warnings: [] } }).mockResolvedValueOnce({ ok: false, error: 'No configuration exists in the selected directory.' })
  await click('Add server'); await click('Import configuration'); await click("Read this computer's OpenCode configuration")
  expect(document.querySelector<HTMLTextAreaElement>('[aria-label="Import MCP configuration"]')!.value).toContain('docs')
  await click("Read this computer's OpenCode configuration")
  const section = document.querySelector('[aria-label="MCP setup"]')!
  expect(section.querySelector('[role="status"]')?.textContent).toContain('No configuration exists')
  expect(section.textContent).not.toContain('/config/opencode.jsonc')
  expect(section.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('')
  expect(button('Review import').disabled).toBe(true)
  expect(saveServers).not.toHaveBeenCalled()
})

it('distinguishes zero external servers and keeps warnings and source visible without a UAM-only preview', async () => {
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: { servers: [], sources: ['/config/opencode.jsonc'], warnings: ['legacy: executable unavailable'], requiresReview: true } })
  await click('Add server'); await click('Import configuration'); await click("Read this computer's OpenCode configuration")
  const section = document.querySelector('[aria-label="MCP setup"]')!
  expect(section.querySelector('[role="status"]')?.textContent).toContain('No external MCP servers found')
  expect(section.textContent).toContain('executable unavailable')
  expect(section.textContent).toContain('/config/opencode.jsonc')
  expect(section.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('')
  expect(button('Review import').disabled).toBe(true)
})

it.each([true, false])('shows Computer Use server build availability %s without a chat permission toggle', async enabled => {
  buildFeatures.COMPUTER_USE_ENABLED = enabled
  await act(async () => root.render(<McpServerSetup onBusyChange={() => {}} onDirtyChange={() => {}} />))
  expect(document.body.textContent).toContain(enabled ? 'Available in this build' : 'Unavailable in this build')
  expect(document.querySelector('[aria-label="UAM Computer Use for current chat"]')).toBeNull()
  expect(document.querySelectorAll('button[aria-label^="Delete"]')).toHaveLength(1)
})

it('switches Computer Use from the built-in row, disables it in flight and reports a failed change', async () => {
  useAppStore.setState({ computerUseEnabled: true })
  await act(async () => root.render(<McpServerSetup onBusyChange={() => {}} onDirtyChange={() => {}} />))
  const toggle = document.querySelector<HTMLInputElement>('input[aria-label="UAM Computer Use"]')!
  expect(toggle.checked).toBe(true)
  let finish!: (value: unknown) => void
  vi.mocked(sendToCEF).mockImplementationOnce((() => new Promise(resolve => { finish = resolve })) as never)
  await act(async () => toggle.click())
  expect(sendToCEF).toHaveBeenCalledWith({ action: 'setComputerUseSettings', payload: { computerUseGloballyEnabled: false } })
  expect(toggle.disabled).toBe(true)
  await act(async () => finish({ ok: false, error: 'Computer Use could not be turned off.' }))
  expect(toggle.disabled).toBe(false)
  expect(document.querySelector('[role="alert"]')?.textContent).toBe('Computer Use could not be turned off.')
  useAppStore.setState({ computerUseEnabled: false })
  await act(async () => root.render(<McpServerSetup onBusyChange={() => {}} onDirtyChange={() => {}} />))
  expect(document.body.textContent).toContain('Off')
  expect(document.body.textContent).toContain("Chats can't control the screen, mouse or keyboard.")
})

it('keeps provider override identity when a server is renamed in the JSON editor', async () => {
  const override = { providerId: 'codex-cli', executionHostId: 'local', serverId: server.id, enabled: false }
  useAppStore.setState(state => ({ centralProviderConfiguration: { ...state.centralProviderConfiguration, mcpOverrides: [override] } }))
  await click('JSON')
  const area = document.querySelector<HTMLTextAreaElement>('[aria-label="MCP JSON"]')!
  const doc = JSON.parse(area.value)
  expect(doc.mcp.playwright.uamId).toBe(server.id)
  doc.mcp['renamed-playwright'] = doc.mcp.playwright
  delete doc.mcp.playwright
  await type(area, JSON.stringify(doc))
  await click('Save')
  expect(saveServers).toHaveBeenCalledWith([{ ...server, name: 'renamed-playwright' }], '*')
  expect(useAppStore.getState().centralProviderConfiguration.mcpOverrides).toEqual([override])
})

it('keeps the known editor ID when workspace metadata changes and generates fresh imported IDs', () => {
  const doc = JSON.parse(toJsonText([server], true))
  doc.mcp.playwright.workspace = '/new-workspace'
  doc.mcp.playwright.name = 'renamed'
  expect(fromJsonText(JSON.stringify(doc), [server]).servers[0]).toMatchObject({ id: server.id, name: 'renamed', workspaceDirectory: '/new-workspace' })
  const imported = mergeMcpImport(JSON.stringify(doc), [server])
  expect(imported[0]).toEqual(server)
  expect(imported[1].id).not.toBe(server.id)
  expect(imported[1]).toMatchObject({ name: 'renamed', workspaceDirectory: '/new-workspace' })
})

it.each([null, 3, '', 'white space', 'id\u0000secret', 'x'.repeat(129)])('rejects invalid editor IDs without echoing their values (%j)', uamId => {
  expect(() => fromJsonText(JSON.stringify({ mcp: { docs: { type: 'remote', url: 'https://example.test/mcp', uamId } } }), [server])).toThrow('uamId must be a valid server identifier')
})

it('rejects duplicate editor IDs and does not adopt an unknown foreign ID', () => {
  const entry = { type: 'remote', url: 'https://example.test/mcp', uamId: 'foreign-id' }
  expect(() => fromJsonText(JSON.stringify({ mcp: { first: entry, second: entry } }), [server])).toThrow('duplicate uamId')
  expect(fromJsonText(JSON.stringify({ mcp: { docs: entry } }), [server]).servers[0].id).not.toBe('foreign-id')
})

it.each(['local', 'remote'])('rejects external %s definitions using the builtin key instead of dropping them', type => {
  const text = JSON.stringify({ mcp: {
    'uam-mcp': { type, command: ['/opt/mcp'], url: 'https://example.test/reserved' },
    docs: { type: 'remote', url: 'https://example.test/docs' },
  } })
  expect(() => fromJsonText(text, [server])).toThrow('reserved for the built-in UAM service')
  expect(() => mergeMcpImport(text, [server])).toThrow('reserved for the built-in UAM service')
})

it('keeps skipped-server warnings through editing, review and save', async () => {
  const warning = 'legacy: OAuth settings need manual configuration'
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: { servers: [{ ...server, id: 'docs', name: 'docs' }], sources: ['/config/opencode.jsonc'], warnings: [warning] } })
  await click('Add server'); await click('Import configuration')
  await click("Read this computer's OpenCode configuration")
  const area = document.querySelector<HTMLTextAreaElement>('[aria-label="Import MCP configuration"]')!
  await type(area, `${area.value}\n`)
  expect(document.querySelector('[aria-label="Import warnings"]')?.textContent).toContain(warning)
  await click('Review import')
  expect(document.querySelector('[aria-label="Import warnings"]')?.textContent).toContain(warning)
  await click('Save')
  expect(saveServers.mock.calls[0][0]).toHaveLength(2)
  expect(document.querySelector('[aria-label="Import warnings"]')?.textContent).toContain(warning)
})

it('clears earlier read warnings only on a new read or Cancel setup', async () => {
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: true, data: { servers: [], sources: ['/config/opencode.jsonc'], warnings: ['first: manual setup required'] } })
    .mockResolvedValueOnce({ ok: true, data: { servers: [], sources: ['/config/opencode.jsonc'], warnings: ['second: executable unavailable'] } })
  await click('Add server'); await click('Import configuration')
  await click("Read this computer's OpenCode configuration")
  expect(document.body.textContent).toContain('first: manual setup required')
  await click("Read this computer's OpenCode configuration")
  expect(document.body.textContent).not.toContain('first: manual setup required')
  expect(document.body.textContent).toContain('second: executable unavailable')
  await click('Cancel setup')
  expect(document.querySelector('[aria-label="Import warnings"]')).toBeNull()
})

it('explains why Save is disabled while server setup is incomplete', async () => {
  await click('Add server'); await click('Import configuration')
  expect(button('Save').disabled).toBe(true)
  expect(button('Save').title).toBe('Finish or cancel setup before saving.')
  expect(document.body.textContent).toContain('Finish or cancel setup before saving.')
})


it('identifies the OpenCode read action as local to this computer', async () => {
  await click('Add server'); await click('Import configuration')
  expect(button("Read this computer's OpenCode configuration")).toBeDefined()
})

it('uses host effective MCP servers and keeps global built-in controls out of host saves', async () => {
  const scopedServer = { ...server, id: 'host-server', name: 'host-only', args: [] }
  useAppStore.setState({ hostScopedSettings: true, executionHosts: [{ id: 'lab', label: 'Lab', transport: 'ssh', sshAlias: 'lab' }], hostScopedSettingsValues: { lab: { defaultNewChatProviderId: 'codex-cli', providerChatDefaults: {}, mcpServers: [scopedServer] } } })
  await act(async () => root.render(<McpServerSetup key="lab" scopeExecutionHostId="lab" onBusyChange={() => {}} onDirtyChange={() => {}} />))
  expect(host.textContent).toContain('host-only')
  expect(host.textContent).not.toContain('UAM Computer Use')
  expect(host.textContent).not.toContain('playwright')
  await click('JSON')
  const field = host.querySelector('textarea[aria-label="MCP JSON"]')!
  await type(field as HTMLTextAreaElement, toJsonText([scopedServer], false))
  await click('Save')
  expect(saveServers).not.toHaveBeenCalled()
  expect(saveCentral).not.toHaveBeenCalled()
  expect(host.textContent).toContain('Could not save MCP servers.')
})

it('does not publish a completed MCP save into a replacement scope', async () => {
  let finish!: (result: { ok: boolean }) => void
  saveServers.mockImplementationOnce(() => new Promise(resolve => { finish = resolve }))
  await click('JSON')
  await type(host.querySelector('textarea[aria-label="MCP JSON"]') as HTMLTextAreaElement, toJsonText([server], false))
  await act(async () => button('Save').click())
  await act(async () => root.render(<McpServerSetup key="replacement" scopeExecutionHostId="lab" onBusyChange={() => {}} onDirtyChange={() => {}} />))
  await act(async () => finish({ ok: true }))
  expect(saveCentral).not.toHaveBeenCalled()
  expect(host.textContent).not.toContain('Saved. Gemini')
})
