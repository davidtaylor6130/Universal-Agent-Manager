import { createRequestId, isCefContext, sendToCEF } from '../../ipc/cefBridge'
import type { ResourceCollection, ResourceReference, ResourceReferenceType } from '../../types/resourceCollection'
import type { ZustandGet, ZustandSet } from '../storeTypes'

let localId = 0

function nextLocalId(prefix: string) {
  localId += 1
  return `${prefix}-${localId}`
}

export function createResourceCollectionsSlice(set: ZustandSet, get: ZustandGet) {
  return {
    resourceCollections: [] as ResourceCollection[],

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
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'toggleResourceCollection', payload: { collectionId }, requestId: createRequestId('toggleResourceCollection'),
        })
        if (!response.ok) return false
      }
      set((state) => ({
        resourceCollections: state.resourceCollections.map((item) => item.id === collectionId ? { ...item, collapsed: !item.collapsed } : item),
      }))
      return true
    },

    reorderResourceCollections: async (collectionIds: string[]): Promise<boolean> => {
      const current = get().resourceCollections
      if (collectionIds.length !== current.length || new Set(collectionIds).size !== current.length) return false
      const byId = new Map(current.map((item) => [item.id, item]))
      const ordered = collectionIds.flatMap((id) => byId.get(id) ? [byId.get(id)!] : [])
      if (ordered.length !== current.length) return false
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'reorderResourceCollections', payload: { collectionIds }, requestId: createRequestId('reorderResourceCollections'),
        })
        if (!response.ok) return false
      }
      set({ resourceCollections: ordered })
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
      const collection = get().resourceCollections.find((item) => item.id === collectionId)
      if (!collection || referenceIds.length !== collection.references.length || new Set(referenceIds).size !== collection.references.length) return false
      const byId = new Map(collection.references.map((item) => [item.id, item]))
      const ordered = referenceIds.flatMap((id) => byId.get(id) ? [byId.get(id)!] : [])
      if (ordered.length !== collection.references.length) return false
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'reorderResourceReferences', payload: { collectionId, referenceIds }, requestId: createRequestId('reorderResourceReferences'),
        })
        if (!response.ok) return false
      }
      set((state) => ({
        resourceCollections: state.resourceCollections.map((item) => item.id === collectionId ? { ...item, references: ordered } : item),
      }))
      return true
    },
  }
}
