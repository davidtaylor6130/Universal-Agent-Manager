import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { CustomAcpAgentSettings, parseArguments, parseEnvironment, type CustomAcpAgentView } from './CustomAcpAgentSettings'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const send = vi.hoisted(() => vi.fn())
const failure = vi.hoisted(() => vi.fn())
const notice = vi.hoisted(() => vi.fn())
vi.mock('../../ipc/cefBridge', async importOriginal => ({ ...await importOriginal<typeof import('../../ipc/cefBridge')>(), isCompanionContext: () => false, sendToCEF: send }))
vi.mock('../../utils/appFailure', () => ({ reportAppFailure: failure, reportAppNotice: notice }))

const view = (patch: Partial<CustomAcpAgentView> = {}): CustomAcpAgentView => ({
  configured: true, name: 'Fixture', displayName: 'Fixture', command: 'python3',
  arguments: ['-I', 'agent.py', 'two words'], environment: [{ name: 'AGENT_TOKEN', value: 'a=b' }],
  displayCommand: 'python3 -I agent.py "two words"', fingerprint: '0123456789abcdef', allowed: false, ...patch,
})

let host: HTMLDivElement, root: Root
const flush = () => act(async () => { await Promise.resolve(); await Promise.resolve(); await Promise.resolve() })
const button = (text: string) => [...document.querySelectorAll('button')].find(item => item.textContent?.trim() === text)!
const setValue = (element: HTMLInputElement | HTMLTextAreaElement, value: string) => act(() => {
  const prototype = element instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype
  Object.getOwnPropertyDescriptor(prototype, 'value')!.set!.call(element, value)
  element.dispatchEvent(new Event('input', { bubbles: true }))
})

beforeEach(async () => {
  send.mockReset(); failure.mockReset(); notice.mockReset()
  send.mockImplementation(async ({ action, payload }: { action: string; payload?: Record<string, unknown> }) => {
    if (action === 'getCustomAcpAgent') return { ok: true, data: view() }
    if (action === 'setCustomAcpAgent') return { ok: true, data: view({ ...(payload as Partial<CustomAcpAgentView>), fingerprint: 'fedcba9876543210', displayCommand: 'agent --acp', allowed: false }) }
    if (action === 'allowCustomAcpAgent') return { ok: true, data: view({ allowed: true }) }
    return { ok: false, error: 'unexpected' }
  })
  host = document.createElement('div'); document.body.append(host); root = createRoot(host)
  act(() => root.render(<CustomAcpAgentSettings />))
  await flush()
})

afterEach(() => { act(() => root.unmount()); host.remove() })

describe('Custom ACP agent settings', () => {
  it('parses one argument per line exactly and one NAME=value per line', () => {
    expect(parseArguments('--acp\n\n two words \r\n$HOME;id')).toEqual(['--acp', ' two words ', '$HOME;id'])
    expect(parseEnvironment('TOKEN=a=b\n\n DEBUG =1')).toEqual([{ name: 'TOKEN', value: 'a=b' }, { name: 'DEBUG', value: '1' }])
    expect(parseEnvironment('NO_VALUE')).toBe('Write each environment variable on its own line as NAME=value.')
  })

  it('labels the agent best effort and shows the exact command before it is allowed', () => {
    expect(host.textContent).toContain('Best effort')
    expect(host.querySelector('[aria-label="Exact command"]')?.textContent).toBe('python3 -I agent.py "two words"')
    expect(host.textContent).toContain('Also sets AGENT_TOKEN.')
    expect(button('Allow this command').disabled).toBe(false)
  })

  it('allows only the command it showed, by fingerprint', async () => {
    await act(async () => { button('Allow this command').click() })
    await flush()
    expect(send).toHaveBeenCalledWith({ action: 'allowCustomAcpAgent', payload: { fingerprint: '0123456789abcdef' } })
    expect(notice).toHaveBeenCalledWith('Custom ACP agent command allowed.', 'success')
    expect(host.textContent).not.toContain('Allow this command')
  })

  it('saves arguments as an array, keeps allowing blocked until saved, and reports bad environment lines', async () => {
    setValue(host.querySelector<HTMLInputElement>('#custom-acp-command')!, 'agent')
    setValue(host.querySelector<HTMLTextAreaElement>('#custom-acp-arguments')!, '--acp')
    expect(button('Allow this command').disabled).toBe(true)
    expect(host.textContent).toContain('Save your changes to review the new command.')
    setValue(host.querySelector<HTMLTextAreaElement>('#custom-acp-environment')!, 'BROKEN')
    await act(async () => { button('Save').click() })
    expect(failure).toHaveBeenCalledWith('Write each environment variable on its own line as NAME=value.', 'Your changes are kept.')
    expect(send).not.toHaveBeenCalledWith(expect.objectContaining({ action: 'setCustomAcpAgent' }))
    setValue(host.querySelector<HTMLTextAreaElement>('#custom-acp-environment')!, 'AGENT_TOKEN=x')
    await act(async () => { button('Save').click() })
    await flush()
    expect(send).toHaveBeenCalledWith({ action: 'setCustomAcpAgent', payload: { displayName: 'Fixture', command: 'agent', arguments: ['--acp'], environment: [{ name: 'AGENT_TOKEN', value: 'x' }] } })
    expect(host.querySelector('[aria-label="Exact command"]')?.textContent).toBe('agent --acp')
  })

  it('reports a save the app refused as one line in the notification system', async () => {
    send.mockImplementation(async ({ action }: { action: string }) => action === 'setCustomAcpAgent'
      ? { ok: false, error: 'Environment variable names must start with a letter or underscore and use only letters, digits and underscores.' }
      : { ok: true, data: view() })
    setValue(host.querySelector<HTMLInputElement>('#custom-acp-name')!, 'Renamed')
    await act(async () => { button('Save').click() })
    await flush()
    expect(failure).toHaveBeenCalledWith('Environment variable names must start with a letter or underscore and use only letters, digits and underscores.', 'Your changes are kept.')
    expect(host.querySelector<HTMLInputElement>('#custom-acp-name')!.value).toBe('Renamed')
  })
})
