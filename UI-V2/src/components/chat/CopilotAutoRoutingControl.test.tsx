import { act, useRef, useState, type ReactNode } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { sendToCEF, type CEFResponse } from '../../ipc/cefBridge'
import type { AcpBinding, CopilotAutoRoutingProjection } from '../../store/cpp/types'
import { acpBindingFromCppChat } from '../../store/cpp/reconcile'
import { sanitizeCppChat } from '../../store/cpp/sanitizers'
import { CopilotAutoRoutingControl } from './CopilotAutoRoutingControl'
import { ViewportMenu } from '../ui'

vi.mock('../../ipc/cefBridge', async (importOriginal) => ({ ...await importOriginal<typeof import('../../ipc/cefBridge')>(), sendToCEF: vi.fn() }))
globalThis.IS_REACT_ACT_ENVIRONMENT = true

function projection(): CopilotAutoRoutingProjection {
  return {
    ownership: { chatId: 'captured-chat', nativeSessionId: 'native-owner', executionHostId: 'local', lifetime: 'public-token' },
    catalogStatus: 'ready', stateStatus: 'ready', eligibleTiers: { present: true, value: [
      { id: 'balance', displayName: 'Balanced', description: 'Balanced routing', type: 'auto', status: { enabled: true } },
      { id: ' vendor/Latency:Fast+V2 ', displayName: 'Low latency', description: 'Native latency preference', type: 'auto', status: { enabled: true } },
      { id: 'default', displayName: 'Native default tier', description: 'An opaque provider ID', type: 'auto', status: { enabled: true } },
      { id: 'efficiency', displayName: 'Efficiency', description: 'Efficient routing', type: 'auto', status: { enabled: false, message: 'Disabled by provider policy' } },
    ] }, providerDefault: { present: true, value: 'balance' }, currentModel: { present: true, value: 'auto' },
    effective: { present: true, value: 'balance' }, pending: { present: false }, activating: { present: false },
    requestPending: false, available: true, disabledCode: '', disabledReason: '', lastAckStatus: '', lastError: '',
  }
}

let root: Root
let host: HTMLDivElement
function render(value = projection()) {
  const chat = sanitizeCppChat({ id: value.ownership?.chatId ?? 'captured-chat', title: '', folderId: '', providerId: 'copilot-cli',
    createdAt: '', updatedAt: '', acpSession: { sessionId: value.ownership?.nativeSessionId ?? '', providerId: 'copilot-cli', protocolKind: 'copilot-sdk',
      running: true, lifecycleState: 'ready', processing: false, nativeModelOptions: { autoRouting: value } } })!
  act(() => root.render(<CopilotAutoRoutingControl acp={acpBindingFromCppChat(chat, undefined)} />))
}
function button(label: string) { return Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((element) => element.textContent === label)! }
function choose(label: string) {
  const trigger = host.querySelector<HTMLButtonElement>('[role="combobox"][aria-label="Auto routing"]')!
  act(() => trigger.click())
  const option = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((element) => element.querySelector('span.block')?.textContent === label)!
  expect(option).toBeDefined()
  act(() => option.click())
}

beforeEach(() => {
  window.history.replaceState({}, '', '/')
  window.cefQuery = vi.fn()
  vi.mocked(sendToCEF).mockReset().mockResolvedValue({ ok: true, data: { effectiveAutoTier: 'invented' } })
  host = document.createElement('div'); document.body.append(host); root = createRoot(host)
})
afterEach(() => { act(() => root.unmount()); host.remove(); window.cefQuery = undefined; window.history.replaceState({}, '', '/') })

