import { Library } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import type { ResourceReferenceType } from '../../types/resourceCollection'
import { reportAppFailure } from '../../utils/appFailure'
import { RowSubmenu } from './RowSubmenu'
import { runForEach, type BulkTarget } from '../../utils/runForEach'

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
  targets,
}: {
  target: string
  label: string
  onAdded: () => void
  type?: ResourceReferenceType
  showDivider?: boolean
  /** Multi-selection: every listed resource moves together. */
  targets?: BulkTarget[]
}) {
  const collections = useAppStore((state) => state.resourceCollections)
  if (collections.length === 0) return null

  const rows = targets && targets.length > 1 ? targets : [{ id: target, label }]
  const collectionsOf = (id: string) => new Set(collections.filter((collection) => collection.references
    .some((reference) => reference.type === type && reference.target === id)).map((collection) => collection.id))
  const memberships = rows.map(({ id }) => collectionsOf(id))
  const inAnyCollection = memberships.some((ids) => ids.size > 0)
  const move = (collectionId: string | null) => {
    if (rows.length === 1) {
      void moveResourceToCollection(collectionId, type, target, label).then((moved) => { if (moved) onAdded() })
      return
    }
    // Rows already in the destination are left alone; every other row is moved and failures are reported by name.
    const moving = rows.filter((_, index) => collectionId === null ? memberships[index].size > 0 : !memberships[index].has(collectionId))
    void runForEach(moving, async (row) => await useAppStore.getState().moveResourceToCollection(collectionId, type, row.id, row.label)
      ? null : 'Collection membership changed or could not be saved. Refresh and try again.', 'Could not move to collection')
      .then((moved) => { if (moved.length === moving.length) onAdded() })
  }

  return (
    <>
      {showDivider && <div className="uam-sb-menu-divider" role="separator" />}
      <RowSubmenu label="Move to collection" ariaLabel="Move to collection" icon={<Library size={13} aria-hidden />}>
        {inAnyCollection && (
          <button
            type="button"
            role="menuitem"
            className="uam-sb-menu-item"
            onClick={() => move(null)}
          >
            <Library size={13} aria-hidden />
            <span>Remove from collection</span>
          </button>
        )}
        {collections.map((collection) => {
          const current = memberships.every((ids) => ids.has(collection.id))
          return (
            <button
              key={collection.id}
              type="button"
              role="menuitem"
              disabled={current}
              className="uam-sb-menu-item"
              onClick={() => {
                if (current) return
                move(collection.id)
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
