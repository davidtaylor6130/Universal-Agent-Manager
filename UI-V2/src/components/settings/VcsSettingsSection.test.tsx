import { act, createRef } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { VcsSettingsSection, type VcsSettingsSectionHandle } from './VcsSettingsSection'
import { useAppStore } from '../../store/useAppStore'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const initial = useAppStore.getState()
beforeEach(() => { useAppStore.setState({ ...initial, gitEnabled: true, svnEnabled: true, gitAutoWorktree: true, gitDefaultBaseBranch: '', gitPortMethod: 'patch', gitPullBeforeChat: false }, true) })
afterEach(() => { delete window.cefQuery; document.body.replaceChildren(); useAppStore.setState(initial, true) })
function input(element: HTMLInputElement, value: string) {
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(element, value)
  element.dispatchEvent(new Event('input', { bubbles: true }))
}
function button(host: HTMLElement, text: string) { return Array.from(host.querySelectorAll('button')).find((value) => value.textContent === text)! }
function mount() {
  const host = document.createElement('div'); document.body.appendChild(host)
  const root = createRoot(host); const ref = createRef<VcsSettingsSectionHandle>()
  act(() => root.render(<VcsSettingsSection ref={ref} />))
  return { host, root, ref }
}
describe('VCS defaults controls', () => {
  it('sends an explicit dirty VCS payload preserving Unicode and NBSP branch bytes', async () => {
    const requests: { action: string; payload: unknown }[] = []
    window.cefQuery = ({ request, onSuccess }) => { const message = JSON.parse(request); requests.push({ action: message.action, payload: message.payload }); onSuccess('{}') }
    const { host, root } = mount()
    try {
      const branch = '\u00a0feature/日本語\u00a0'
      act(() => input(host.querySelector<HTMLInputElement>('[aria-label="Default local Git base branch"]')!, branch))
      expect(requests).toHaveLength(0)
      await act(async () => button(host, 'Save VCS settings').click())
      expect(requests).toEqual([{ action: 'setVcsSettings', payload: { gitEnabled: true, svnEnabled: true, gitAutoWorktree: true, gitDefaultBaseBranch: branch, gitPortMethod: 'patch', gitPullBeforeChat: false } }])
      expect(button(host, 'Save VCS settings').disabled).toBe(true)
      expect(useAppStore.getState().gitDefaultBaseBranch).toBe(branch)
    } finally { act(() => root.unmount()) }
  })
  it('retains a failed-save draft and requires explicit discard before leaving', async () => {
    window.cefQuery = ({ onFailure }) => onFailure(500, 'Disk full')
    const { host, root, ref } = mount()
    try {
      act(() => host.querySelector<HTMLElement>('[aria-label="Git"] button:nth-child(2)')!.click())
      await act(async () => button(host, 'Save VCS settings').click())
      expect(useAppStore.getState().gitEnabled).toBe(true)
      expect(host.querySelector('[aria-label="Git"] button:nth-child(2)')?.getAttribute('aria-pressed')).toBe('true')
      expect(host.textContent).toContain('Your changes are retained')
      let left = false
      act(() => ref.current!.requestLeave(() => { left = true }))
      expect(left).toBe(false)
      expect(host.textContent).toContain('Discard unsaved VCS changes?')
      act(() => button(host, 'Discard and leave').click())
      expect(left).toBe(true)
    } finally { act(() => root.unmount()) }
  })
})