describe('Copilot Auto routing native action control', () => {
  it('does nothing on mount and sends explicit Default null without claiming commitment from the ACK', async () => {
    const value = projection(); render(value)
    expect(sendToCEF).not.toHaveBeenCalled()
    choose('Default')
    expect(host.textContent).toContain('Sending preference…')
    expect(button('Refresh Auto routing').disabled).toBe(true)
    expect(host.textContent).toContain('Effective: Balanced')
    expect(sendToCEF).toHaveBeenCalledWith({ action: 'setNativeModelOption', payload: { ...value.ownership, optionId: 'autoRouting', value: null } }, false)
    await act(async () => {})
    expect(host.textContent).toContain('Effective: Balanced')
    expect(host.textContent).not.toContain('invented')
    expect(host.textContent).toContain('Preference request accepted.')
  })

  it('round-trips opaque tier IDs including an ID named default, without sending disabled choices', async () => {
    render(); choose('Low latency'); await act(async () => {})
    expect(vi.mocked(sendToCEF).mock.calls[0][0].payload).toMatchObject({ value: ' vendor/Latency:Fast+V2 ' })
    choose('Native default tier'); await act(async () => {})
    expect(vi.mocked(sendToCEF).mock.calls[1][0].payload).toMatchObject({ value: 'default' })
    act(() => host.querySelector<HTMLButtonElement>('[role="combobox"]')!.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    const disabled = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((option) => option.textContent?.includes('Efficiency'))!
    expect(disabled.disabled).toBe(true)
    expect(disabled.textContent).toContain('Disabled by provider policy')
    act(() => disabled.click())
    expect(sendToCEF).toHaveBeenCalledTimes(2)
  })

  it('separates committed, pending-null and activating states and displays native unavailability verbatim', () => {
    render({ ...projection(), pending: { present: true, value: null }, activating: { present: true, value: 'intelligence' },
      available: false, disabledCode: 'activating', disabledReason: 'Wait for the native Auto routing activation to finish.' })
    expect(host.textContent).toContain('Effective: Balanced')
    expect(host.textContent).toContain('Pending: Default')
    expect(host.textContent).toContain('Activating: intelligence')
    expect(host.textContent).toContain('Wait for the native Auto routing activation to finish.')
    expect(host.querySelector<HTMLButtonElement>('[role="combobox"]')!.disabled).toBe(true)
    expect(sendToCEF).not.toHaveBeenCalled()
  })

  it('refreshes unsupported native metadata explicitly with captured ownership and no mutation fields', async () => {
    const value = { ...projection(), catalogStatus: 'unsupported' as const, available: false,
      disabledCode: 'catalog_unsupported', disabledReason: 'This runtime does not advertise Auto routing.' }
    render(value)
    expect(host.querySelector<HTMLButtonElement>('[role="combobox"]')!.disabled).toBe(true)
    act(() => button('Refresh Auto routing').click()); await act(async () => {})
    expect(sendToCEF).toHaveBeenCalledWith({ action: 'refreshNativeModelOptions', payload: value.ownership }, false)
  })

  it('blocks both mutation and refresh while a provider request is pending', () => {
    render({ ...projection(), requestPending: true, available: false, disabledCode: 'request_pending', disabledReason: 'Wait for the native model option request to finish.' })
    expect(host.querySelector<HTMLButtonElement>('[role="combobox"]')!.disabled).toBe(true)
    expect(button('Refresh Auto routing').disabled).toBe(true)
    act(() => button('Refresh Auto routing').click())
    expect(sendToCEF).not.toHaveBeenCalled()
  })

  it('retires a delayed failure when ownership or native props change and does not carry local errors across snapshots', async () => {
    let finish!: (response: CEFResponse) => void
    vi.mocked(sendToCEF).mockReturnValueOnce(new Promise((resolve) => { finish = resolve }))
    render(); choose('Default')
    const next = projection(); next.ownership!.lifetime = 'replacement-token'; render(next)
    await act(async () => { finish({ ok: false, error: 'Old session failure' }) })
    expect(host.textContent).not.toContain('Old session failure')
    vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: false, error: 'Current refusal' })
    choose('Default'); await act(async () => {})
    expect(host.querySelector('.uam-notice[role="alert"]')?.textContent).toContain('Current refusal')
    render({ ...next, pending: { present: true, value: null } })
    expect(host.textContent).not.toContain('Current refusal')
  })

  it('guards standalone previews while retaining companion bridge support', async () => {
    window.cefQuery = undefined; render()
    expect(host.querySelector<HTMLButtonElement>('[role="combobox"]')!.disabled).toBe(true)
    act(() => button('Refresh Auto routing').click())
    expect(sendToCEF).not.toHaveBeenCalled()
    window.history.replaceState({}, '', '/companion'); render(); choose('Default'); await act(async () => {})
    expect(sendToCEF).toHaveBeenCalledTimes(1)
  })
})


