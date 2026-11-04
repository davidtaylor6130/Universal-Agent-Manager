import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { StorageCleanupSettingsSection } from './StorageCleanupSettingsSection'
import { useAppStore } from '../../store/useAppStore'
import { reportAppFailure } from '../../utils/appFailure'
vi.mock('../../utils/appFailure', async importOriginal => ({ ...await importOriginal<typeof import('../../utils/appFailure')>(), reportAppFailure: vi.fn() }));

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const initial = useAppStore.getState()
beforeEach(() => { useAppStore.setState({ ...initial, gitEnabled: true, svnEnabled: true, worktreeCleanupEnabled: true, worktreeRetentionDays: 14, backupLogRetentionDays: 14, httpCacheClearIntervalDays: 0 }, true) })
afterEach(() => { delete window.cefQuery; document.body.replaceChildren(); useAppStore.setState(initial, true) })
function input(element: HTMLInputElement, value: string) {
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(element, value)
  element.dispatchEvent(new Event('input', { bubbles: true }))
}
// NumberField commits on blur, so each edit is committed in its own act after the value lands.
function commit(element: HTMLInputElement, value: string) {
  act(() => input(element, value))
  act(() => element.focus())
  act(() => element.blur())
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
      for (const label of ['Worktree retention age', 'Backup log retention']) {
        const field = host.querySelector<HTMLInputElement>(`[aria-label="${label}"]`)!
        expect(field.classList.contains('uam-field')).toBe(true)
      }
      // Cache interval is only shown while HTTP cache clearing is on.
      expect(host.querySelector('[aria-label="HTTP cache interval"]')).toBeNull()
      act(() => host.querySelector<HTMLInputElement>('[aria-label="HTTP cache clearing"]')!.click())
      expect(host.querySelector<HTMLInputElement>('[aria-label="HTTP cache interval"]')!.classList.contains('uam-field')).toBe(true)
    } finally { act(() => root.unmount()) }
  })
  it('keeps independent endpoint acceptance when backup and cache persistence fails', async () => {
    const requests: { action: string; payload: unknown }[] = []
    window.cefQuery = ({ request, onSuccess, onFailure }) => { const message = JSON.parse(request); requests.push({ action: message.action, payload: message.payload }); if (message.action === 'setStorageSettings') onFailure(500, 'Disk full'); else onSuccess('{}') }
    const { host, root } = mount()
    try {
      commit(host.querySelector<HTMLInputElement>('[aria-label="Worktree retention age"]')!, '30')
      await act(async () => button(host, 'Save worktree settings').click())
      commit(host.querySelector<HTMLInputElement>('[aria-label="Backup log retention"]')!, '7')
      await act(async () => button(host, 'Save backup and cache policy').click())
      expect(requests).toEqual([{ action: 'setVcsSettings', payload: { gitEnabled: true, svnEnabled: true, worktreeCleanupEnabled: true, worktreeRetentionDays: 30 } }, { action: 'setStorageSettings', payload: { backupLogRetentionDays: 7, httpCacheClearIntervalDays: 0 } }])
      expect(useAppStore.getState().worktreeRetentionDays).toBe(30)
      expect(useAppStore.getState().backupLogRetentionDays).toBe(14)
      expect(host.querySelector<HTMLInputElement>('[aria-label="Backup log retention"]')!.value).toBe('7')
      expect(button(host, 'Save worktree settings').disabled).toBe(true)
      expect(button(host, 'Save backup and cache policy').disabled).toBe(false)
      expect(vi.mocked(reportAppFailure)).toHaveBeenCalledWith('Could not save backup and cache policy.', 'Your changes are retained.')
    } finally { act(() => root.unmount()) }
  })
  it('clamps retention values into range on commit without a native request', async () => {
    let requests = 0
    window.cefQuery = () => { requests++ }
    const { host, root } = mount()
    try {
      const worktree = host.querySelector<HTMLInputElement>('[aria-label="Worktree retention age"]')!
      commit(worktree, '-1')
      expect(worktree.value).toBe('1')
      commit(worktree, '3651')
      expect(worktree.value).toBe('3650')
      const backup = host.querySelector<HTMLInputElement>('[aria-label="Backup log retention"]')!
      commit(backup, '-4')
      expect(backup.value).toBe('0')
      expect(requests).toBe(0)
    } finally { act(() => root.unmount()) }
  })
})
