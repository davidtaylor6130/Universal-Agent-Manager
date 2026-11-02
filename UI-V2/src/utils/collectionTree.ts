import type { ResourceCollection } from '../types/resourceCollection'

/** Mirrors ResourceCollectionService::kMaxCollectionDepth; native validation stays authoritative. */
export const MAX_COLLECTION_DEPTH = 5

type Tree = readonly ResourceCollection[]

/** Ancestors from the collection itself up to its top-level root, stopping on unknown parents or cycles. */
function ancestry(collections: Tree, id: string): ResourceCollection[] {
  const chain: ResourceCollection[] = []
  for (let current = collections.find((entry) => entry.id === id); current && !chain.includes(current) && chain.length <= MAX_COLLECTION_DEPTH;) {
    chain.push(current)
    current = current.parentId ? collections.find((entry) => entry.id === current!.parentId) : undefined
  }
  return chain
}

export const collectionDepth = (collections: Tree, id: string) => ancestry(collections, id).length

export const collectionPath = (collections: Tree, id: string) => ancestry(collections, id).reverse().map((entry) => entry.name).join(' / ')

/** The collection and every collection nested beneath it, in stored order. */
export function collectionSubtreeIds(collections: Tree, id: string): string[] {
  const ids = [id]
  for (let index = 0; index < ids.length; index += 1) {
    for (const entry of collections) if (entry.parentId === ids[index] && !ids.includes(entry.id)) ids.push(entry.id)
  }
  return ids
}

function subtreeHeight(collections: Tree, id: string, seen = new Set<string>()): number {
  seen.add(id)
  const children = collections.filter((entry) => entry.parentId === id && !seen.has(entry.id))
  return 1 + Math.max(0, ...children.map((child) => subtreeHeight(collections, child.id, seen)))
}

/** Whether `collectionId` (null for a new collection) may live under `parentId` (null for top level). */
export function canNestCollection(collections: Tree, collectionId: string | null, parentId: string | null): boolean {
  if (!parentId) return true
  if (!collections.some((entry) => entry.id === parentId)) return false
  if (collectionId && collectionSubtreeIds(collections, collectionId).includes(parentId)) return false
  const height = collectionId ? subtreeHeight(collections, collectionId) : 1
  return collectionDepth(collections, parentId) + height <= MAX_COLLECTION_DEPTH
}