function FollowupOptions({ children }: { children: ReactNode }) {
  const anchor = useRef<HTMLButtonElement>(null)
  const [open, setOpen] = useState(true)
  return <><button ref={anchor}>Options</button>{open && <ViewportMenu role="menu" aria-label="Composer Options"
    anchorRef={anchor} onRequestClose={() => setOpen(false)}>{children}<button role="menuitem">Other option</button></ViewportMenu>}</>
}

function renderFollowup(value = projection(), overrides: Partial<AcpBinding> = {}, disabled = false, options = false) {
  const binding = { sessionId: value.ownership?.nativeSessionId ?? '', providerId: 'copilot-cli', protocolKind: 'copilot-sdk',
    running: true, processing: false, compacting: false, backgroundTaskCount: 0, lifecycleState: 'ready',
    nativeModelOptions: { autoRouting: value }, ...overrides } as AcpBinding
  const control = <CopilotAutoRoutingControl acp={binding} disabled={disabled} />
  act(() => root.render(options ? <FollowupOptions>{control}</FollowupOptions> : control))
}

function followupSelect(label: string) {
  const trigger = document.body.querySelector<HTMLButtonElement>('[role="combobox"][aria-label="Auto routing"]')!
  act(() => { trigger.focus(); trigger.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })) })
  const option = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((entry) => entry.querySelector('span.block')?.textContent === label)
  if (option) act(() => { option.focus(); option.click() })
  return trigger
}

