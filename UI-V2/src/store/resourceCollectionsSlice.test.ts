import { beforeEach, describe, expect, it, vi } from 'vitest'
import { sanitizeCppAppState, sanitizeCppStatePatch } from './cpp/sanitizers'
import { useAppStore } from './useAppStore'
import { moveResourceToCollection } from '../components/sidebar/CollectionMenuItems'

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

  it('does not leave a resource in two collections when a move fails', async () => {
    useAppStore.setState({
      resourceCollections: [
        {
          id: 'old',
          name: 'Old',
          collapsed: false,
          references: [{ id: 'old-ref', type: 'workspace-folder', target: 'project', label: 'Project' }],
        },
        { id: 'new', name: 'New', collapsed: false, references: [] },
      ],
      addResourceReference: vi.fn(async (collectionId, type, target, label) => {
        const reference = { id: 'new-ref', type, target, label }
        useAppStore.setState((state) => ({
          resourceCollections: state.resourceCollections.map((collection) =>
            collection.id === collectionId
              ? { ...collection, references: [...collection.references, reference] }
              : collection
          ),
        }))
        return reference
      }),
      removeResourceReference: vi.fn(async () => false),
    })

    await moveResourceToCollection('new', 'workspace-folder', 'project', 'Project')

    const memberships = useAppStore.getState().resourceCollections.filter((collection) =>
      collection.references.some((reference) =>
        reference.type === 'workspace-folder' && reference.target === 'project'
      )
    )
    expect(memberships).toHaveLength(1)
  })
})
