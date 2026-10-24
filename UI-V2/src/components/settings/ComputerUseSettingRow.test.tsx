import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { ComputerUseSettingRow } from './ComputerUseSettingRow'
import { sendToCEF } from '../../ipc/cefBridge'
import { useAppStore } from '../../store/useAppStore'

const features = vi.hoisted(() => ({ COMPUTER_USE_ENABLED: true }))
vi.mock('../../config/buildFeatures', () => features)
vi.mock('../../ipc/cefBridge', async importOriginal => ({ ...await importOriginal<typeof import('../../ipc/cefBridge')>(), sendToCEF: vi.fn() }))
;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

let host: HTMLDivElement
let root: Root
const receipt = (enabled: boolean, pending = false) => ({ ok: true, data: { computerUseGloballyEnabled: enabled, computerUseRevocationPending: pending } })
const toggle = () => host.querySelector<HTMLInputElement>('input[aria-label="UAM Computer Use"]')
const button = (label: string) => [...host.querySelectorAll('button')].find(item => item.textContent === label)!
beforeEach(async () => {
  vi.mocked(sendToCEF).mockReset()
  features.COMPUTER_USE_ENABLED = true
  useAppStore.setState({ computerUseEnabled: true, computerUseRevocationPending: false, computerUseSettingsStateRevision: 1, computerUseSettingsError: '', computerUseSettingsErrorStateRevision: undefined })
  host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host)
  await act(async () => root.render(<ComputerUseSettingRow />))
})
afterEach(async () => { await act(async () => root.unmount()); host.remove() })

it('checks unknown state without granting a target or requesting OS permission', async () => {
  await act(async () => useAppStore.setState({ computerUseEnabled: undefined, computerUseRevocationPending: undefined }))
  expect(toggle()).toBeNull()
  expect(host.textContent).toContain('Status not confirmed.')
  expect(sendToCEF).not.toHaveBeenCalled()
  vi.mocked(sendToCEF).mockResolvedValueOnce(receipt(false))
  await act(async () => button('Check status').click())
  expect(sendToCEF).toHaveBeenCalledExactlyOnceWith({ action: 'getComputerUseSettings' })
  expect(toggle()?.checked).toBe(false)
  expect(toggle()?.disabled).toBe(false)
})

it('waits for authoritative enablement and keeps target approval explicit', async () => {
  await act(async () => useAppStore.setState({ computerUseEnabled: false }))
  let finish!: (value: unknown) => void
  vi.mocked(sendToCEF).mockImplementationOnce((() => new Promise(resolve => { finish = resolve })) as never)
  await act(async () => toggle()!.click())
  expect(toggle()?.checked).toBe(false)
  expect(toggle()?.disabled).toBe(true)
  expect(host.textContent).toContain('Turning on…')
  expect(sendToCEF).toHaveBeenCalledExactlyOnceWith({ action: 'setComputerUseSettings', payload: { computerUseGloballyEnabled: true } })
  await act(async () => finish(receipt(true)))
  expect(toggle()?.checked).toBe(true)
  expect(toggle()?.disabled).toBe(false)
  expect(host.textContent).toContain('Each chat still needs an approved target.')
})

it('retains persisted Off after failed revocation and retries only the global disable', async () => {
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: false, error: 'Runtime controls are still stopping.' }).mockResolvedValueOnce(receipt(false, true))
  await act(async () => toggle()!.click())
  expect(useAppStore.getState()).toMatchObject({ computerUseEnabled: false, computerUseRevocationPending: true })
  expect(toggle()?.checked).toBe(false)
  expect(toggle()?.disabled).toBe(true)
  expect(host.textContent).toContain('Cleanup pending.')
  expect(host.textContent).not.toContain('UAM grants cleared')
  expect(host.querySelector('[role="alert"]')?.textContent).toContain('Runtime controls are still stopping.')
  // Pending ownership survives leaving and reopening the row.
  await act(async () => { root.unmount(); root = createRoot(host); root.render(<ComputerUseSettingRow />) })
  expect(button('Retry disabling')).toBeTruthy()
  vi.mocked(sendToCEF).mockResolvedValueOnce(receipt(false))
  await act(async () => button('Retry disabling').click())
  expect(vi.mocked(sendToCEF).mock.calls.map(([request]) => request)).toEqual([
    { action: 'setComputerUseSettings', payload: { computerUseGloballyEnabled: false } },
    { action: 'getComputerUseSettings' },
    { action: 'setComputerUseSettings', payload: { computerUseGloballyEnabled: false } },
  ])
  expect(host.textContent).toContain('Off. UAM grants cleared.')
  expect(toggle()?.disabled).toBe(false)
})

it('does not display Off when persistence failed and native state stayed enabled', async () => {
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: false, error: 'Failed to persist Computer Use Settings.' }).mockResolvedValueOnce(receipt(true))
  await act(async () => toggle()!.click())
  expect(toggle()?.checked).toBe(true)
  expect(host.textContent).toContain('Failed to persist')
  expect(host.textContent).not.toContain('UAM grants cleared')
})

