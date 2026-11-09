import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { AutomationApprovalNotice } from './AutomationApprovalNotice'
import type { Session } from '../../types/session'
import { sanitizeCppChat } from '../../store/cpp/sanitizers'
import { sessionFromCppChat } from '../../store/cpp/reconcile'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const send = vi.hoisted(() => vi.fn())
vi.mock('../../ipc/cefBridge', () => ({ isCompanionContext: () => false, sendToCEF: send }))
vi.mock('../../utils/appFailure', () => ({ reportAppFailure: vi.fn() }))

const session = { id: 'held', name: 'Held', automationPendingPrompt: '<b>Fix</b> the parser', automationMaxIterations: 3, automationGitPermission: 'commit', automationTokenName: 'n8n' } as Session

describe('Automation approval notice', () => {
  it('projects the native fields into the session', () => {
    const chat = sanitizeCppChat({ id: 'held', title: 'Held', providerId: 'codex-cli', automationTokenName: 'n8n', automationPendingPrompt: 'Fix', automationMaxIterations: 3, automationGitPermission: 'pr' })!
    const projected = sessionFromCppChat(chat, undefined, [{ id: 'codex-cli' }])
    expect(projected.automationPendingPrompt).toBe('Fix')
    expect(projected.automationTokenName).toBe('n8n')
    expect(sanitizeCppChat({ id: 'plain', title: 'Plain' })!.automationPendingPrompt).toBeUndefined()
  })

  it('renders the prompt as text and approves only on request', async () => {
    send.mockResolvedValue({ ok: true, data: {} })
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    act(() => root.render(<AutomationApprovalNotice session={session} />))
    expect(host.querySelector('b')).toBeNull()
    expect(host.textContent).toContain('<b>Fix</b> the parser')
    expect(send).not.toHaveBeenCalled()
    await act(async () => { host.querySelector('button')!.click(); await Promise.resolve() })
    expect(send).toHaveBeenCalledExactlyOnceWith({ action: 'approveAutomationChat', payload: { chatId: 'held' } })
    act(() => root.unmount()); host.remove()
  })
})
