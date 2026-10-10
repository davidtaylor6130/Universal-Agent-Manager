import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { VcsSettings } from './VcsSettings'
import { useAppStore } from '../../store/useAppStore'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const save = useAppStore.getState().setVcsSettings
beforeEach(() => { vi.restoreAllMocks(); delete window.cefQuery; useAppStore.setState({ gitEnabled: true, svnEnabled: true, setVcsSettings: save }) })

function render() {
  const host = document.createElement('div'); document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(<VcsSettings />))
  return { host, root }
}

describe('VCS preferences', () => {
  it('saves each system independently and rolls back failed persistence', async () => {
    const requests: unknown[] = []
    let fail = false
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const message = JSON.parse(request)
      requests.push(message)
      if (fail) onFailure(500, 'Disk full')
      else onSuccess('{}')
    }
    const { host, root } = render()
    try {
      await act(async () => host.querySelector<HTMLInputElement>('[aria-label="Enable Git"]')!.click())
      expect(useAppStore.getState()).toMatchObject({ gitEnabled: false, svnEnabled: true })
      expect(requests[0]).toMatchObject({ action: 'setVcsSettings', payload: { gitEnabled: false, svnEnabled: true } })
      fail = true
      await act(async () => host.querySelector<HTMLInputElement>('[aria-label="Enable SVN"]')!.click())
      expect(useAppStore.getState()).toMatchObject({ gitEnabled: false, svnEnabled: true })
      expect(host.querySelector('[role="alert"]')?.textContent).toBe('Could not save. Try again.')
      fail = false
      await act(async () => host.querySelector<HTMLInputElement>('[aria-label="Enable SVN"]')!.click())
      expect(useAppStore.getState().svnEnabled).toBe(false)
      expect(host.querySelector('[role="alert"]')).toBeNull()
    } finally { delete window.cefQuery; act(() => root.unmount()); host.remove() }
  })

  it('disables both controls during a save', async () => {
    let finish: ((response: string) => void) | undefined
    window.cefQuery = ({ onSuccess }) => { finish = onSuccess }
    const { host, root } = render()
    try {
      await act(async () => host.querySelector<HTMLInputElement>('[aria-label="Enable Git"]')!.click())
      expect([...host.querySelectorAll<HTMLInputElement>('input')].every(input => input.disabled)).toBe(true)
      expect(host.querySelector('[role="status"]')?.textContent).toBe('Saving…')
      await act(async () => finish!('{}'))
      expect([...host.querySelectorAll<HTMLInputElement>('input')].every(input => !input.disabled)).toBe(true)
    } finally { delete window.cefQuery; act(() => root.unmount()); host.remove() }
  })

  it('does not let an obsolete failure undo a newer setting', async () => {
    const complete: { ok: (text: string) => void; fail: (code: number, text: string) => void }[] = []
    window.cefQuery = ({ onSuccess, onFailure }) => { complete.push({ ok: onSuccess, fail: onFailure }) }
    const older = save({ gitEnabled: false, svnEnabled: true })
    const newer = save({ gitEnabled: true, svnEnabled: false })
    complete[1].ok('{}'); expect(await newer).toBe(true)
    complete[0].fail(500, 'Old failure'); expect(await older).toBe(false)
    expect(useAppStore.getState()).toMatchObject({ gitEnabled: true, svnEnabled: false })
    delete window.cefQuery
  })
})