it('keeps unknown transport outcomes distinct from cleared consent', async () => {
  vi.mocked(sendToCEF).mockRejectedValueOnce(Error('Connection lost.')).mockResolvedValueOnce({ ok: false, error: 'Read unavailable.' })
  await act(async () => toggle()!.click())
  expect(toggle()).toBeNull()
  expect(useAppStore.getState().computerUseEnabled).toBeUndefined()
  expect(useAppStore.getState().computerUseRevocationPending).toBeUndefined()
  expect(host.textContent).toContain('Status is not confirmed')
  vi.mocked(sendToCEF).mockResolvedValueOnce(receipt(false, true))
  await act(async () => button('Check status').click())
  expect(host.textContent).toContain('Cleanup pending.')
})

it('does not treat an old backend missing cleanup state as confirmed Off', async () => {
  await act(async () => useAppStore.setState({ computerUseEnabled: false, computerUseRevocationPending: undefined }))
  vi.mocked(sendToCEF).mockResolvedValue({ ok: true, data: { computerUseGloballyEnabled: false } })
  await act(async () => button('Check status').click())
  expect(toggle()).toBeNull()
  expect(host.textContent).not.toContain('UAM grants cleared')
})

it('allows a global Off request while state is unknown without offering enablement', async () => {
  await act(async () => useAppStore.setState({ computerUseEnabled: undefined, computerUseRevocationPending: undefined }))
  vi.mocked(sendToCEF).mockResolvedValueOnce(receipt(false))
  await act(async () => button('Turn off').click())
  expect(sendToCEF).toHaveBeenCalledExactlyOnceWith({ action: 'setComputerUseSettings', payload: { computerUseGloballyEnabled: false } })
  expect(toggle()?.checked).toBe(false)
})

it('hides mutations in a build without Computer Use', async () => {
  features.COMPUTER_USE_ENABLED = false
  await act(async () => root.render(<ComputerUseSettingRow />))
  expect(host.textContent).toContain('Unavailable in this build')
  expect(host.textContent).not.toContain('Saves immediately')
  expect(host.querySelectorAll('input, button')).toHaveLength(0)
  expect(sendToCEF).not.toHaveBeenCalled()
})

it('serializes a captured Off request across navigation between settings surfaces', async () => {
  let finish!: (value: unknown) => void
  vi.mocked(sendToCEF).mockImplementationOnce((() => new Promise(resolve => { finish = resolve })) as never)
  await act(async () => toggle()!.click())
  expect(sendToCEF).toHaveBeenCalledExactlyOnceWith({ action: 'setComputerUseSettings', payload: { computerUseGloballyEnabled: false } })
  await act(async () => { root.unmount(); root = createRoot(host); root.render(<ComputerUseSettingRow />) })
  expect(toggle()?.disabled).toBe(true)
  expect(host.textContent).toContain('Turning off and clearing UAM grants')
  await act(async () => toggle()!.click())
  expect(sendToCEF).toHaveBeenCalledTimes(1)
  vi.mocked(sendToCEF).mockResolvedValueOnce(receipt(false, true))
  await act(async () => finish({ ok: false, error: 'The owned runtime has not stopped.' }))
  expect(host.textContent).toContain('Cleanup pending.')
  expect(toggle()?.disabled).toBe(true)
  expect(host.querySelector('[role="alert"]')?.textContent).toBe('The owned runtime has not stopped.')
})

it('does not let an older readback clear a newer native pending observation', async () => {
  await act(async () => useAppStore.setState({ computerUseEnabled: undefined, computerUseRevocationPending: undefined }))
  let finish!: (value: unknown) => void
  vi.mocked(sendToCEF).mockImplementationOnce((() => new Promise(resolve => { finish = resolve })) as never)
  await act(async () => button('Check status').click())
  await act(async () => useAppStore.setState({ computerUseEnabled: false, computerUseRevocationPending: true }))
  await act(async () => finish(receipt(true)))
  expect(useAppStore.getState()).toMatchObject({ computerUseEnabled: false, computerUseRevocationPending: true })
  expect(toggle()?.disabled).toBe(true)
  expect(host.textContent).toContain('Cleanup pending.')
})

it('retains newer native authority even when its values return to the captured state', async () => {
  let finish!: (value: unknown) => void
  vi.mocked(sendToCEF).mockImplementationOnce((() => new Promise(resolve => { finish = resolve })) as never)
  await act(async () => toggle()!.click())
  await act(async () => useAppStore.setState({ computerUseEnabled: true, computerUseRevocationPending: false, computerUseSettingsStateRevision: 3 }))
  vi.mocked(sendToCEF).mockResolvedValueOnce(receipt(true))
  await act(async () => finish(receipt(false)))
  expect(toggle()?.checked).toBe(true)
  expect(useAppStore.getState().computerUseSettingsStateRevision).toBe(3)
})

