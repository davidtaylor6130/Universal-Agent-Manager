import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { sanitizeCppAppState, sanitizeCppStatePatch } from '../../store/cpp/sanitizers'
import { useAppStore } from '../../store/useAppStore'
import type { ResourceCollection } from '../../types/resourceCollection'
import { ResourceCollections } from './ResourceCollections'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function collection(id: string, references: ResourceCollection['references'] = []): ResourceCollection {
  return { id, name: id.toUpperCase(), collapsed: false, references }
}

function dataTransfer(initial: Record<string, string> = {}) {
  const data = new Map(Object.entries(initial))
  return {
    effectAllowed: '',
    types: [...data.keys()],
    getData: (type: string) => data.get(type) ?? '',
    setData: (type: string, value: string) => {
      data.set(type, value)
    },
  }
}

async function click(element: Element | null) {
  expect(element).toBeTruthy()
  await act(async () => {
    element!.dispatchEvent(new MouseEvent('click', { bubbles: true }))
  })
}

function input(element: HTMLInputElement, value: string) {
  act(() => {
    const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
    setter?.call(element, value)
    element.dispatchEvent(new Event('input', { bubbles: true }))
  })
}

describe('resource collection state', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    delete (window as Window & { cefQuery?: Window['cefQuery'] }).cefQuery
    useAppStore.setState({
      resourceCollections: [],
      folders: [],
      sessions: [],
      activeSessionId: null,
    })
  })

  it('sanitizes collections in full state and patches', () => {
    const raw = [
      {
        id: ' work ',
        name: ' Work ',
        collapsed: true,
        references: [
          { id: ' docs ', type: 'website', target: ' https://example.com ', label: ' Docs ' },
          { id: 'bad', type: 'unknown', target: 'ignored' },
        ],
      },
      { id: '', name: 'Invalid' },
    ]

    const full = sanitizeCppAppState({ resourceCollections: raw })
    const patch = sanitizeCppStatePatch({ stateRevision: 2, resourceCollections: raw })

    expect(full?.resourceCollections).toEqual([
      {
        id: 'work',
        name: 'Work',
        collapsed: true,
        references: [{ id: 'docs', type: 'website', target: 'https://example.com', label: 'Docs' }],
      },
    ])
    expect(patch?.resourceCollections).toEqual(full?.resourceCollections)
  })

  it('creates, adds, reorders, and removes through the store contract', async () => {
    const store = useAppStore.getState()
    const first = await store.createResourceCollection(' First ')
    const second = await store.createResourceCollection('Second')
    expect(first?.name).toBe('First')

    const one = await store.addResourceReference(first!.id, 'file', ' /tmp/one ', ' One ')
    const two = await store.addResourceReference(first!.id, 'file', '/tmp/two', 'Two')
    expect(await store.reorderResourceCollections([second!.id, first!.id])).toBe(true)
    expect(await store.reorderResourceReferences(first!.id, [two!.id, one!.id])).toBe(true)
    expect(await store.removeResourceReference(first!.id, one!.id)).toBe(true)

    const state = useAppStore.getState().resourceCollections
    expect(state.map((item) => item.id)).toEqual([second!.id, first!.id])
    expect(state[1].references.map((item) => item.label)).toEqual(['Two'])
  })
})

