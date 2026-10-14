import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { StorageCleanupSettingsSection } from './StorageCleanupSettingsSection'
import { useAppStore } from '../../store/useAppStore'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const initial = useAppStore.getState()
beforeEach(() => { useAppStore.setState({ ...initial, gitEnabled: true, svnEnabled: true, worktreeCleanupEnabled: true, worktreeRetentionDays: 14, backupLogRetentionDays: 14, httpCacheClearIntervalDays: 0 }, true) })
afterEach(() => { delete window.cefQuery; document.body.replaceChildren(); useAppStore.setState(initial, true) })
function input(element: HTMLInputElement, value: string) {
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(element, value)
  element.dispatchEvent(new Event('input', { bubbles: true }))
}
function button(host: HTMLElement, text: string) { return Array.from(host.querySelectorAll('button')).find((value) => value.textContent === text)! }
function mount() {
  const host = document.createElement('div'); document.body.appendChild(host)
  const root = createRoot(host); act(() => root.render(<StorageCleanupSettingsSection />))
  return { host, root }
}
describe('Cleanup policy controls', () => {
  it('renders numeric inputs with the themed field class so text follows the theme tokens', () => {
    const { host, root } = mount()
    try {
      for (const label of ['Worktree retention age', 'Backup log retention', 'HTTP cache interval']) {
        const field = host.querySelector<HTMLInputElement>(`[aria-label="${label}"]`)!
        expect(field.classList.contains('uam-field')).toBe(true)
      }
    } finally { act(() => root.unmount()) }
  })
  it('keeps independent endpoint acceptance when backup and cache persistence fails', async () => {
    const requests: { action: string; payload: unknown }[] = []
    window.cefQuery = ({ request, onSuccess, onFailure }) => { const message = JSON.parse(request); requests.push({ action: message.action, payload: message.payload }); if (message.action === 'setStorageSettings') onFailure(500, 'Disk full'); else onSuccess('{}') }
    const { host, root } = mount()
    try {
      act(() => input(host.querySelector<HTMLInputElement>('[aria-label="Worktree retention age"]')!, '30'))
      await act(async () => button(host, 'Save worktree settings').click())
      act(() => input(host.querySelector<HTMLInputElement>('[aria-label="Backup log retention"]')!, '7'))
      await act(async () => button(host, 'Save backup and cache policy').click())
      expect(requests).toEqual([{ action: 'setVcsSettings', payload: { gitEnabled: true, svnEnabled: true, worktreeCleanupEnabled: true, worktreeRetentionDays: 30 } }, { action: 'setStorageSettings', payload: { backupLogRetentionDays: 7, httpCacheClearIntervalDays: 0 } }])
      expect(useAppStore.getState().worktreeRetentionDays).toBe(30)
      expect(useAppStore.getState().backupLogRetentionDays).toBe(14)
      expect(host.querySelector<HTMLInputElement>('[aria-label="Backup log retention"]')!.value).toBe('7')
      expect(button(host, 'Save worktree settings').disabled).toBe(true)
      expect(button(host, 'Save backup and cache policy').disabled).toBe(false)
      expect(host.textContent).toContain('Could not save backup and cache policy')
    } finally { act(() => root.unmount()) }
  })
  it('refuses negative and fractional retention values before either native request', async () => {
    let requests = 0
    window.cefQuery = () => { requests++ }
    const { host, root } = mount()
    try {
      for (const value of ['-1', '1.5', '0', '3651']) {
        act(() => input(host.querySelector<HTMLInputElement>('[aria-label="Worktree retention age"]')!, value))
        expect(button(host, 'Save worktree settings').disabled).toBe(true)
        act(() => button(host, 'Save worktree settings').click())
      }
      for (const value of ['-1', '1.5', '3651']) {
        act(() => input(host.querySelector<HTMLInputElement>('[aria-label="Backup log retention"]')!, value))
        expect(button(host, 'Save backup and cache policy').disabled).toBe(true)
        act(() => button(host, 'Save backup and cache policy').click())
      }
      expect(requests).toBe(0)
    } finally { act(() => root.unmount()) }
  })
})
