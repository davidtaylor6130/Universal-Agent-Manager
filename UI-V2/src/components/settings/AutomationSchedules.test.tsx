import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { AutomationSchedules, describeTrigger, type AutomationSchedule } from './AutomationSchedules'
import { useAppStore } from '../../store/useAppStore'
import { reportAppFailure } from '../../utils/appFailure'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const send = vi.hoisted(() => vi.fn())
vi.mock('../../ipc/cefBridge', async importOriginal => ({ ...await importOriginal<typeof import('../../ipc/cefBridge')>(), isCompanionContext: () => false, sendToCEF: send }))
vi.mock('../../utils/appFailure', () => ({ reportAppFailure: vi.fn() }))

const schedule: AutomationSchedule = {
  id: 'as_1', name: 'Nightly triage', paused: false, trusted: false,
  trigger: { kind: 'weekly', days: [1, 2, 3, 4, 5], time: '09:00' }, missedPolicy: 'skip',
  request: { prompt: 'Summarise failures.', workspaceId: 'folder-a', providerId: 'codex-cli', title: 'Nightly triage', maxIterations: 3, gitPermission: 'commit' },
  nextRunMs: Date.UTC(2026, 10, 24, 9, 0), lastRunMs: Date.UTC(2026, 10, 23, 9, 0), lastResult: 'failed', lastError: 'That workspace no longer exists.', lastSkipped: 0,
}
let host: HTMLDivElement, root: Root, previous: ReturnType<typeof useAppStore.getState>

const flush = () => act(async () => { await Promise.resolve(); await Promise.resolve() })
const button = (text: string) => [...document.querySelectorAll('button')].find(item => item.textContent?.trim() === text || item.getAttribute('aria-label') === text)!
const setValue = (element: HTMLInputElement | HTMLTextAreaElement, value: string) => act(() => {
  const prototype = element instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype
  Object.getOwnPropertyDescriptor(prototype, 'value')!.set!.call(element, value)
  element.dispatchEvent(new Event('input', { bubbles: true }))
})

beforeEach(async () => {
  previous = useAppStore.getState()
  useAppStore.setState({
    folders: [{ id: 'folder-a', name: 'Parser', parentId: null, directory: '/work/parser', isExpanded: true, createdAt: new Date() }],
    providers: [
      { id: 'codex-cli', name: 'Codex', shortName: 'Codex', color: '', description: '', supportsStructured: true, structuredPermissionControl: 'uam' },
      { id: 'antigravity-cli', name: 'Antigravity', shortName: 'Antigravity', color: '', description: '', supportsStructured: true, structuredPermissionControl: 'provider' },
    ],
  })
  send.mockImplementation(async ({ action, payload }: { action: string; payload?: Record<string, unknown> }) => {
    if (action === 'getAutomationSchedules') return { ok: true, data: { schedules: [schedule] } }
    if (action === 'saveAutomationSchedule') return { ok: true, data: { schedule: { ...schedule, id: 'as_2', ...(payload?.schedule as object), nextRunMs: 0, lastRunMs: 0, lastResult: '' } } }
    if (action === 'setAutomationSchedulePaused') return { ok: true, data: { schedule: { ...schedule, paused: payload?.paused === true } } }
    return { ok: true, data: {} }
  })
  host = document.createElement('div'); document.body.append(host); root = createRoot(host)
  act(() => root.render(<AutomationSchedules />))
  await flush()
})

afterEach(() => {
  act(() => root.unmount()); host.remove()
  useAppStore.setState(previous, true)
  document.body.innerHTML = ''
  vi.clearAllMocks()
})

