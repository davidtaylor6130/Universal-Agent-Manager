import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { PhoneTokenRegenerateRow } from './PhoneTokenRegenerateRow'

const send = vi.hoisted(() => vi.fn())
const notice = vi.hoisted(() => vi.fn())
const companion = vi.hoisted(() => ({ value: false }))
vi.mock('../../utils/appFailure', () => ({ reportAppNotice: notice, reportAppFailure: vi.fn() }))
vi.mock('../../ipc/cefBridge', () => ({ isCompanionContext: () => companion.value, sendToCEF: send }))

let root: Root | undefined
let host: HTMLDivElement | undefined
afterEach(() => {
  if (root) act(() => root!.unmount())
  host?.remove(); root = undefined; host = undefined
  companion.value = false
  vi.clearAllMocks()
})

function render(configured = true) {
  host = document.createElement('div'); document.body.append(host)
  root = createRoot(host)
  act(() => root!.render(<PhoneTokenRegenerateRow configured={configured} />))
  return host
}

const dialog = () => document.querySelector<HTMLElement>('[role="alertdialog"]')
const dialogButton = (label: string) => [...dialog()!.querySelectorAll('button')].find(button => button.textContent === label)!

describe('phone access token regeneration', () => {
  it('asks for confirmation naming the disconnect before regenerating', async () => {
    send.mockResolvedValue({ ok: true, data: {} })
    const host = render()
    act(() => host.querySelector('button')!.click())
    expect(send).not.toHaveBeenCalled()
    expect(dialog()!.textContent).toContain('Every paired phone and browser disconnects right away and must pair again')
    await act(async () => dialogButton('Regenerate token').click())
    expect(send).toHaveBeenCalledExactlyOnceWith({ action: 'regenerateCompanionToken' })
    expect(notice).toHaveBeenCalledWith('New phone access token issued. Pair your phones and browsers again.')
  })

  it('keeps the old token when cancelled and shows a failure inside the dialog', async () => {
    send.mockResolvedValue({ ok: false, error: 'Phone access is not configured.' })
    const host = render()
    act(() => host.querySelector('button')!.click())
    act(() => dialogButton('Cancel').click())
    expect(send).not.toHaveBeenCalled()
    act(() => host.querySelector('button')!.click())
    await act(async () => dialogButton('Regenerate token').click())
    expect(dialog()!.textContent).toContain('Phone access is not configured.')
    expect(notice).not.toHaveBeenCalled()
  })

  it('is unavailable when unconfigured and absent from the phone companion', () => {
    expect(render(false).querySelector('button')!.disabled).toBe(true)
    act(() => root!.unmount()); root = undefined
    companion.value = true
    expect(render().querySelector('button')).toBeNull()
  })
})
