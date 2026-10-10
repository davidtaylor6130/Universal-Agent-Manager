import type { OrganizationChange, OrganizationUndo, ResourceMembership } from '../../types/organization'
import { sanitizeCustomIcon, type CustomIcon, type CustomIconInput, type CustomIconTarget } from '../../types/customIcon'
import { createRequestId, isCefContext, sendToCEF } from '../../ipc/cefBridge'
import type { ResourceCollection, ResourceReference, ResourceReferenceType } from '../../types/resourceCollection'
import type { ZustandGet, ZustandSet } from '../storeTypes'

let localId = 0
const pendingToggles = new Map<string, { baseline: boolean; count: number; successCount: number }>()

function nextLocalId(prefix: string) {
  localId += 1
  return `${prefix}-${localId}`
}

function memberships(collections: ResourceCollection[], type: ResourceReferenceType, target: string): ResourceMembership[] {
  return collections.flatMap((collection) => collection.references.flatMap((reference, index) => reference.type === type && reference.target === target
    ? [{ collectionId: collection.id, index, reference: { ...reference, customIcon: reference.customIcon ? { type: reference.customIcon.type, value: reference.customIcon.value } : null } }] : []))
}

function applyMemberships(collections: ResourceCollection[], change: Extract<OrganizationChange, { kind: 'memberships' }>): ResourceCollection[] {
  const next = collections.map((collection) => ({ ...collection, references: collection.references.filter((reference) => reference.type !== change.type || reference.target !== change.target) }))
  for (const item of change.replacement) {
    const collection = next.find((entry) => entry.id === item.collectionId)
    if (!collection) continue
    const { customIcon, ...reference } = item.reference
    collection.references.splice(item.index, 0, { ...reference, ...(customIcon ? { customIcon } : {}) })
  }
  return next
}