describe('Automation schedules', () => {
  it('describes triggers in plain words', () => {
    expect(describeTrigger({ kind: 'interval', minutes: 60 })).toBe('Every hour')
    expect(describeTrigger({ kind: 'interval', minutes: 180 })).toBe('Every 3 hours')
    expect(describeTrigger({ kind: 'weekly', days: [5, 1, 2, 3, 4], time: '09:00' })).toBe('Weekdays at 09:00')
    expect(describeTrigger({ kind: 'weekly', days: [0, 3], time: '18:30' })).toBe('Wed, Sun at 18:30')
  })

  it('lists each schedule with its trigger, workspace, trust, next run and last result', () => {
    expect(host.textContent).toContain('Nightly triage')
    expect(host.textContent).toContain('Weekdays at 09:00 · Parser · Waits for approval')
    expect(host.textContent).toContain('Next ')
    expect(host.textContent).toContain('failed: That workspace no longer exists.')
  })

  it('creates a schedule from the dialog and offers no provider that answers its own prompts', async () => {
    act(() => button('New schedule').click())
    expect((button('Save schedule') as HTMLButtonElement).disabled).toBe(true)
    setValue(document.querySelector<HTMLInputElement>('[aria-label="Schedule name"]')!, 'Morning check')
    setValue(document.querySelector<HTMLTextAreaElement>('[aria-label="Schedule prompt"]')!, 'Check the overnight build.')
    act(() => button('Provider').click())
    expect(document.body.textContent).not.toContain('Antigravity')
    act(() => button('Provider').click())
    expect((button('Save schedule') as HTMLButtonElement).disabled).toBe(false)
    await act(async () => { button('Save schedule').click(); await Promise.resolve() })
    await flush()
    expect(send).toHaveBeenCalledWith({ action: 'saveAutomationSchedule', payload: { id: '', schedule: {
      name: 'Morning check', trusted: false, trigger: { kind: 'interval', minutes: 1440 }, missedPolicy: 'skip',
      request: { prompt: 'Check the overnight build.', workspaceId: 'folder-a', providerId: 'codex-cli', modelId: '', title: 'Morning check', maxIterations: 5, gitPermission: 'commit' },
    } } })
    expect(document.querySelector('[aria-label="Schedule name"]')).toBeNull()
    expect(host.textContent).toContain('Morning check')
  })

  it('keeps the draft and reports one line when saving fails', async () => {
    send.mockImplementation(async ({ action }: { action: string }) => action === 'saveAutomationSchedule'
      ? { ok: false, error: 'That workspace no longer exists.' }
      : { ok: true, data: { schedules: [schedule] } })
    act(() => button('Edit Nightly triage').click())
    // Nothing changed yet, so there is nothing to save.
    expect((button('Save schedule') as HTMLButtonElement).disabled).toBe(true)
    setValue(document.querySelector<HTMLTextAreaElement>('[aria-label="Schedule prompt"]')!, 'Summarise every failure.')
    await act(async () => { button('Save schedule').click(); await Promise.resolve() })
    await flush()
    expect(reportAppFailure).toHaveBeenCalledWith('The schedule was not saved. That workspace no longer exists.', '')
    expect(document.querySelector<HTMLInputElement>('[aria-label="Schedule name"]')?.value).toBe('Nightly triage')
  })

  it('pauses with the row switch and deletes only after confirmation', async () => {
    await act(async () => { document.querySelector<HTMLInputElement>('[aria-label="Run Nightly triage on schedule"]')!.click(); await Promise.resolve() })
    await flush()
    expect(send).toHaveBeenCalledWith({ action: 'setAutomationSchedulePaused', payload: { id: 'as_1', paused: true } })
    expect(host.textContent).not.toContain('Next ')
    act(() => button('Delete Nightly triage').click())
    expect(send).not.toHaveBeenCalledWith(expect.objectContaining({ action: 'deleteAutomationSchedule' }))
    await act(async () => { button('Delete schedule').click(); await Promise.resolve() })
    await flush()
    expect(send).toHaveBeenCalledWith({ action: 'deleteAutomationSchedule', payload: { id: 'as_1' } })
    expect(host.textContent).not.toContain('Nightly triage')
  })

  it('shows why schedules are unavailable instead of an empty list', async () => {
    act(() => root.unmount())
    send.mockImplementation(async () => ({ ok: false, error: 'The automation schedule file could not be read.' }))
    root = createRoot(host)
    act(() => root.render(<AutomationSchedules />))
    await flush()
    expect(host.textContent).toContain('The automation schedule file could not be read.')
    expect(host.textContent).not.toContain('No schedules')
    expect((button('New schedule') as HTMLButtonElement).disabled).toBe(true)
  })
})