describe('Copilot Auto routing Claude follow-up regressions', () => {
  it.each(['preference', 'refresh'] as const)('preserves Options focus and Escape after %s admission', (kind) => {
    vi.mocked(sendToCEF).mockReturnValueOnce(new Promise(() => {}))
    renderFollowup(projection(), {}, false, true)
    const original = document.body.querySelector<HTMLButtonElement>('[role="combobox"]')!
    if (kind === 'preference') followupSelect('Balanced')
    else act(() => { button('Refresh Auto routing').focus(); button('Refresh Auto routing').click() })
    renderFollowup({ ...projection(), requestPending: true, available: false, disabledCode: 'request_pending', disabledReason: 'Wait for native confirmation.' }, {}, false, true)
    const menu = document.body.querySelector('[role="menu"][aria-label="Composer Options"]')!
    expect.soft(document.body.querySelector('[role="combobox"]')).toBe(original)
    expect.soft(menu.contains(document.activeElement)).toBe(true)
    expect.soft(document.activeElement?.matches(':disabled')).toBe(false)
    act(() => document.activeElement?.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    expect.soft(menu.contains(document.activeElement)).toBe(true)
    expect.soft(document.activeElement?.matches(':disabled')).toBe(false)
    act(() => document.activeElement?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.querySelector('[role="menu"][aria-label="Composer Options"]')).toBeNull()
    expect(document.activeElement?.textContent).toBe('Options')
  })

  it('keeps the in-flight guard across an unrelated same-owner push', async () => {
    let finish!: (value: CEFResponse) => void
    vi.mocked(sendToCEF).mockReturnValueOnce(new Promise((resolve) => { finish = resolve }))
    renderFollowup(); followupSelect('Balanced')
    renderFollowup({ ...projection(), providerDefault: { present: true, value: 'intelligence' } })
    followupSelect('Default')
    expect.soft(sendToCEF).toHaveBeenCalledTimes(1)
    expect.soft(host.textContent).toContain('Sending preference…')
    await act(async () => { finish({ ok: true }) })
  })

  it('retains admission feedback and reports a native unchanged ACK without changing effective', async () => {
    let finish!: (value: CEFResponse) => void
    vi.mocked(sendToCEF).mockReturnValueOnce(new Promise((resolve) => { finish = resolve }))
    renderFollowup(); followupSelect('Balanced')
    renderFollowup({ ...projection(), requestPending: true, available: false, disabledReason: 'Wait for native confirmation.' })
    await act(async () => { finish({ ok: true, data: { effectiveAutoTier: 'invented' } }) })
    expect.soft(host.textContent).toContain('Preference requested; waiting for Copilot.')
    renderFollowup({ ...projection(), lastAckStatus: 'unchanged' })
    expect.soft(host.textContent).toContain('No change; already Balanced.')
    expect(host.textContent).toContain('Effective: Balanced')
    expect(host.textContent).not.toContain('invented')
  })

  it('uses Button loading and preserves a focused Refresh through admission', () => {
    vi.mocked(sendToCEF).mockReturnValueOnce(new Promise(() => {}))
    renderFollowup()
    const refresh = button('Refresh Auto routing')
    act(() => { refresh.focus(); refresh.click() })
    expect.soft(refresh.getAttribute('aria-busy')).toBe('true')
    expect.soft(refresh.querySelector('.uam-btn__spinner')).not.toBeNull()
    renderFollowup({ ...projection(), requestPending: true, available: false, disabledReason: 'Wait for native confirmation.' })
    expect.soft(button('Refresh Auto routing')).toBe(refresh)
    expect(host.contains(document.activeElement)).toBe(true)
  })

  it.each(['request', 'activating'] as const)('announces %s progress with a working ring and readable reduced-motion text', (kind) => {
    document.documentElement.dataset.motion = 'off'
    try {
      renderFollowup({ ...projection(),
        ...(kind === 'request' ? { requestPending: true, available: false, disabledReason: 'Wait for native confirmation.' } : {}),
        ...(kind === 'activating' ? { activating: { present: true, value: 'intelligence' } } : {}),
      })
      const status = host.querySelector('[role="status"][aria-label="Auto routing progress"]')
      expect.soft(status?.getAttribute('aria-busy')).not.toBe('true')
      expect.soft(host.querySelector('[role="group"][aria-label="Copilot Auto routing"]')?.getAttribute('aria-busy')).toBe('true')
      expect.soft(host.querySelector('.uam-working-ring')).not.toBeNull()
      expect.soft(host.textContent).toContain(kind === 'request' ? 'Waiting for Copilot' : 'Activating: intelligence')
      expect(host.textContent).not.toContain('later Auto turn')
    } finally { delete document.documentElement.dataset.motion }
  })

  it('keeps an idle staged preference readable without working motion or a busy region', () => {
    renderFollowup({ ...projection(), pending: { present: true, value: null } })
    expect.soft(host.textContent).toContain('Pending: Default')
    expect.soft(host.querySelector('.uam-working-ring')).toBeNull()
    expect.soft(host.querySelector('[role="group"][aria-label="Copilot Auto routing"]')?.getAttribute('aria-busy')).not.toBe('true')
    expect.soft(host.querySelector<HTMLButtonElement>('[role="combobox"]')?.disabled).toBe(false)
    expect(button('Refresh Auto routing').disabled).toBe(false)
  })

  it('settles request motion when only an accepted staged preference remains', async () => {
    renderFollowup(); followupSelect('Default')
    renderFollowup({ ...projection(), pending: { present: true, value: null } })
    await act(async () => {})
    expect.soft(host.textContent).toContain('Pending: Default')
    expect.soft(host.textContent).toContain('Preference request accepted.')
    expect.soft(host.textContent).not.toContain('waiting for Copilot')
    expect.soft(host.querySelector('.uam-working-ring')).toBeNull()
    expect.soft(host.querySelector('[role="group"][aria-label="Copilot Auto routing"]')?.getAttribute('aria-busy')).not.toBe('true')
    expect.soft(host.querySelector<HTMLButtonElement>('[role="combobox"]')?.disabled).toBe(false)
    expect.soft(sendToCEF).toHaveBeenCalledTimes(1)
    expect(host.textContent).toContain('Effective: Balanced')
  })

  it('uses one stable Notice for an unchanged native error across same-owner pushes', () => {
    const value = { ...projection(), lastError: 'Native routing was refused.' }
    renderFollowup(value)
    const notice = host.querySelector('.uam-notice[role="alert"]')
    expect.soft(notice).not.toBeNull()
    renderFollowup({ ...value, providerDefault: { present: true, value: 'intelligence' } })
    expect.soft(host.querySelector('.uam-notice[role="alert"]')).toBe(notice)
    expect(host.querySelectorAll('[role="alert"]')).toHaveLength(1)
  })

  it.each([
    ['processing', { processing: true }, false],
    ['compaction', { compacting: true }, false],
    ['background work', { backgroundTaskCount: 1 }, false],
    ['lifecycle', { lifecycleState: 'waitingPermission' }, false],
    ['disabled composer', {}, true],
  ] as const)('blocks both actions for %s', (_name, overrides, disabled) => {
    renderFollowup(projection(), overrides, disabled)
    followupSelect('Default')
    act(() => button('Refresh Auto routing').click())
    expect(sendToCEF).not.toHaveBeenCalled()
  })

  it('opens the preference menu on a neutral non-action rather than Default reset', () => {
    renderFollowup()
    const trigger = host.querySelector<HTMLButtonElement>('[role="combobox"]')!
    act(() => { trigger.focus(); trigger.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })) })
    expect.soft(document.activeElement?.textContent).toBe('Choose preference')
    act(() => (document.activeElement as HTMLButtonElement)?.click())
    expect(sendToCEF).not.toHaveBeenCalled()
  })
})


