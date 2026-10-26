import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { DataFolderSettingsSection } from './DataFolderSettingsSection'
import { retainUnconfirmedFolderChoice, retainUnconfirmedFolderRestart, type DataFolderSettings } from './dataFolderSettings'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const initial: DataFolderSettings = { currentFolder: '/disposable/source', pendingFolder: '', revision: 'app/initial', lastOperationId: '',
  transitionState: 'none', externalOverride: false, canChange: true, restartReady: false, message: 'Current folder remains active.' }
beforeEach(() => { retainUnconfirmedFolderChoice(null); retainUnconfirmedFolderRestart(null) })
afterEach(() => { delete window.cefQuery; document.body.replaceChildren(); retainUnconfirmedFolderChoice(null); retainUnconfirmedFolderRestart(null) })
function button(text: string) { return Array.from(document.querySelectorAll('button')).find(value => value.textContent === text)! }
async function mount() {
  const host = document.createElement('div'); document.body.appendChild(host)
  const root = createRoot(host)
  await act(async () => root.render(<DataFolderSettingsSection />))
  return { host, root }
}

describe('Application data-folder Settings', () => {
  it('shows the external override and disables folder mutation', async () => {
    const actions: string[] = []
    window.cefQuery = ({ request, onSuccess }) => { actions.push(JSON.parse(request).action); onSuccess(JSON.stringify({ ...initial, externalOverride: true, canChange: false })) }
    const { root } = await mount()
    try {
      expect(document.body.textContent).toContain('/disposable/source')
      expect(document.body.textContent).toContain('Set by the startup environment.')
      expect(button('Choose folder…').disabled).toBe(true)
      expect(actions).toEqual(['getDataFolderSettings'])
    } finally { act(() => root.unmount()) }
  })
  it('saves and cancels a pending folder without reporting relocation or requesting restart', async () => {
    let state = initial
    const requests: { action: string; payload: Record<string, unknown> }[] = []
    window.cefQuery = ({ request, onSuccess }) => {
      const message = JSON.parse(request); requests.push(message)
      if (message.action === 'getDataFolderSettings') onSuccess(JSON.stringify(state))
      else if (message.action === 'browseFolderDirectory') onSuccess(JSON.stringify({ selectedPath: '/disposable/destination' }))
      else {
        state = { ...initial, revision: `app/${message.action}`, lastOperationId: message.payload.operationId,
          pendingFolder: message.action === 'chooseDataFolder' ? message.payload.folder : '',
          transitionState: message.action === 'chooseDataFolder' ? 'awaitingMigration' : 'none', message: 'Transfer is held. The current folder remains in use.' }
        onSuccess(JSON.stringify({ committed: true, settings: state, error: '' }))
      }
    }
    const { root } = await mount()
    try {
      await act(async () => button('Choose folder…').click())
      expect(document.body.textContent).toContain('This saves a pending choice.')
      await act(async () => button('Save folder choice').click())
      expect(document.body.textContent).toContain('Saved folder choice')
      expect(document.body.textContent).toContain('/disposable/source')
      expect(button('Restart to use folder').disabled).toBe(true)
      expect(requests.find(value => value.action === 'chooseDataFolder')?.payload.expectedRevision).toBe(initial.revision)
      await act(async () => button('Cancel folder change').click())
      expect(document.body.textContent).not.toContain('Saved folder choice')
      expect(requests.some(value => value.action === 'restartApplication')).toBe(false)
    } finally { act(() => root.unmount()) }
  })
  it('retains a rejected folder draft and shows the native validation failure', async () => {
    window.cefQuery = ({ request, onSuccess }) => {
      const { action } = JSON.parse(request)
      onSuccess(JSON.stringify(action === 'getDataFolderSettings' ? initial : action === 'browseFolderDirectory'
        ? { selectedPath: '/occupied' } : { committed: false, settings: initial, error: 'Choose an empty folder.' }))
    }
    const { root } = await mount()
    try {
      await act(async () => button('Choose folder…').click())
      await act(async () => button('Save folder choice').click())
      expect(document.body.textContent).toContain('/occupied')
      expect(document.querySelector('[role="alertdialog"]')?.textContent).toContain('Choose an empty folder.')
      expect(document.body.textContent).not.toContain('Saved folder choice')
    } finally { act(() => root.unmount()) }
  })
  it('retains a lost save acknowledgement through remount until the matching native action is observed', async () => {
    let state = initial
    let operationId = ''
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const message = JSON.parse(request)
      if (message.action === 'getDataFolderSettings') onSuccess(JSON.stringify(state))
      else if (message.action === 'browseFolderDirectory') onSuccess(JSON.stringify({ selectedPath: '/alias/destination' }))
      else { operationId = message.payload.operationId; onFailure(500, 'Acknowledgement lost') }
    }
    const first = await mount()
    await act(async () => button('Choose folder…').click())
    await act(async () => button('Save folder choice').click())
    expect(document.body.textContent).toContain('Unconfirmed folder choice')
    act(() => first.root.unmount())
    const second = await mount()
    try {
      expect(document.body.textContent).toContain('Unconfirmed folder choice')
      expect(button('Choose folder…').disabled).toBe(true)
      state = { ...initial, pendingFolder: '/canonical/destination', revision: 'app/accepted', lastOperationId: operationId, transitionState: 'awaitingMigration' }
      await act(async () => button('Refresh folder state').click())
      expect(document.body.textContent).not.toContain('Unconfirmed folder choice')
      expect(document.body.textContent).toContain('Saved folder choice')
    } finally { act(() => second.root.unmount()) }
  })
  it('requests the guarded native restart only after native readiness and explicit confirmation', async () => {
    const ready: DataFolderSettings = { ...initial, pendingFolder: '/disposable/destination', transitionState: 'awaitingMigration', restartReady: true }
    const requests: { action: string; payload?: unknown }[] = []
    window.cefQuery = ({ request, onSuccess }) => {
      const message = JSON.parse(request); requests.push(message)
      onSuccess(JSON.stringify(message.action === 'getDataFolderSettings' ? ready : {}))
    }
    const { root } = await mount()
    try {
      await act(async () => button('Restart to use folder').click())
      expect(document.querySelector('[role="alertdialog"]')?.textContent).toContain('/disposable/source')
      expect(requests.map(value => value.action)).toEqual(['getDataFolderSettings'])
      await act(async () => button('Restart and transfer').click())
      expect(requests.at(-1)).toEqual({ action: 'restartToApplyDataFolder', payload: { expectedRevision: ready.revision }, requestId: expect.any(String) })
      expect(document.body.textContent).toContain('This does not confirm migration.')
      expect(button('Cancel folder change').disabled).toBe(true)
    } finally { act(() => root.unmount()) }
  })
  it('holds an uncertain restart through remount without replaying it', async () => {
    const ready: DataFolderSettings = { ...initial, pendingFolder: '/disposable/destination', transitionState: 'awaitingMigration', restartReady: true }
    const actions: string[] = []
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const message = JSON.parse(request); actions.push(message.action)
      if (message.action === 'getDataFolderSettings') onSuccess(JSON.stringify(ready))
      else onFailure(500, 'Restart acknowledgement lost')
    }
    const first = await mount()
    await act(async () => button('Restart to use folder').click())
    await act(async () => button('Restart and transfer').click())
    act(() => first.root.unmount())
    const second = await mount()
    try {
      expect(button('Restart to use folder').disabled).toBe(true)
      expect(button('Cancel folder change').disabled).toBe(true)
      expect(actions.filter(value => value === 'restartToApplyDataFolder')).toHaveLength(1)
      expect(document.body.textContent).toContain('until the application reopens')
    } finally { act(() => second.root.unmount()) }
  })

})