describe('ResourceCollections', () => {
  let host: HTMLDivElement
  let root: ReturnType<typeof createRoot>

  beforeEach(() => {
    vi.restoreAllMocks()
    delete (window as Window & { cefQuery?: Window['cefQuery'] }).cefQuery
    useAppStore.setState({
      resourceCollections: [],
      folders: [],
      sessions: [],
      activeSessionId: null,
    })
    host = document.createElement('div')
    document.body.appendChild(host)
    root = createRoot(host)
  })

  it('creates a collection, adds a resource, and removes it', async () => {
    act(() => root.render(<ResourceCollections />))
    await click(host.querySelector('[aria-label="New collection"]'))
    input(host.querySelector('[aria-label="Collection name"]') as HTMLInputElement, 'Work')
    await click(Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Create') ?? null)

    expect(useAppStore.getState().resourceCollections[0]?.name).toBe('Work')
    await click(host.querySelector('[aria-label="Actions for Work"]'))
    await click(Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Add resource')) ?? null)
    input(host.querySelector('[aria-label="Resource target"]') as HTMLInputElement, 'https://example.com')
    input(host.querySelector('[aria-label="Resource label"]') as HTMLInputElement, 'Docs')
    await click(Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Add resource') ?? null)

    expect(useAppStore.getState().resourceCollections[0].references[0]).toMatchObject({
      target: 'https://example.com',
      label: 'Docs',
    })
    await click(host.querySelector('[aria-label="Remove Docs"]'))
    expect(useAppStore.getState().resourceCollections[0].references).toEqual([])

    act(() => root.unmount())
    host.remove()
  })

  it('accepts dropped chats and reorders collections and references', async () => {
    useAppStore.setState({
      sessions: [{
        id: 'chat-1', name: 'Planning', viewMode: 'chat', folderId: null,
        createdAt: new Date(), updatedAt: new Date(), lastOpenedAt: new Date(),
      }],
      resourceCollections: [
        collection('first', [
          { id: 'one', type: 'file', target: '/tmp/one', label: 'One' },
          { id: 'two', type: 'file', target: '/tmp/two', label: 'Two' },
        ]),
        collection('second'),
      ],
    })
    act(() => root.render(<ResourceCollections />))

    const second = host.querySelector('[data-testid="resource-collection-second"]')!
    const chatDrop = new Event('drop', { bubbles: true, cancelable: true })
    Object.defineProperty(chatDrop, 'dataTransfer', { value: dataTransfer({ 'text/x-uam-chat-id': 'chat-1' }) })
    await act(async () => second.dispatchEvent(chatDrop))
    expect(useAppStore.getState().resourceCollections[1].references[0]).toMatchObject({
      type: 'chat', target: 'chat-1', label: 'Planning',
    })

    const first = host.querySelector('[data-testid="resource-collection-first"]')!
    const collectionDrag = dataTransfer()
    const dragStart = new Event('dragstart', { bubbles: true })
    Object.defineProperty(dragStart, 'dataTransfer', { value: collectionDrag })
    const collectionDrop = new Event('drop', { bubbles: true, cancelable: true })
    Object.defineProperty(collectionDrop, 'dataTransfer', { value: collectionDrag })
    act(() => {
      second.dispatchEvent(dragStart)
      first.dispatchEvent(collectionDrop)
    })
    expect(useAppStore.getState().resourceCollections.map((item) => item.id)).toEqual(['second', 'first'])

    const referenceDrag = dataTransfer()
    const referenceStart = new Event('dragstart', { bubbles: true })
    Object.defineProperty(referenceStart, 'dataTransfer', { value: referenceDrag })
    const referenceDrop = new Event('drop', { bubbles: true, cancelable: true })
    Object.defineProperty(referenceDrop, 'dataTransfer', { value: referenceDrag })
    act(() => {
      host.querySelector('[data-testid="resource-reference-two"]')!.dispatchEvent(referenceStart)
      host.querySelector('[data-testid="resource-reference-one"]')!.dispatchEvent(referenceDrop)
    })
    expect(useAppStore.getState().resourceCollections[1].references.map((item) => item.id)).toEqual(['two', 'one'])

    const moveTransfer = dataTransfer()
    const moveStart = new Event('dragstart', { bubbles: true })
    Object.defineProperty(moveStart, 'dataTransfer', { value: moveTransfer })
    const moveDrop = new Event('drop', { bubbles: true, cancelable: true })
    Object.defineProperty(moveDrop, 'dataTransfer', { value: moveTransfer })
    await act(async () => {
      host.querySelector('[data-testid="resource-reference-one"]')!.dispatchEvent(moveStart)
      host.querySelector('[data-testid="resource-collection-second"]')!.dispatchEvent(moveDrop)
    })
    expect(useAppStore.getState().resourceCollections[0].references.map((item) => item.label)).toEqual(['Planning', 'One'])
    expect(useAppStore.getState().resourceCollections[1].references.map((item) => item.label)).toEqual(['Two'])

    await act(async () => {})
    const movedReference = Array.from(host.querySelectorAll('[data-testid^="resource-reference-"]'))
      .find((element) => element.textContent?.includes('One'))
    act(() => {
      movedReference!.dispatchEvent(new MouseEvent('contextmenu', {
        bubbles: true,
        clientX: 20,
        clientY: 30,
      }))
    })
    await click(Array.from(host.querySelectorAll('[role="menu"] button')).find((button) => button.textContent === 'FIRST') ?? null)
    expect(useAppStore.getState().resourceCollections[1].references.map((item) => item.label)).toEqual(['Two', 'One'])

    act(() => root.unmount())
    host.remove()
  })
})
