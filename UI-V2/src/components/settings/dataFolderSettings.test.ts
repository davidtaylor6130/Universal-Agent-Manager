import { describe, expect, it } from 'vitest'
import { observesFolderChoice, parseDataFolderSettings, type DataFolderSettings, type UnconfirmedFolderChoice } from './dataFolderSettings'

const baseline: DataFolderSettings = { currentFolder: '/source', pendingFolder: '', revision: 'scope/initial', lastOperationId: '',
  transitionState: 'none', externalOverride: false, canChange: true, restartReady: false, message: 'Current folder remains active.' }
const attempt: UnconfirmedFolderChoice = { action: 'chooseDataFolder', folder: '/alias/destination', expectedRevision: baseline.revision, operationId: 'choose-1' }

describe('Data-folder native acknowledgement boundary', () => {
  it('rejects preview stubs, incomplete snapshots and unsupported restart authority', () => {
    for (const invalid of [null, {}, { ...baseline, revision: '' }, { ...baseline, restartReady: true },
      { ...baseline, transitionState: 'complete' }, { ...baseline, pendingFolder: '/other' },
      { ...baseline, transitionState: 'awaitingMigration' }, { ...baseline, externalOverride: true },
      { ...baseline, transitionState: 'publicationUnknown' }]) expect(parseDataFolderSettings(invalid)).toBeNull()
    expect(parseDataFolderSettings(baseline)).toEqual(baseline)
    const ready = { ...baseline, pendingFolder: '/destination', transitionState: 'awaitingMigration', restartReady: true }
    expect(parseDataFolderSettings(ready)).toEqual(ready)
    expect(parseDataFolderSettings({ ...ready, canChange: false })).toBeNull()
  })
  it('requires the exact originating action and a newer revision to reconcile a lost acknowledgement', () => {
    const observed: DataFolderSettings = { ...baseline, pendingFolder: '/canonical/destination', revision: 'scope/new',
      lastOperationId: attempt.operationId, transitionState: 'awaitingMigration' }
    expect(observesFolderChoice(observed, attempt)).toBe(true)
    expect(observesFolderChoice({ ...observed, lastOperationId: 'another-window' }, attempt)).toBe(false)
    expect(observesFolderChoice({ ...observed, revision: attempt.expectedRevision }, attempt)).toBe(false)
    expect(observesFolderChoice({ ...observed, transitionState: 'publicationUnknown', canChange: false }, attempt)).toBe(false)
  })
  it('never interprets an unchanged read or another operation as confirmed cancellation', () => {
    const cancel: UnconfirmedFolderChoice = { ...attempt, action: 'cancelDataFolderChoice', folder: '', operationId: 'cancel-1' }
    expect(observesFolderChoice(baseline, cancel)).toBe(false)
    expect(observesFolderChoice({ ...baseline, revision: 'scope/canceled', lastOperationId: cancel.operationId }, cancel)).toBe(true)
    expect(observesFolderChoice({ ...baseline, revision: 'scope/canceled', lastOperationId: cancel.operationId, externalOverride: true }, cancel)).toBe(false)
  })
})
