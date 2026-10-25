import { act, createRef } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { VcsSettingsSection, type VcsSettingsSectionHandle } from './VcsSettingsSection'
import { useAppStore } from '../../store/useAppStore'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const initial = useAppStore.getState()
beforeEach(() => { useAppStore.setState({ ...initial, gitEnabled: true, svnEnabled: true, gitSnapshotSvn: true, gitSnapshotPlainFolders: true, gitAutoWorktree: true, gitDefaultBaseBranch: '', gitPortMethod: 'patch', gitPullBeforeChat: false, svnUpdateBeforeChat: false }, true) })
afterEach(() => { delete window.cefQuery; document.body.replaceChildren(); useAppStore.setState(initial, true) })
function input(element: HTMLInputElement, value: string) {
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(element, value)
  element.dispatchEvent(new Event('input', { bubbles: true }))
}
function button(host: HTMLElement, text: string) { return Array.from(host.querySelectorAll('button')).find((value) => value.textContent === text)! }
function toggle(host: HTMLElement, label: string) { return host.querySelector<HTMLInputElement>(`input[type="checkbox"][aria-label="${label}"]`)! }
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
      const branch = ' feature/日本語 '
      act(() => input(host.querySelector<HTMLInputElement>('[aria-label="Default local Git base branch"]')!, branch))
      expect(requests).toHaveLength(0)
      await act(async () => button(host, 'Save VCS settings').click())
      expect(requests).toEqual([{ action: 'setVcsSettings', payload: { gitEnabled: true, svnEnabled: true, gitSnapshotSvn: true, gitSnapshotPlainFolders: true, gitAutoWorktree: true, gitDefaultBaseBranch: branch, gitPortMethod: 'patch', gitPullBeforeChat: false, svnUpdateBeforeChat: false } }])
      expect(button(host, 'Save VCS settings').disabled).toBe(true)
      expect(useAppStore.getState().gitDefaultBaseBranch).toBe(branch)
    } finally { act(() => root.unmount()) }
  })
  it('retains a failed-save draft and requires explicit discard before leaving', async () => {
    window.cefQuery = ({ onFailure }) => onFailure(500, 'Disk full')
    const { host, root, ref } = mount()
    try {
      act(() => toggle(host, 'Enable Git').click())
      await act(async () => button(host, 'Save VCS settings').click())
      expect(useAppStore.getState().gitEnabled).toBe(true)
      expect(toggle(host, 'Enable Git').checked).toBe(false)
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

describe('separate Git and SVN sections', () => {
  it('stacks Git then SVN sections and uses one switch per boolean setting', () => {
    const { host, root } = mount()
    try {
      const sections = Array.from(host.querySelectorAll('section')).map((section) => section.getAttribute('aria-label'))
      expect(sections).toEqual(['Git', 'SVN', 'No VCS'])
      expect(host.querySelector('[aria-label="Git and SVN"]')).toBeNull()
      const git = host.querySelector<HTMLElement>('section[aria-label="Git"]')!
      const svn = host.querySelector<HTMLElement>('section[aria-label="SVN"]')!
      for (const label of ['Enable Git', 'Automatic isolation', 'Pull before first chat']) expect(git.contains(toggle(host, label)), label).toBe(true)
      expect(git.querySelector('[aria-label="Default local Git base branch"]')).not.toBeNull()
      expect(git.querySelector('[role="group"][aria-label="Port method"]')).not.toBeNull()
      for (const label of ['Enable SVN', 'Update before new chat']) expect(svn.contains(toggle(host, label)), label).toBe(true)
      expect(button(svn, 'Not supported').disabled).toBe(true)
      expect(host.querySelectorAll('input[type="checkbox"]')).toHaveLength(7)
      expect(Array.from(host.querySelectorAll('button')).filter((control) => control.textContent === 'Enabled' || control.textContent === 'Disabled')).toHaveLength(0)
      expect([toggle(host, 'Enable Git'), toggle(host, 'Automatic isolation'), toggle(host, 'Pull before first chat'), toggle(host, 'Enable SVN'), toggle(host, 'Update before new chat')].map((control) => control.checked))
        .toEqual([true, true, false, true, false])
      expect(Array.from(git.querySelectorAll('[role="group"][aria-label="Port method"] button')).map((control) => [control.textContent, control.getAttribute('aria-pressed')]))
        .toEqual([['Patch', 'true'], ['Merge', 'false'], ['Squash', 'false']])
    } finally { act(() => root.unmount()) }
  })

  it('disables only dependent controls and restores them when the parent switch returns', () => {
    const { host, root } = mount()
    try {
      const gitDependents = () => [toggle(host, 'Automatic isolation'), toggle(host, 'Pull before first chat'), host.querySelector<HTMLInputElement>('[aria-label="Default local Git base branch"]')!, ...Array.from(host.querySelectorAll<HTMLButtonElement>('[aria-label="Port method"] button'))]
      const snapshots = () => [toggle(host, 'Local snapshot Git for SVN'), toggle(host, 'Enable local Git files')]
      expect(gitDependents().every((control) => !control.disabled)).toBe(true)
      act(() => toggle(host, 'Automatic isolation').click())
      expect(toggle(host, 'Pull before first chat').disabled).toBe(true)
      expect(toggle(host, 'Enable Git').disabled).toBe(false)
      act(() => toggle(host, 'Automatic isolation').click())
      act(() => toggle(host, 'Enable Git').click())
      expect(gitDependents().every((control) => control.disabled)).toBe(true)
      expect(snapshots().every((control) => !control.disabled && control.checked)).toBe(true)
      expect(toggle(host, 'Enable SVN').disabled).toBe(false)
      expect(toggle(host, 'Update before new chat').disabled).toBe(false)
      act(() => toggle(host, 'Enable SVN').click())
      expect(toggle(host, 'Update before new chat').disabled).toBe(true)
      expect(snapshots().every((control) => !control.disabled && control.checked)).toBe(true)
      act(() => toggle(host, 'Enable Git').click())
      expect(gitDependents().every((control) => !control.disabled)).toBe(true)
      expect(useAppStore.getState().gitEnabled).toBe(true)
      expect(useAppStore.getState().svnEnabled).toBe(true)
    } finally { act(() => root.unmount()) }
  })

  it('keeps every switch draft after a failed save and disables switches while saving', async () => {
    let fail: (() => void) | undefined
    window.cefQuery = ({ onFailure }) => { fail = () => onFailure(500, 'Disk full') }
    const { host, root } = mount()
    try {
      act(() => { toggle(host, 'Pull before first chat').click(); toggle(host, 'Update before new chat').click() })
      act(() => { toggle(host, 'Automatic isolation').click() })
      await act(async () => button(host, 'Save VCS settings').click())
      expect(Array.from(host.querySelectorAll<HTMLInputElement>('input[type="checkbox"]')).every((control) => control.disabled)).toBe(true)
      await act(async () => fail?.())
      expect(host.textContent).toContain('Your changes are retained')
      expect([toggle(host, 'Automatic isolation').checked, toggle(host, 'Pull before first chat').checked, toggle(host, 'Update before new chat').checked]).toEqual([false, true, true])
      expect(toggle(host, 'Pull before first chat').disabled).toBe(true)
      expect(useAppStore.getState()).toMatchObject({ gitAutoWorktree: true, gitPullBeforeChat: false, svnUpdateBeforeChat: false })
    } finally { act(() => root.unmount()) }
  })
})

describe('SVN update before new chat', () => {
  it('defaults off and saves only after an explicit draft save', async () => {
    const requests: { action: string; payload: unknown }[] = []
    window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request)); onSuccess('{}') }
    const { host, root } = mount()
    try {
      const update = toggle(host, 'Update before new chat')
      expect(update.checked).toBe(false)
      act(() => update.click())
      expect(requests).toHaveLength(0)
      expect(useAppStore.getState().svnUpdateBeforeChat).toBe(false)
      await act(async () => button(host, 'Save VCS settings').click())
      expect(requests).toHaveLength(1)
      expect(requests[0]).toMatchObject({ action: 'setVcsSettings', payload: { svnUpdateBeforeChat: true, svnEnabled: true } })
      expect(useAppStore.getState().svnUpdateBeforeChat).toBe(true)
    } finally { act(() => root.unmount()) }
  })

  it('discards the update draft and disables editing when SVN is disabled', () => {
    const { host, root } = mount()
    try {
      const update = () => toggle(host, 'Update before new chat')
      act(() => update().click())
      act(() => button(host, 'Discard').click())
      expect(update().checked).toBe(false)
      act(() => toggle(host, 'Enable SVN').click())
      expect(update().disabled).toBe(true)
      expect(useAppStore.getState().svnUpdateBeforeChat).toBe(false)
    } finally { act(() => root.unmount()) }
  })

  it('retains the update draft after a save failure without changing the saved setting', async () => {
    window.cefQuery = ({ onFailure }) => onFailure(500, 'Disk full')
    const { host, root } = mount()
    try {
      act(() => toggle(host, 'Update before new chat').click())
      await act(async () => button(host, 'Save VCS settings').click())
      expect(useAppStore.getState().svnUpdateBeforeChat).toBe(false)
      expect(toggle(host, 'Update before new chat').checked).toBe(true)
      expect(host.textContent).toContain('Your changes are retained')
    } finally { act(() => root.unmount()) }
  })
})


