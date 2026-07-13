import { Library } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import type { ResourceReferenceType } from '../../types/resourceCollection'

export function CollectionMenuItems({
  type,
  target,
  label,
  onAdded,
}: {
  type: ResourceReferenceType
  target: string
  label: string
  onAdded: () => void
}) {
  const collections = useAppStore((state) => state.resourceCollections)
  const addReference = useAppStore((state) => state.addResourceReference)

  if (collections.length === 0) return null

  return (
    <>
      <div className="mx-2 my-1" style={{ borderTop: '1px solid var(--border)' }} />
      <div className="px-3 pb-1 pt-1 text-[10px] font-semibold uppercase tracking-wider" style={{ color: 'var(--text-3)' }}>
        Add to collection
      </div>
      {collections.map((collection) => {
        const alreadyAdded = collection.references.some((reference) => reference.type === type && reference.target === target)
        return (
          <button
            key={collection.id}
            type="button"
            disabled={alreadyAdded}
            className="flex w-full items-center gap-2 px-3 py-1.5 text-left text-sm"
            style={{
              background: 'transparent',
              border: 'none',
              color: alreadyAdded ? 'var(--text-3)' : 'var(--text-2)',
              cursor: alreadyAdded ? 'default' : 'pointer',
              fontFamily: 'inherit',
            }}
            onClick={() => {
              if (alreadyAdded) return
              void addReference(collection.id, type, target, label).then((created) => {
                if (created) onAdded()
              })
            }}
          >
            <Library size={13} aria-hidden />
            <span className="truncate">{collection.name}{alreadyAdded ? ' (added)' : ''}</span>
          </button>
        )
      })}
    </>
  )
}
