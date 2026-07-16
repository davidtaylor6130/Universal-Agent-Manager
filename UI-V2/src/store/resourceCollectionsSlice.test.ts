import { beforeEach, describe, expect, it, vi } from 'vitest'
import { sanitizeCppAppState, sanitizeCppStatePatch } from './cpp/sanitizers'
import { useAppStore } from './useAppStore'

describe('resource collection state', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    delete (window as Window & { cefQuery?: Window['cefQuery'] }).cefQuery
    useAppStore.setState({ resourceCollections: [], folders: [], sessions: [], activeSessionId: null })
  })

  it('sanitizes collections in full state and patches', () => {
    const raw = [
      {
        id: ' work ',
        name: ' Work ',
        collapsed: true,
        references: [
          { id: ' folder ', type: 'workspace-folder', target: ' project ', label: ' Project ' },
          { id: 'bad', type: 'unknown', target: 'ignored' },
        ],
      },
      { id: '', name: 'Invalid' },
    ]

    const full = sanitizeCppAppState({ resourceCollections: raw })
    const patch = sanitizeCppStatePatch({ stateRevision: 2, resourceCollections: raw })

    expect(full?.resourceCollections).toEqual([{
      id: 'work',
      name: 'Work',
      collapsed: true,
      references: [{ id: 'folder', type: 'workspace-folder', target: 'project', label: 'Project' }],
    }])
    expect(patch?.resourceCollections).toEqual(full?.resourceCollections)
  })

  it('creates and assigns folders through the store contract', async () => {
    const collection = await useAppStore.getState().createResourceCollection(' Work ')
    const reference = await useAppStore.getState().addResourceReference(
      collection!.id,
      'workspace-folder',
      ' project ',
      ' Project ',
    )

    expect(collection?.name).toBe('Work')
    expect(reference).toMatchObject({ type: 'workspace-folder', target: 'project', label: 'Project' })
  })
})
