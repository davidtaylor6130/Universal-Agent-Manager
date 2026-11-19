import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { AutomationStartQuestion } from './AutomationStartQuestion'
import type { Session } from '../../types/session'
import { sanitizeCppChat } from '../../store/cpp/sanitizers'
import { sessionFromCppChat } from '../../store/cpp/reconcile'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const companion = vi.hoisted(() => ({ value: false }))
vi.mock('../../ipc/cefBridge', async importOriginal => ({ ...await importOriginal<typeof import('../../ipc/cefBridge')>(), isCompanionContext: () => companion.value }))

const session = { id: 'held', name: 'Held', automationPendingPrompt: 'Fix the parser', automationMaxIterations: 3, automationGitPermission: 'commit', automationTokenName: 'n8n' } as Session
let host: HTMLDivElement | undefined, root: Root | undefined
const render = (onStart: () => void, error = '') => {
  host = document.createElement('div'); document.body.append(host); root = createRoot(host)
  act(() => root!.render(<AutomationStartQuestion session={session} open error={error} onStart={onStart} />))
}
afterEach(() => { if (root) act(() => root!.unmount()); host?.remove(); root = undefined; host = undefined; companion.value = false })

describe('Automation start question', () => {
  it('projects the native fields into the session', () => {
    const chat = sanitizeCppChat({ id: 'held', title: 'Held', providerId: 'codex-cli', automationTokenName: 'n8n', automationPendingPrompt: 'Fix', automationMaxIterations: 3, automationGitPermission: 'pr' })!
    const projected = sessionFromCppChat(chat, undefined, [{ id: 'codex-cli' }])
    expect(projected.automationPendingPrompt).toBe('Fix')
    expect(projected.automationTokenName).toBe('n8n')
    expect(sanitizeCppChat({ id: 'plain', title: 'Plain' })!.automationPendingPrompt).toBeUndefined()
  })

  it('asks in the question card and starts only on request', async () => {
    const onStart = vi.fn()
    render(onStart)
    expect(host!.querySelector('.qr-panel legend')?.textContent).toBe('Start this chat?')
    expect(host!.textContent).toContain('Up to 3 iterations, commit only.')
    expect(onStart).not.toHaveBeenCalled()
    await act(async () => { host!.querySelector('button')!.click(); await Promise.resolve() })
    expect(onStart).toHaveBeenCalledOnce()
  })

  it('shows a refused start beside the question', () => {
    render(vi.fn(), 'Worktrees are off.')
    expect(host!.querySelector('[role="alert"]')?.textContent).toBe('Worktrees are off.')
  })

  it('offers no action on the phone', () => {
    companion.value = true
    render(vi.fn())
    expect(host!.querySelector('button')).toBeNull()
    expect(host!.textContent).toContain('Start it on the computer.')
  })
})
