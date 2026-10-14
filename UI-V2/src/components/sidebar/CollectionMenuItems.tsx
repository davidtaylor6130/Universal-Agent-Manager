import { Library } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import type { ResourceReferenceType } from '../../types/resourceCollection'
import { reportAppFailure } from '../../utils/appFailure'
import { RowSubmenu } from './RowSubmenu'

/** Moves membership through one guarded native transaction and reports failures to the shell. */
export async function moveResourceToCollection(collectionId: string | null, type: ResourceReferenceType, target: string, label: string) {
  try {
    if (await useAppStore.getState().moveResourceToCollection(collectionId, type, target, label)) return true
    throw new Error('Collection membership changed or could not be saved. Refresh and try again.')
  } catch (error) {
    reportAppFailure(`Could not move "${label}".`, error instanceof Error ? error.message : 'Collection move failed.')
    return false
  }
}

export const moveFolderToCollection = (collectionId: string | null, target: string, label: string) =>
  moveResourceToCollection(collectionId, 'workspace-folder', target, label)

export function CollectionMenuItems({
  target,
  label,
  onAdded,
  type = 'workspace-folder',
  showDivider = true,
}: {
  target: string
  label: string
  onAdded: () => void
  type?: ResourceReferenceType
  showDivider?: boolean
}) {
  const collections = useAppStore((state) => state.resourceCollections)
  if (collections.length === 0) return null

  const memberships = collections.flatMap((collection) => collection.references
    .filter((reference) => reference.type === type && reference.target === target)
    .map((reference) => ({ collectionId: collection.id, referenceId: reference.id })))
  const currentCollectionIds = new Set(memberships.map(({ collectionId }) => collectionId))

  return (
    <>
      {showDivider && <div className="uam-sb-menu-divider" role="separator" />}
      <RowSubmenu label="Move to collection" ariaLabel="Move to collection" icon={<Library size={13} aria-hidden />}>
        {memberships.length > 0 && (
          <button
            type="button"
            role="menuitem"
            className="uam-sb-menu-item"
            onClick={() => { void moveResourceToCollection(null, type, target, label).then((moved) => { if (moved) onAdded() }) }}
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
              role="menuitem"
              disabled={current}
              className="uam-sb-menu-item"
              onClick={() => {
                if (current) return
                void moveResourceToCollection(collection.id, type, target, label).then((moved) => { if (moved) onAdded() })
              }}
            >
              <Library size={13} aria-hidden />
              <span className="truncate">{collection.name}{current ? ' (current)' : ''}</span>
            </button>
          )
        })}
      </RowSubmenu>
    </>
  )
}