it.each(['switch', 'Turn off'])('keeps On with per-chat cleanup truthful and permits explicit global Off via %s', async action => {
  await act(async () => useAppStore.setState({ computerUseRevocationPending: true }))
  expect(toggle()?.checked).toBe(true)
  expect(toggle()?.disabled).toBe(false)
  expect(host.textContent).toContain("On. A chat's controls are still stopping.")
  expect(host.textContent).toContain('Turn off for all chats to retry cleanup.')
  expect(host.textContent).not.toContain('in that chat')
  expect(host.textContent).not.toContain('before enabling')
  expect(button('Retry disabling')).toBeUndefined()
  expect(sendToCEF).not.toHaveBeenCalled()
  vi.mocked(sendToCEF).mockResolvedValueOnce(receipt(false))
  await act(async () => (action === 'switch' ? toggle()! : button(action)).click())
  expect(sendToCEF).toHaveBeenCalledExactlyOnceWith({ action: 'setComputerUseSettings', payload: { computerUseGloballyEnabled: false } })
})

it.each(['pending', 'unknown'])('removes superseded %s error feedback after a newer native observation, including remount', async outcome => {
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: false, error: 'Runtime controls are still stopping. Retry disabling.' })
    .mockResolvedValueOnce(outcome === 'pending' ? receipt(false, true) : { ok: false, error: 'Status unavailable.' })
  await act(async () => toggle()!.click())
  expect(host.querySelector('[role="alert"]')).not.toBeNull()
  await act(async () => { root.unmount(); root = createRoot(host); root.render(<ComputerUseSettingRow />) })
  expect(host.querySelector('[role="alert"]')).not.toBeNull()
  await act(async () => useAppStore.setState({ computerUseEnabled: false, computerUseRevocationPending: false, computerUseSettingsStateRevision: 2 }))
  expect(host.textContent).toContain('Off. UAM grants cleared.')
  expect(host.querySelector('[role="alert"]')).toBeNull()
  await act(async () => { root.unmount(); root = createRoot(host); root.render(<ComputerUseSettingRow />) })
  expect(host.querySelector('[role="alert"]')).toBeNull()
})

it('retains failure feedback for a native push received before its failure response', async () => {
  vi.mocked(sendToCEF).mockImplementationOnce(async () => {
    useAppStore.setState({ computerUseEnabled: false, computerUseRevocationPending: true, computerUseSettingsStateRevision: 2 })
    return { ok: false, error: 'Runtime controls are still stopping.' }
  }).mockResolvedValueOnce(receipt(false, true))
  await act(async () => toggle()!.click())
  expect(host.querySelector('[role="alert"]')?.textContent).toBe('Runtime controls are still stopping.')
})

it.each(['Retry disabling', 'Check status', 'Turn off'])('returns keyboard focus to the switch when %s recovery disappears', async label => {
  await act(async () => useAppStore.setState(label === 'Retry disabling'
    ? { computerUseEnabled: false, computerUseRevocationPending: true }
    : { computerUseEnabled: undefined, computerUseRevocationPending: undefined }))
  let finish!: (value: unknown) => void
  vi.mocked(sendToCEF).mockImplementationOnce((() => new Promise(resolve => { finish = resolve })) as never)
  button(label).focus()
  expect(document.activeElement).toBe(button(label))
  await act(async () => button(label).click())
  await act(async () => finish(receipt(false)))
  expect(document.activeElement).toBe(toggle())
})

it.each(['switch', 'Check status'])('does not steal focus moved elsewhere while a %s request is pending', async label => {
  if (label !== 'switch') await act(async () => useAppStore.setState({ computerUseEnabled: undefined, computerUseRevocationPending: undefined }))
  let finish!: (value: unknown) => void
  vi.mocked(sendToCEF).mockImplementationOnce((() => new Promise(resolve => { finish = resolve })) as never)
  const trigger = label === 'switch' ? toggle()! : button(label)
  trigger.focus()
  await act(async () => trigger.click())
  const elsewhere = document.createElement('button'); host.appendChild(elsewhere); elsewhere.focus()
  await act(async () => finish(receipt(false)))
  expect(document.activeElement).toBe(elsewhere)
})

it.each(['pending', 'unknown'])('moves switch focus to recovery when disabling ends %s', async outcome => {
  vi.mocked(sendToCEF).mockResolvedValueOnce({ ok: false, error: 'Runtime controls are still stopping.' })
    .mockResolvedValueOnce(outcome === 'pending' ? receipt(false, true) : { ok: false, error: 'Read unavailable.' })
  toggle()!.focus()
  expect(document.activeElement).toBe(toggle())
  await act(async () => toggle()!.click())
  expect(document.activeElement).toBe(button(outcome === 'pending' ? 'Retry disabling' : 'Turn off'))
  expect(sendToCEF).toHaveBeenNthCalledWith(1, { action: 'setComputerUseSettings', payload: { computerUseGloballyEnabled: false } })
  expect(sendToCEF).toHaveBeenCalledTimes(2)
})
