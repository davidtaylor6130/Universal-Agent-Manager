import { Library } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'

export async function moveFolderToCollection(collectionId: string | null, target: string, label: string) {
  const { resourceCollections, addResourceReference, removeResourceReference } = useAppStore.getState()
  const memberships = resourceCollections.flatMap((collection) => collection.references
    .filter((reference) => reference.type === 'workspace-folder' && reference.target === target)
    .map((reference) => ({ collectionId: collection.id, referenceId: reference.id })))
  const alreadyInTarget = memberships.some((membership) => membership.collectionId === collectionId)

  if (collectionId && !alreadyInTarget && !await addResourceReference(collectionId, 'workspace-folder', target, label)) return false
  const results = await Promise.all(memberships
    .filter((membership) => membership.collectionId !== collectionId)
    .map((membership) => removeResourceReference(membership.collectionId, membership.referenceId)))
  return results.every(Boolean)
}

export function CollectionMenuItems({
  target,
  label,
  onAdded,
}: {
  target: string
  label: string
  onAdded: () => void
}) {
  const collections = useAppStore((state) => state.resourceCollections)

  if (collections.length === 0) return null

  const memberships = collections.flatMap((collection) => collection.references
    .filter((reference) => reference.type === 'workspace-folder' && reference.target === target)
    .map((reference) => ({ collectionId: collection.id, referenceId: reference.id })))
  const currentCollectionIds = new Set(memberships.map(({ collectionId }) => collectionId))

  return (
    <>
      <div className="mx-2 my-1" style={{ borderTop: '1px solid var(--border)' }} />
      <div className="px-3 pb-1 pt-1 text-[10px] font-semibold uppercase tracking-wider" style={{ color: 'var(--text-3)' }}>
        Move to collection
      </div>
      {memberships.length > 0 && (
        <button
          type="button"
          className="flex w-full items-center gap-2 px-3 py-1.5 text-left text-sm"
          style={{ background: 'transparent', border: 'none', color: 'var(--text-2)', fontFamily: 'inherit' }}
          onClick={() => { void moveFolderToCollection(null, target, label).then((moved) => { if (moved) onAdded() }) }}
        >
          <Library size={13} aria-hidden />
          <span>Remove from collection</span>
        </button>
      )}
      {collections.map((collection) => {
        const current = currentCollectionIds.has(collection.id)
        return (
          <button
            key={collection.id}
            type="button"
            disabled={current}
            className="flex w-full items-center gap-2 px-3 py-1.5 text-left text-sm"
            style={{
              background: 'transparent',
              border: 'none',
              color: current ? 'var(--text-3)' : 'var(--text-2)',
              cursor: current ? 'default' : 'pointer',
              fontFamily: 'inherit',
            }}
            onClick={() => {
              if (current) return
              void moveFolderToCollection(collection.id, target, label).then((moved) => { if (moved) onAdded() })
            }}
          >
            <Library size={13} aria-hidden />
            <span className="truncate">{collection.name}{current ? ' (current)' : ''}</span>
          </button>
        )
      })}
    </>
  )
}