describe('Copilot Auto routing Claude R2 boundaries', () => {
  it('keeps enabled Options focus and Escape after dismissing the native error Notice', () => {
    renderFollowup({ ...projection(), lastError: 'Native routing was refused.' }, {}, false, true)
    const dismiss = document.body.querySelector<HTMLButtonElement>('[aria-label="Dismiss Auto routing error"]')!
    act(() => { dismiss.focus(); dismiss.click() })
    const menu = document.body.querySelector('[role="menu"][aria-label="Composer Options"]')!
    expect.soft(document.body.querySelector('[role="alert"]')).toBeNull()
    expect.soft(menu.contains(document.activeElement)).toBe(true)
    expect.soft(document.activeElement).not.toBe(document.body)
    expect.soft(document.activeElement?.matches(':disabled')).toBe(false)
    act(() => document.activeElement?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.querySelector('[role="menu"][aria-label="Composer Options"]')).toBeNull()
    expect(document.activeElement?.textContent).toBe('Options')
  })

  it('retires a local waiting receipt when native routing fails', async () => {
    renderFollowup(); followupSelect('Low latency'); await act(async () => {})
    expect(host.textContent).toContain('Preference request accepted.')
    renderFollowup({ ...projection(), lastError: 'Native preference did not activate.' })
    expect.soft(host.textContent).not.toContain('waiting for Copilot.')
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Native preference did not activate.')
  })

  it('retires a local waiting receipt when native effective resolves the preference', async () => {
    renderFollowup(); followupSelect('Low latency'); await act(async () => {})
    renderFollowup({ ...projection(), pending: { present: true, value: ' vendor/Latency:Fast+V2 ' }, lastAckStatus: 'pending' })
    renderFollowup({ ...projection(), effective: { present: true, value: ' vendor/Latency:Fast+V2 ' }, lastAckStatus: 'pending' })
    expect.soft(host.textContent).toContain('Effective: Low latency')
    expect.soft(host.textContent).not.toContain('waiting for Copilot.')
    expect(host.textContent).not.toContain('Copilot accepted the preference request.')
  })

  it('retains the in-flight guard through native failure without resurrecting waiting on a late bridge ACK', async () => {
    let finish!: (value: CEFResponse) => void
    vi.mocked(sendToCEF).mockReturnValueOnce(new Promise((resolve) => { finish = resolve }))
    renderFollowup(); followupSelect('Low latency')
    const failed = { ...projection(), lastError: 'Native preference did not activate.' }
    renderFollowup(failed); followupSelect('Default')
    expect.soft(sendToCEF).toHaveBeenCalledTimes(1)
    expect.soft(host.textContent).toContain('Sending preference…')
    await act(async () => { finish({ ok: true }) })
    expect.soft(host.textContent).not.toContain('waiting for Copilot.')
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Native preference did not activate.')
  })

  it('does not present a retained unchanged ACK on a fresh Options mount', async () => {
    renderFollowup(); followupSelect('Balanced'); await act(async () => {})
    renderFollowup({ ...projection(), lastAckStatus: 'unchanged' })
    expect(host.textContent).toContain('No change; already Balanced.')
    act(() => root.render(<div />))
    renderFollowup({ ...projection(), lastAckStatus: 'unchanged' })
    expect(host.textContent).not.toContain('No change; already')
  })

  it('does not reinterpret a previous unchanged ACK after native effective changes', async () => {
    renderFollowup(); followupSelect('Balanced'); await act(async () => {})
    renderFollowup({ ...projection(), lastAckStatus: 'unchanged' })
    expect(host.textContent).toContain('No change; already Balanced.')
    renderFollowup({ ...projection(), lastAckStatus: 'unchanged', effective: { present: true, value: 'intelligence' } })
    expect.soft(host.textContent).toContain('Effective: intelligence')
    expect.soft(host.textContent).not.toContain('No change; already')
    renderFollowup({ ...projection(), lastAckStatus: 'unchanged' })
    expect(host.textContent).not.toContain('No change; already')
  })

  it('does not attribute a mismatched native unchanged ACK to the issued opaque preference', async () => {
    renderFollowup(); followupSelect('Low latency'); await act(async () => {})
    renderFollowup({ ...projection(), lastAckStatus: 'unchanged' })
    expect.soft(host.textContent).toContain('Effective: Balanced')
    expect(host.textContent).not.toContain('No change; already Balanced.')
  })

  it('keeps progress status unbusy while the control group reports native work', () => {
    renderFollowup({ ...projection(), requestPending: true, available: false })
    const status = host.querySelector('[role="status"][aria-label="Auto routing progress"]')!
    expect.soft(status.getAttribute('aria-busy')).not.toBe('true')
    expect.soft(status.textContent).toContain('Waiting for Copilot…')
    expect(host.querySelector('[role="group"][aria-label="Copilot Auto routing"]')?.getAttribute('aria-busy')).toBe('true')
  })
})


describe('Copilot Auto routing coalesced native observations', () => {
  it.each(['preference', 'refresh'] as const)('uses neutral %s acceptance when native work was never observed', async (kind) => {
    const value = { ...projection(), lastAckStatus: 'unchanged' as const }
    renderFollowup(value)
    if (kind === 'preference') followupSelect('Balanced')
    else act(() => button('Refresh Auto routing').click())
    await act(async () => {})
    // CEF can coalesce an admission snapshot; the final unchanged observation has no new work.
    renderFollowup({ ...value })
    expect.soft(host.textContent).not.toContain('waiting for Copilot.')
    expect.soft(host.textContent).toContain(kind === 'preference' ? 'Preference request accepted.' : 'Refresh request accepted.')
    expect.soft(host.textContent).toContain('Effective: Balanced')
    expect.soft(host.textContent).not.toContain('No change; already')
    expect(sendToCEF).toHaveBeenCalledTimes(1)
  })
})
