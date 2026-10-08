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

  it('does not overwrite a newer collection rename with an older acknowledgement', async () => {
    const acknowledge: Array<() => void> = []
    useAppStore.setState({ resourceCollections: [{ id: 'work', name: 'Original', collapsed: false, references: [] }] })
    window.cefQuery = vi.fn(({ request, onSuccess }) => {
      const { payload } = JSON.parse(request) as { payload: { name: string } }
      useAppStore.setState((state) => ({ resourceCollections: state.resourceCollections.map((collection) => ({ ...collection, name: payload.name })) }))
      acknowledge.push(() => onSuccess('{}'))
    })

    const first = useAppStore.getState().renameResourceCollection('work', 'First')
    const second = useAppStore.getState().renameResourceCollection('work', 'Second')
    acknowledge[1]()
    expect(await second).toBe(true)
    acknowledge[0]()
    await first

    expect(useAppStore.getState().resourceCollections[0].name).toBe('Second')
  })

  it('preserves a later authoritative name when a pending rename completes', async () => {
    let acknowledge!: () => void
    useAppStore.setState({ resourceCollections: [{ id: 'work', name: 'Original', collapsed: false, references: [] }] })
    window.cefQuery = vi.fn(({ onSuccess }) => { acknowledge = () => onSuccess('{}') })

    const rename = useAppStore.getState().renameResourceCollection('work', 'Pending')
    useAppStore.setState({ resourceCollections: [{ id: 'work', name: 'Changed elsewhere', collapsed: true, references: [] }] })
    acknowledge()
    await rename

    expect(useAppStore.getState().resourceCollections[0]).toMatchObject({ name: 'Changed elsewhere', collapsed: true })
  })

  it('keeps rename ownership independent for different collections and preserves pushed fields', async () => {
    const acknowledge: Array<() => void> = []
    useAppStore.setState({ resourceCollections: [
      { id: 'first', name: 'First', collapsed: false, references: [] },
      { id: 'second', name: 'Second', collapsed: false, references: [] },
    ] })
    window.cefQuery = vi.fn(({ onSuccess }) => { acknowledge.push(() => onSuccess('{}')) })
    const first = useAppStore.getState().renameResourceCollection('first', 'Renamed first')
    const second = useAppStore.getState().renameResourceCollection('second', 'Renamed second')
    useAppStore.setState((state) => ({ resourceCollections: state.resourceCollections.map((collection) => ({ ...collection, collapsed: true })) }))
    acknowledge[1]()
    acknowledge[0]()

    expect(await Promise.all([first, second])).toEqual([true, true])
    expect(useAppStore.getState().resourceCollections.map(({ name, collapsed }) => ({ name, collapsed }))).toEqual([
      { name: 'Renamed first', collapsed: true }, { name: 'Renamed second', collapsed: true },
    ])
  })

  it('preserves the native name when the newest rename fails before an older success returns', async () => {
    const complete: Array<() => void> = []
    useAppStore.setState({ resourceCollections: [{ id: 'work', name: 'Original', collapsed: false, references: [] }] })
    vi.spyOn(console, 'error').mockImplementation(() => {})
    window.cefQuery = vi.fn(({ onSuccess, onFailure }) => {
      complete.push(complete.length === 0 ? () => onSuccess('{}') : () => onFailure(500, 'Save failed.'))
    })
    const first = useAppStore.getState().renameResourceCollection('work', 'First')
    const second = useAppStore.getState().renameResourceCollection('work', 'Second')
    useAppStore.setState({ resourceCollections: [{ id: 'work', name: 'First', collapsed: false, references: [] }] })
    complete[1]()
    expect(await second).toBe(false)
    complete[0]()
    await first
    expect(useAppStore.getState().resourceCollections[0].name).toBe('First')
  })

  it('keeps both collections collapsed when native pushes outrun delayed toggle responses', async () => {
    let nativeCollections = [
      { id: 'misc', name: 'Misc AI', collapsed: false, references: [] },
      { id: 'generic', name: 'Generic', collapsed: false, references: [] },
    ]
    const acknowledge: Array<() => void> = []
    useAppStore.setState({ resourceCollections: nativeCollections })
    window.cefQuery = vi.fn(({ request, onSuccess }) => {
      const { payload } = JSON.parse(request) as { payload: { collectionId: string } }
      nativeCollections = nativeCollections.map((collection) =>
        collection.id === payload.collectionId
          ? { ...collection, collapsed: !collection.collapsed }
          : collection
      )
      useAppStore.setState({ resourceCollections: nativeCollections })
      acknowledge.push(() => onSuccess('{}'))
    })

    const collapseMisc = useAppStore.getState().toggleResourceCollection('misc')
    expect(useAppStore.getState().resourceCollections.map(({ id, collapsed }) => ({ id, collapsed }))).toEqual([
      { id: 'misc', collapsed: true },
      { id: 'generic', collapsed: false },
    ])

    const collapseGeneric = useAppStore.getState().toggleResourceCollection('generic')
    acknowledge.forEach((complete) => complete())
    await Promise.all([collapseMisc, collapseGeneric])

    expect(useAppStore.getState().resourceCollections.every((collection) => collection.collapsed)).toBe(true)
  })

  it('restores a collection when its latest native toggle fails', async () => {
    useAppStore.setState({
      resourceCollections: [{ id: 'misc', name: 'Misc AI', collapsed: false, references: [] }],
    })
    vi.spyOn(console, 'error').mockImplementation(() => {})
    window.cefQuery = vi.fn(({ onFailure }) => onFailure(500, 'Failed to persist collection state.'))

    const result = await useAppStore.getState().toggleResourceCollection('misc')

    expect(result).toBe(false)
    expect(useAppStore.getState().resourceCollections[0].collapsed).toBe(false)
  })

  it('restores the native baseline when overlapping toggles both fail', async () => {
    const reject: Array<() => void> = []
    useAppStore.setState({
      resourceCollections: [{ id: 'misc', name: 'Misc AI', collapsed: false, references: [] }],
    })
    vi.spyOn(console, 'error').mockImplementation(() => {})
    window.cefQuery = vi.fn(({ onFailure }) => {
      reject.push(() => onFailure(500, 'Failed to persist collection state.'))
    })

    const collapse = useAppStore.getState().toggleResourceCollection('misc')
    const expand = useAppStore.getState().toggleResourceCollection('misc')
    reject.forEach((fail) => fail())
    await Promise.all([collapse, expand])

    expect(useAppStore.getState().resourceCollections[0].collapsed).toBe(false)
  })

  it('matches the successful parity when one overlapping toggle fails', async () => {
    const complete: Array<() => void> = []
    useAppStore.setState({
      resourceCollections: [{ id: 'misc', name: 'Misc AI', collapsed: false, references: [] }],
    })
    vi.spyOn(console, 'error').mockImplementation(() => {})
    window.cefQuery = vi.fn(({ onSuccess, onFailure }) => {
      complete.push(complete.length === 0
        ? () => onSuccess('{}')
        : () => onFailure(500, 'Failed to persist collection state.'))
    })

    const collapse = useAppStore.getState().toggleResourceCollection('misc')
    const failedExpand = useAppStore.getState().toggleResourceCollection('misc')
    complete.forEach((finish) => finish())
    await Promise.all([collapse, failedExpand])

    expect(useAppStore.getState().resourceCollections[0].collapsed).toBe(true)
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