export function createResourceCollectionsSlice(set: ZustandSet, get: ZustandGet) {
  async function applyChange(change: OrganizationChange): Promise<boolean> {
    const state = get()
    const observedValue = (current: ReturnType<ZustandGet>) => {
      if (change.kind === 'pin') { const session = current.sessions.find((entry) => entry.id === change.chatId); return session ? session.isPinned ?? false : undefined }
      if (change.kind === 'folderOrder') return current.folders.map((entry) => entry.id)
      if (change.kind === 'referenceOrder') return current.resourceCollections.find((entry) => entry.id === change.collectionId)?.references.map((entry) => entry.id)
      if (change.kind === 'collectionOrder') return current.resourceCollections.map((entry) => entry.id)
      return memberships(current.resourceCollections, change.type, change.target)
    }
    if (JSON.stringify(observedValue(state)) !== JSON.stringify(change.expected)) return false
    const sequence = state.organizationMutationSequence + 1
    set({ organizationMutationSequence: sequence })
    if (isCefContext()) {
      const response = await sendToCEF(change.kind === 'pin'
        ? { action: 'setChatPinned', payload: { chatId: change.chatId, pinned: change.replacement, expectedPinned: change.expected } }
        : { action: 'applyOrganizationAction', payload: change })
      if (!response.ok) return false
    }
    if (get().organizationMutationSequence !== sequence) return false
    const latest = get()
    const observed = observedValue(latest)
    if (JSON.stringify(observed) !== JSON.stringify(change.expected) && JSON.stringify(observed) !== JSON.stringify(change.replacement)) return false
    set((current) => {
      if (change.kind === 'pin') return { sessions: current.sessions.map((entry) => entry.id === change.chatId ? { ...entry, isPinned: change.replacement } : entry) }
      if (change.kind === 'folderOrder') return { folders: change.replacement.flatMap((id) => current.folders.find((entry) => entry.id === id) ?? []) }
      if (change.kind === 'referenceOrder') return { resourceCollections: current.resourceCollections.map((collection) => collection.id === change.collectionId ? { ...collection, references: change.replacement.flatMap((id) => collection.references.find((entry) => entry.id === id) ?? []) } : collection) }
      if (change.kind === 'memberships') return { resourceCollections: applyMemberships(current.resourceCollections, change) }
      return { resourceCollections: change.replacement.flatMap((id) => current.resourceCollections.find((entry) => entry.id === id) ?? []) }
    })
    return true
  }
  return {
    resourceCollections: [] as ResourceCollection[],
    organizationUndo: null as OrganizationUndo | null,
    organizationMutationSequence: 0,
    applyOrganizationChange: applyChange,
    undoOrganization: async (): Promise<boolean> => {
      const undo = get().organizationUndo
      if (!undo || !await applyChange(undo.change)) return false
      if (get().organizationUndo === undo) set({ organizationUndo: null })
      return true
    },
    moveResourceToCollection: async (collectionId: string | null, type: ResourceReferenceType, target: string, label: string): Promise<boolean> => {
      const collections = get().resourceCollections
      const expected = memberships(collections, type, target)
      const destination = collectionId ? collections.find((entry) => entry.id === collectionId) : null
      if (collectionId && !destination) return false
      const existing = expected.find((entry) => entry.collectionId === collectionId)
      const replacement: ResourceMembership[] = destination ? [existing ?? {
        collectionId: destination.id, index: destination.references.length - expected.filter((entry) => entry.collectionId === destination.id).length,
        reference: expected[0]?.reference ?? { id: crypto.randomUUID(), type, target, label, customIcon: null },
      }] : []
      if (JSON.stringify(expected) === JSON.stringify(replacement)) return true
      const change: OrganizationChange = { kind: 'memberships', type, target, expected, replacement }
      if (!await applyChange(change)) return false
      set({ organizationUndo: { label: 'Move resource', change: { ...change, expected: replacement, replacement: expected } } })
      return true
    },

    setCustomIcon: async (targetType: CustomIconTarget, targetId: string, icon: CustomIconInput): Promise<boolean> => {
      if (icon?.type === 'text' && !sanitizeCustomIcon(icon)) return false
      if (isCefContext()) {
        const response = await sendToCEF({ action: 'setCustomIcon', payload: { targetType, targetId, icon }, requestId: createRequestId('setCustomIcon') })
        return response.ok
      }
      const customIcon: CustomIcon | undefined = icon === null ? undefined : icon.type === 'text' ? icon : {
        type: 'png', value: `${crypto.randomUUID()}.png`, dataUrl: `data:image/png;base64,${icon.base64}`,
      }
      if (targetType === 'workspace') {
        if (!get().folders.some((folder) => folder.id === targetId)) return false
        set((state) => ({ folders: state.folders.map((folder) => folder.id === targetId ? { ...folder, customIcon } : folder) }))
      } else if (targetType === 'host') {
        if (!get().executionHosts.some((host) => host.id === targetId)) return false
        set((state) => ({ executionHosts: state.executionHosts.map((host) => host.id === targetId ? { ...host, customIcon } : host) }))
      } else {
        const found = get().resourceCollections.some((collection) => targetType === 'collection' ? collection.id === targetId
          : collection.references.some((reference) => reference.id === targetId && reference.type === 'desktop-app'))
        if (!found) return false
        set((state) => ({ resourceCollections: state.resourceCollections.map((collection) => targetType === 'collection'
          ? collection.id === targetId ? { ...collection, customIcon } : collection
          : { ...collection, references: collection.references.map((reference) => reference.id === targetId && reference.type === 'desktop-app' ? { ...reference, customIcon } : reference) }) }))
      }
      return true
    },

    createResourceCollection: async (name: string): Promise<ResourceCollection | null> => {
      const normalized = name.trim()
      if (!normalized) return null
      if (isCefContext()) {
        const response = await sendToCEF<ResourceCollection>({
          action: 'createResourceCollection',
          payload: { name: normalized },
          requestId: createRequestId('createResourceCollection'),
        })
        if (!response.ok || !response.data?.id) return null
        set((state) => state.resourceCollections.some((item) => item.id === response.data!.id)
          ? {}
          : { resourceCollections: [...state.resourceCollections, response.data!] })
        return response.data
      }
      const collection = { id: nextLocalId('collection'), name: normalized, collapsed: false, references: [] }
      set((state) => ({ resourceCollections: [...state.resourceCollections, collection] }))
      return collection
    },

    renameResourceCollection: async (collectionId: string, name: string): Promise<boolean> => {
      const normalized = name.trim()
      if (!normalized || !get().resourceCollections.some((item) => item.id === collectionId)) return false
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'renameResourceCollection',
          payload: { collectionId, name: normalized },
          requestId: createRequestId('renameResourceCollection'),
        })
        if (!response.ok) return false
      }
      set((state) => ({
        resourceCollections: state.resourceCollections.map((item) => item.id === collectionId ? { ...item, name: normalized } : item),
      }))
      return true
    },

    deleteResourceCollection: async (collectionId: string): Promise<boolean> => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'deleteResourceCollection', payload: { collectionId }, requestId: createRequestId('deleteResourceCollection'),
        })
        if (!response.ok) return false
      }
      set((state) => ({ resourceCollections: state.resourceCollections.filter((item) => item.id !== collectionId) }))
      return true
    },

    toggleResourceCollection: async (collectionId: string): Promise<boolean> => {
      const collection = get().resourceCollections.find((item) => item.id === collectionId)
      if (!collection) return false
      const previousCollapsed = collection.collapsed
      set((state) => ({
        resourceCollections: state.resourceCollections.map((item) => item.id === collectionId ? { ...item, collapsed: !previousCollapsed } : item),
      }))
      if (!isCefContext()) return true

      const pending = pendingToggles.get(collectionId) ?? {
        baseline: previousCollapsed,
        count: 0,
        successCount: 0,
      }
      pending.count += 1
      pendingToggles.set(collectionId, pending)
      const requestId = createRequestId('toggleResourceCollection')
      const response = await sendToCEF({
        action: 'toggleResourceCollection', payload: { collectionId }, requestId,
      })
      pending.count -= 1
      if (response.ok) pending.successCount += 1
      if (pending.count === 0) {
        pendingToggles.delete(collectionId)
        const collapsed = pending.successCount % 2 === 0 ? pending.baseline : !pending.baseline
        set((state) => ({
          resourceCollections: state.resourceCollections.map((item) => item.id === collectionId ? { ...item, collapsed } : item),
        }))
      }
      return response.ok
    },

    reorderResourceCollections: async (collectionIds: string[]): Promise<boolean> => {
      const current = get().resourceCollections.map((entry) => entry.id)
      if (collectionIds.length !== current.length || new Set(collectionIds).size !== current.length || collectionIds.some((id) => !current.includes(id))) return false
      if (JSON.stringify(current) === JSON.stringify(collectionIds)) return true
      if (!await applyChange({ kind: 'collectionOrder', expected: current, replacement: collectionIds })) return false
      set({ organizationUndo: { label: 'Reorder collections', change: { kind: 'collectionOrder', expected: collectionIds, replacement: current } } })
      return true
    },

    addResourceReference: async (collectionId: string, type: ResourceReferenceType, target: string, label = ''): Promise<ResourceReference | null> => {
      const normalizedTarget = target.trim()
      if (!normalizedTarget) return null
      if (isCefContext()) {
        const response = await sendToCEF<ResourceReference>({
          action: 'addResourceReference',
          payload: { collectionId, type, target: normalizedTarget, label: label.trim() },
          requestId: createRequestId('addResourceReference'),
        })
        if (!response.ok || !response.data?.id) return null
        set((state) => ({
          resourceCollections: state.resourceCollections.map((item) => item.id === collectionId && !item.references.some((ref) => ref.id === response.data!.id)
            ? { ...item, references: [...item.references, response.data!] }
            : item),
        }))
        return response.data
      }
      const reference = { id: nextLocalId('reference'), type, target: normalizedTarget, label: label.trim() }
      set((state) => ({
        resourceCollections: state.resourceCollections.map((item) => item.id === collectionId
          ? { ...item, references: [...item.references, reference] }
          : item),
      }))
      return reference
    },

    removeResourceReference: async (collectionId: string, referenceId: string): Promise<boolean> => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'removeResourceReference', payload: { collectionId, referenceId }, requestId: createRequestId('removeResourceReference'),
        })
        if (!response.ok) return false
      }
      set((state) => ({
        resourceCollections: state.resourceCollections.map((item) => item.id === collectionId
          ? { ...item, references: item.references.filter((ref) => ref.id !== referenceId) }
          : item),
      }))
      return true
    },

    reorderResourceReferences: async (collectionId: string, referenceIds: string[]): Promise<boolean> => {
      const current = get().resourceCollections.find((entry) => entry.id === collectionId)?.references.map((entry) => entry.id)
      if (!current || referenceIds.length !== current.length || new Set(referenceIds).size !== current.length || referenceIds.some((id) => !current.includes(id))) return false
      if (JSON.stringify(current) === JSON.stringify(referenceIds)) return true
      if (!await applyChange({ kind: 'referenceOrder', collectionId, expected: current, replacement: referenceIds })) return false
      set({ organizationUndo: { label: 'Reorder references', change: { kind: 'referenceOrder', collectionId, expected: referenceIds, replacement: current } } })
      return true
    },
  }
}
