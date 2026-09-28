import { useEffect, useRef, useState } from 'react'
import type { KeyboardEvent as ReactKeyboardEvent } from 'react'
import { ChevronRight, Library } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { createRequestId } from '../../ipc/cefBridge'
import { ViewportMenu } from '../ui'
import type { ResourceReferenceType } from '../../types/resourceCollection'

export const COLLECTION_MOVE_FAILURE_EVENT = 'uam-collection-move-failure'
export type CollectionMoveFailure = { id: string; time: string; message: string; detail: string }

/** Moves a resource through the store actions and reports failures to the shell after rollback. */
export async function moveResourceToCollection(collectionId: string | null, type: ResourceReferenceType, target: string, label: string) {
  const { resourceCollections, addResourceReference, removeResourceReference } = useAppStore.getState()
  const memberships = resourceCollections.flatMap((collection) => collection.references
    .filter((reference) => reference.type === type && reference.target === target)
    .map((reference) => ({ collectionId: collection.id, referenceId: reference.id, label: reference.label })))
  const alreadyInTarget = memberships.some((membership) => membership.collectionId === collectionId)
  const removed = []

  try {
    for (const membership of memberships.filter((item) => item.collectionId !== collectionId)) {
      if (!await removeResourceReference(membership.collectionId, membership.referenceId)) {
        throw new Error('Could not remove the original collection membership.')
      }
      removed.push(membership)
    }
    if (collectionId && !alreadyInTarget && !await addResourceReference(collectionId, type, target, label)) {
      throw new Error('Could not add the destination collection membership.')
    }
    return true
  } catch (error) {
    const restored = await Promise.allSettled(removed.map((item) => addResourceReference(item.collectionId, type, target, item.label)))
    const restorationFailed = restored.some((result) => result.status === 'rejected' || !result.value)
    window.dispatchEvent(new CustomEvent<CollectionMoveFailure>(COLLECTION_MOVE_FAILURE_EVENT, {
      detail: {
        id: createRequestId('collection-move-failure'),
        time: new Date().toISOString(),
        message: `Could not move "${label}".`,
        detail: [
          error instanceof Error ? error.message : 'Collection move failed.',
          restorationFailed ? 'Some memberships could not be restored. Check collections before retrying.' : 'Check collection membership and try again.',
        ].join(' '),
      },
    }))
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
}: {
  target: string
  label: string
  onAdded: () => void
  type?: ResourceReferenceType
}) {
  const collections = useAppStore((state) => state.resourceCollections)
  const [open, setOpen] = useState(false)
  const triggerRef = useRef<HTMLButtonElement>(null)
  const submenuRef = useRef<HTMLDivElement>(null)
  const closeTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const focusSubmenuOnOpenRef = useRef(false)

  const cancelClose = () => {
    if (closeTimerRef.current) clearTimeout(closeTimerRef.current)
    closeTimerRef.current = null
  }
  const openSubmenu = (focusFirstItem = false) => {
    cancelClose()
    focusSubmenuOnOpenRef.current = focusFirstItem
    setOpen(true)
  }
  const closeSubmenu = (restoreFocus = false) => {
    cancelClose()
    setOpen(false)
    if (restoreFocus) triggerRef.current?.focus()
  }
  const scheduleClose = () => {
    cancelClose()
    closeTimerRef.current = setTimeout(() => setOpen(false), 120)
  }
  const onTriggerKeyDown = (event: ReactKeyboardEvent<HTMLButtonElement>) => {
    if (event.key !== 'ArrowRight' && event.key !== 'Enter' && event.key !== ' ') return
    event.preventDefault()
    event.stopPropagation()
    openSubmenu(true)
  }
  const onSubmenuKeyDown = (event: ReactKeyboardEvent<HTMLDivElement>) => {
    if (event.key !== 'ArrowLeft' && event.key !== 'Escape') return
    event.preventDefault()
    event.stopPropagation()
    closeSubmenu(true)
  }

  useEffect(() => {
    if (open && focusSubmenuOnOpenRef.current) {
      focusSubmenuOnOpenRef.current = false
      submenuRef.current?.querySelector<HTMLButtonElement>('button:not(:disabled)')?.focus()
    }
  }, [open])

  useEffect(() => () => cancelClose(), [])

  if (collections.length === 0) return null

  const memberships = collections.flatMap((collection) => collection.references
    .filter((reference) => reference.type === type && reference.target === target)
    .map((reference) => ({ collectionId: collection.id, referenceId: reference.id })))
  const currentCollectionIds = new Set(memberships.map(({ collectionId }) => collectionId))

  return (
    <>
      <div className="mx-2 my-1" style={{ borderTop: '1px solid var(--border)' }} />
      <button
        ref={triggerRef}
        type="button"
        aria-haspopup="menu"
        aria-expanded={open}
        className="uam-menu-select__option flex w-full items-center gap-2 px-3 py-1.5 text-left text-sm"
        style={{ border: 'none', color: 'var(--text-2)', fontFamily: 'inherit' }}
        onClick={() => open ? closeSubmenu() : openSubmenu(true)}
        onKeyDown={onTriggerKeyDown}
        onMouseEnter={() => openSubmenu()}
        onMouseLeave={scheduleClose}
      >
        <Library size={13} aria-hidden />
        <span className="flex-1">Move to collection</span>
        <ChevronRight size={13} aria-hidden />
      </button>
      {open && (
        <ViewportMenu
          ref={submenuRef}
          anchorRef={triggerRef}
          side="right"
          role="menu"
          aria-label="Move to collection"
          className="rounded-md py-1 animate-fade-in"
          style={{ minWidth: 168, background: 'var(--surface-up)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-2)' }}
          onKeyDown={onSubmenuKeyDown}
          onMouseEnter={cancelClose}
          onMouseLeave={scheduleClose}
        >
          {memberships.length > 0 && (
            <button
              type="button"
              role="menuitem"
              className="uam-menu-select__option flex w-full items-center gap-2 px-3 py-1.5 text-left text-sm"
              style={{ border: 'none', color: 'var(--text-2)', fontFamily: 'inherit' }}
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
                className="uam-menu-select__option flex w-full items-center gap-2 px-3 py-1.5 text-left text-sm"
                style={{ border: 'none', color: current ? 'var(--text-3)' : 'var(--text-2)', cursor: current ? 'default' : 'pointer', fontFamily: 'inherit' }}
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
        </ViewportMenu>
      )}
    </>
  )
}