describe('local snapshot Git settings', () => {
  it('places snapshot controls under SVN and No VCS', () => {
    const { host, root } = mount()
    try {
      const section = host.querySelector<HTMLElement>('section[aria-label="No VCS"]')
      expect(section).not.toBeNull()
      expect(host.querySelector('section[aria-label="SVN"]')!.contains(toggle(host, 'Local snapshot Git for SVN'))).toBe(true)
      expect(section!.contains(toggle(host, 'Local snapshot Git for SVN'))).toBe(false)
      expect(section!.contains(toggle(host, 'Enable local Git files'))).toBe(true)
      expect(toggle(host, 'Local snapshot Git for SVN').checked).toBe(true)
      expect(toggle(host, 'Enable local Git files').checked).toBe(true)
      expect(button(host, 'Save VCS settings').disabled).toBe(true)
    } finally { act(() => root.unmount()) }
  })

  it('saves each snapshot policy independently and discards unsaved changes', async () => {
    const requests: { action: string; payload: unknown }[] = []
    window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request)); onSuccess('{}') }
    const { host, root } = mount()
    try {
      expect(toggle(host, 'Enable local Git files')).not.toBeNull()
      act(() => toggle(host, 'Enable local Git files').click())
      expect(requests).toHaveLength(0)
      expect(toggle(host, 'Local snapshot Git for SVN').checked).toBe(true)
      await act(async () => button(host, 'Save VCS settings').click())
      expect(requests[0]).toMatchObject({ action: 'setVcsSettings', payload: { gitSnapshotSvn: true, gitSnapshotPlainFolders: false } })
      act(() => toggle(host, 'Local snapshot Git for SVN').click())
      act(() => button(host, 'Discard').click())
      expect(toggle(host, 'Local snapshot Git for SVN').checked).toBe(true)
      expect(toggle(host, 'Enable local Git files').checked).toBe(false)
      expect(button(host, 'Save VCS settings').disabled).toBe(true)
    } finally { act(() => root.unmount()) }
  })

  it.each([true, false])('saves and discards independent snapshot policies with Git disabled and SVN enabled=%s', async (svnEnabled) => {
    useAppStore.setState({ gitEnabled: false, svnEnabled, gitAutoWorktree: false })
    const requests: { action: string; payload: unknown }[] = []
    window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request)); onSuccess('{}') }
    const { host, root } = mount()
    try {
      expect(toggle(host, 'Local snapshot Git for SVN').disabled).toBe(false)
      expect(toggle(host, 'Enable local Git files').disabled).toBe(false)
      act(() => toggle(host, 'Local snapshot Git for SVN').click())
      expect(requests).toHaveLength(0)
      expect(useAppStore.getState()).toMatchObject({ gitEnabled: false, svnEnabled, gitSnapshotSvn: true, gitSnapshotPlainFolders: true })
      await act(async () => button(host, 'Save VCS settings').click())
      expect(requests).toHaveLength(1)
      expect(requests[0]).toMatchObject({ action: 'setVcsSettings', payload: { gitEnabled: false, svnEnabled, gitAutoWorktree: false, gitSnapshotSvn: false, gitSnapshotPlainFolders: true } })
      expect(useAppStore.getState()).toMatchObject({ gitEnabled: false, svnEnabled, gitSnapshotSvn: false, gitSnapshotPlainFolders: true })

      act(() => toggle(host, 'Enable local Git files').click())
      act(() => button(host, 'Discard').click())
      expect(toggle(host, 'Local snapshot Git for SVN').checked).toBe(false)
      expect(toggle(host, 'Enable local Git files').checked).toBe(true)
      expect(requests).toHaveLength(1)
      act(() => toggle(host, 'Enable local Git files').click())
      await act(async () => button(host, 'Save VCS settings').click())
      expect(requests).toHaveLength(2)
      expect(requests[1]).toMatchObject({ action: 'setVcsSettings', payload: { gitEnabled: false, svnEnabled, gitAutoWorktree: false, gitSnapshotSvn: false, gitSnapshotPlainFolders: false } })
      expect(useAppStore.getState()).toMatchObject({ gitEnabled: false, svnEnabled, gitSnapshotSvn: false, gitSnapshotPlainFolders: false })

      act(() => { toggle(host, 'Local snapshot Git for SVN').click(); toggle(host, 'Enable local Git files').click() })
      await act(async () => button(host, 'Save VCS settings').click())
      expect(requests).toHaveLength(3)
      expect(requests[2]).toMatchObject({ action: 'setVcsSettings', payload: { gitEnabled: false, svnEnabled, gitAutoWorktree: false, gitSnapshotSvn: true, gitSnapshotPlainFolders: true } })
      expect(useAppStore.getState()).toMatchObject({ gitEnabled: false, svnEnabled, gitSnapshotSvn: true, gitSnapshotPlainFolders: true })
      expect(button(host, 'Save VCS settings').disabled).toBe(true)
      expect(toggle(host, 'Automatic isolation').disabled).toBe(true)
      expect(toggle(host, 'Enable Git').checked).toBe(false)
    } finally { act(() => root.unmount()) }
  })

  it.each([true, false])('retains independent snapshot drafts after native rejection with Git enabled=%s', async (gitEnabled) => {
    useAppStore.setState({ gitEnabled })
    let fail: (() => void) | undefined
    window.cefQuery = ({ onFailure }) => { fail = () => onFailure(500, 'Disk full') }
    const { host, root } = mount()
    try {
      act(() => { toggle(host, 'Local snapshot Git for SVN').click(); toggle(host, 'Enable local Git files').click() })
      await act(async () => button(host, 'Save VCS settings').click())
      expect(toggle(host, 'Local snapshot Git for SVN').disabled).toBe(true)
      expect(toggle(host, 'Enable local Git files').disabled).toBe(true)
      expect(fail).toBeTypeOf('function')
      await act(async () => fail!())
      expect(useAppStore.getState()).toMatchObject({ gitEnabled, gitSnapshotSvn: true, gitSnapshotPlainFolders: true })
      expect(toggle(host, 'Local snapshot Git for SVN').disabled).toBe(false)
      expect(toggle(host, 'Enable local Git files').disabled).toBe(false)
      expect(toggle(host, 'Local snapshot Git for SVN').checked).toBe(false)
      expect(toggle(host, 'Enable local Git files').checked).toBe(false)
      expect(host.textContent).toContain('Your changes are retained')
      act(() => button(host, 'Discard').click())
      expect(toggle(host, 'Local snapshot Git for SVN').checked).toBe(true)
      expect(toggle(host, 'Enable local Git files').checked).toBe(true)
    } finally { act(() => root.unmount()) }
  })
})
