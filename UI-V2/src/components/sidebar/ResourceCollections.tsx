import { useEffect, useMemo, useRef, useState } from 'react'
import {
  AppWindow, ChevronDown, ChevronRight, File, Folder, Globe2, GripVertical,
  Library, Link2, MessageSquare, MoreHorizontal, Pencil, Plus, Trash2, X,
} from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import type { ResourceCollection, ResourceReference, ResourceReferenceType } from '../../types/resourceCollection'
import { Button, MenuSelect, Tooltip } from '../ui'
import { CollectionMenuItems } from './CollectionMenuItems'

const RESOURCE_TYPES: { value: ResourceReferenceType; label: string; placeholder: string }[] = [
  { value: 'file', label: 'File', placeholder: '/path/to/file' },
  { value: 'website', label: 'Website', placeholder: 'https://example.com' },
  { value: 'desktop-app', label: 'Desktop app', placeholder: '/Applications/App.app' },
  { value: 'workspace-folder', label: 'Workspace folder', placeholder: 'Folder ID' },
  { value: 'chat', label: 'Chat', placeholder: 'Chat ID' },
]

function referenceIcon(type: ResourceReferenceType) {
  if (type === 'chat') return <MessageSquare size={13} />
  if (type === 'workspace-folder') return <Folder size={13} />
  if (type === 'website') return <Globe2 size={13} />
  if (type === 'desktop-app') return <AppWindow size={13} />
  return <File size={13} />
}

function displayLabel(reference: ResourceReference) {
  return reference.label || reference.target.split(/[\\/]/).filter(Boolean).pop() || reference.target
}

function AddReferenceForm({ collectionId, onDone }: { collectionId: string; onDone: () => void }) {
  const [type, setType] = useState<ResourceReferenceType>('file')
  const [target, setTarget] = useState('')
  const [label, setLabel] = useState('')
  const addReference = useAppStore((state) => state.addResourceReference)
  const selectedType = RESOURCE_TYPES.find((item) => item.value === type) ?? RESOURCE_TYPES[0]

  return (
    <div className="mx-3 mb-2 space-y-2 rounded-md p-2" style={{ background: 'var(--surface-up)', border: '1px solid var(--border)' }}>
      <div className="flex gap-2">
        <div className="min-w-0 flex-1">
          <MenuSelect
            label="Resource type"
            value={type}
            options={RESOURCE_TYPES}
            onChange={(value) => setType(value as ResourceReferenceType)}
          />
        </div>
        <button type="button" aria-label="Cancel resource" onClick={onDone} style={{ background: 'transparent', border: 'none', color: 'var(--text-3)' }}>
          <X size={14} />
        </button>
      </div>
      <input
        autoFocus
        aria-label="Resource target"
        value={target}
        onChange={(event) => setTarget(event.target.value)}
        placeholder={selectedType.placeholder}
        className="w-full rounded px-2 py-1 text-xs outline-none"
        style={{ background: 'var(--surface)', color: 'var(--text)', border: '1px solid var(--border)' }}
      />
      <input
        aria-label="Resource label"
        value={label}
        onChange={(event) => setLabel(event.target.value)}
        placeholder="Label (optional)"
        className="w-full rounded px-2 py-1 text-xs outline-none"
        style={{ background: 'var(--surface)', color: 'var(--text)', border: '1px solid var(--border)' }}
      />
      <Button
        size="sm"
        variant="primary"
        disabled={!target.trim()}
        onClick={() => {
          void addReference(collectionId, type, target, label).then((created) => {
            if (created) onDone()
          })
        }}
      >
        Add resource
      </Button>
    </div>
  )
}

function ReferenceRow({ collection, reference }: { collection: ResourceCollection; reference: ResourceReference }) {
  const [menu, setMenu] = useState<{ x: number; y: number } | null>(null)
  const menuRef = useRef<HTMLDivElement>(null)
  const removeReference = useAppStore((state) => state.removeResourceReference)
  const reorderReferences = useAppStore((state) => state.reorderResourceReferences)
  const setActiveSession = useAppStore((state) => state.setActiveSession)

  const activate = () => {
    if (reference.type === 'chat') setActiveSession(reference.target)
    else if (reference.type === 'website') window.open(reference.target, '_blank', 'noopener,noreferrer')
  }

  useEffect(() => {
    if (!menu) return
    const close = (event: MouseEvent) => {
      if (!menuRef.current?.contains(event.target as Node)) setMenu(null)
    }
    const escape = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setMenu(null)
    }
    document.addEventListener('mousedown', close)
    document.addEventListener('keydown', escape)
    return () => {
      document.removeEventListener('mousedown', close)
      document.removeEventListener('keydown', escape)
    }
  }, [menu])

  return (
    <div
      className="group/reference mx-2 flex items-center gap-1 rounded px-1.5 py-1 text-xs"
      draggable
      data-testid={`resource-reference-${reference.id}`}
      onContextMenu={(event) => {
        event.preventDefault()
        event.stopPropagation()
        setMenu({ x: event.clientX, y: event.clientY })
      }}
      onDragStart={(event) => {
        event.stopPropagation()
        event.dataTransfer.effectAllowed = 'move'
        event.dataTransfer.setData('text/x-uam-reference-id', reference.id)
        event.dataTransfer.setData('text/x-uam-reference-collection-id', collection.id)
      }}
      onDragOver={(event) => {
        if (event.dataTransfer.types.includes('text/x-uam-reference-id')) event.preventDefault()
      }}
      onDrop={(event) => {
        event.preventDefault()
        event.stopPropagation()
        const sourceCollectionId = event.dataTransfer.getData('text/x-uam-reference-collection-id')
        const draggedId = event.dataTransfer.getData('text/x-uam-reference-id')
        if (sourceCollectionId !== collection.id || !draggedId || draggedId === reference.id) return
        const ids = collection.references.map((item) => item.id).filter((id) => id !== draggedId)
        ids.splice(ids.indexOf(reference.id), 0, draggedId)
        void reorderReferences(collection.id, ids)
      }}
      style={{ color: 'var(--text-2)' }}
    >
      <GripVertical size={11} aria-hidden style={{ color: 'var(--text-3)' }} />
      <button
        type="button"
        className="flex min-w-0 flex-1 items-center gap-1.5 text-left"
        style={{ background: 'transparent', border: 'none', color: 'inherit', cursor: reference.type === 'chat' || reference.type === 'website' ? 'pointer' : 'default' }}
        onClick={activate}
        title={reference.target}
      >
        {referenceIcon(reference.type)}
        <span className="truncate">{displayLabel(reference)}</span>
      </button>
      <button
        type="button"
        aria-label={`Remove ${displayLabel(reference)}`}
        className="opacity-0 group-hover/reference:opacity-100"
        style={{ background: 'transparent', border: 'none', color: 'var(--text-3)' }}
        onClick={() => { void removeReference(collection.id, reference.id) }}
      >
        <X size={12} />
      </button>
      {menu && (
        <div
          ref={menuRef}
          role="menu"
          aria-label={`Actions for ${displayLabel(reference)}`}
          className="fixed z-50 min-w-52 rounded-md py-1 shadow-lg"
          style={{ left: menu.x, top: menu.y, background: 'var(--surface-up)', border: '1px solid var(--border)' }}
        >
          <CollectionMenuItems
            type={reference.type}
            target={reference.target}
            label={displayLabel(reference)}
            onAdded={() => setMenu(null)}
          />
        </div>
      )}
    </div>
  )
}

function CollectionRow({ collection }: { collection: ResourceCollection }) {
  const [editing, setEditing] = useState(false)
  const [name, setName] = useState(collection.name)
  const [adding, setAdding] = useState(false)
  const [menuOpen, setMenuOpen] = useState(false)
  const toggle = useAppStore((state) => state.toggleResourceCollection)
  const rename = useAppStore((state) => state.renameResourceCollection)
  const remove = useAppStore((state) => state.deleteResourceCollection)
  const addReference = useAppStore((state) => state.addResourceReference)
  const removeReference = useAppStore((state) => state.removeResourceReference)
  const reorderCollections = useAppStore((state) => state.reorderResourceCollections)
  const collections = useAppStore((state) => state.resourceCollections)

  const commitName = () => {
    const trimmed = name.trim()
    if (trimmed && trimmed !== collection.name) void rename(collection.id, trimmed)
    else setName(collection.name)
    setEditing(false)
  }

  return (
    <div
      className="mb-1"
      data-testid={`resource-collection-${collection.id}`}
      draggable={!editing}
      onDragStart={(event) => {
        event.dataTransfer.effectAllowed = 'move'
        event.dataTransfer.setData('text/x-uam-collection-id', collection.id)
      }}
      onDragOver={(event) => event.preventDefault()}
      onDrop={(event) => {
        event.preventDefault()
        const draggedCollection = event.dataTransfer.getData('text/x-uam-collection-id')
        if (draggedCollection && draggedCollection !== collection.id) {
          const ids = collections.map((item) => item.id).filter((id) => id !== draggedCollection)
          ids.splice(ids.indexOf(collection.id), 0, draggedCollection)
          void reorderCollections(ids)
          return
        }
        const sourceCollectionId = event.dataTransfer.getData('text/x-uam-reference-collection-id')
        const draggedReferenceId = event.dataTransfer.getData('text/x-uam-reference-id')
        if (sourceCollectionId && sourceCollectionId !== collection.id && draggedReferenceId) {
          const sourceReference = collections
            .find((item) => item.id === sourceCollectionId)
            ?.references.find((item) => item.id === draggedReferenceId)
          if (!sourceReference) return
          void addReference(collection.id, sourceReference.type, sourceReference.target, sourceReference.label)
            .then((created) => {
              if (created) void removeReference(sourceCollectionId, draggedReferenceId)
            })
          return
        }
        const chatId = event.dataTransfer.getData('text/x-uam-chat-id')
        const folderId = event.dataTransfer.getData('text/x-uam-folder-resource-id')
        const target = chatId || folderId
        if (!target) return
        const type: ResourceReferenceType = chatId ? 'chat' : 'workspace-folder'
        const label = chatId
          ? useAppStore.getState().sessions.find((item) => item.id === chatId)?.name ?? chatId
          : useAppStore.getState().folders.find((item) => item.id === folderId)?.name ?? folderId
        void addReference(collection.id, type, target, label)
      }}
    >
      <div className="group/collection mx-1.5 flex items-center gap-1 rounded px-1.5 py-1" style={{ background: 'color-mix(in srgb, var(--surface-up) 55%, transparent)' }}>
        <button type="button" aria-label={collection.collapsed ? `Expand ${collection.name}` : `Collapse ${collection.name}`} onClick={() => { void toggle(collection.id) }} style={{ background: 'transparent', border: 'none', color: 'var(--text-3)', padding: 0 }}>
          {collection.collapsed ? <ChevronRight size={14} /> : <ChevronDown size={14} />}
        </button>
        <Library size={13} aria-hidden style={{ color: 'var(--accent)' }} />
        {editing ? (
          <input
            autoFocus
            value={name}
            onChange={(event) => setName(event.target.value)}
            onBlur={commitName}
            onKeyDown={(event) => {
              if (event.key === 'Enter') commitName()
              if (event.key === 'Escape') { setName(collection.name); setEditing(false) }
            }}
            className="min-w-0 flex-1 bg-transparent text-xs outline-none"
            style={{ color: 'var(--text)', borderBottom: '1px solid var(--accent)' }}
          />
        ) : (
          <button type="button" className="min-w-0 flex-1 truncate text-left text-xs font-medium" onClick={() => { void toggle(collection.id) }} style={{ background: 'transparent', border: 'none', color: 'var(--text)' }}>
            {collection.name}
          </button>
        )}
        <span className="text-[10px]" style={{ color: 'var(--text-3)' }}>{collection.references.length}</span>
        <Tooltip label="Collection actions">
          <button type="button" aria-label={`Actions for ${collection.name}`} onClick={() => setMenuOpen((open) => !open)} style={{ background: 'transparent', border: 'none', color: 'var(--text-3)', padding: 0 }}>
            <MoreHorizontal size={14} />
          </button>
        </Tooltip>
      </div>
      {menuOpen && (
        <div className="mx-3 my-1 rounded-md py-1" style={{ background: 'var(--surface-up)', border: '1px solid var(--border)' }}>
          <button type="button" className="flex w-full items-center gap-2 px-2 py-1 text-xs" style={{ background: 'transparent', border: 'none', color: 'var(--text-2)' }} onClick={() => { setMenuOpen(false); setAdding(true) }}><Plus size={12} />Add resource</button>
          <button type="button" className="flex w-full items-center gap-2 px-2 py-1 text-xs" style={{ background: 'transparent', border: 'none', color: 'var(--text-2)' }} onClick={() => { setMenuOpen(false); setEditing(true) }}><Pencil size={12} />Rename</button>
          <button type="button" className="flex w-full items-center gap-2 px-2 py-1 text-xs" style={{ background: 'transparent', border: 'none', color: 'var(--red)' }} onClick={() => { setMenuOpen(false); if (window.confirm(`Delete collection “${collection.name}”?`)) void remove(collection.id) }}><Trash2 size={12} />Delete</button>
        </div>
      )}
      {!collection.collapsed && collection.references.map((reference) => <ReferenceRow key={reference.id} collection={collection} reference={reference} />)}
      {!collection.collapsed && collection.references.length === 0 && !adding && (
        <div className="mx-3 px-2 py-1 text-[10px]" style={{ color: 'var(--text-3)' }}>Drop resources here</div>
      )}
      {adding && <AddReferenceForm collectionId={collection.id} onDone={() => setAdding(false)} />}
    </div>
  )
}

export function ResourceCollections() {
  const collections = useAppStore((state) => state.resourceCollections)
  const createCollection = useAppStore((state) => state.createResourceCollection)
  const [adding, setAdding] = useState(false)
  const [name, setName] = useState('')
  const collectionIds = useMemo(() => new Set(collections.map((item) => item.id)), [collections])

  const commit = () => {
    const normalized = name.trim()
    if (!normalized) return
    void createCollection(normalized).then((created) => {
      if (!created) return
      setName('')
      setAdding(false)
    })
  }

  return (
    <section aria-label="Resource collections" className="mb-2" data-collection-count={collectionIds.size}>
      <div className="flex items-center gap-2 px-3 pb-1 pt-1" style={{ color: 'var(--text-3)' }}>
        <span className="text-[10px] font-medium uppercase tracking-wider">Collections</span>
        <span style={{ height: 1, flex: 1, background: 'var(--border)' }} />
        <button type="button" aria-label="New collection" onClick={() => setAdding(true)} style={{ background: 'transparent', border: 'none', color: 'var(--text-3)', padding: 0 }}><Plus size={13} /></button>
      </div>
      {collections.map((collection) => <CollectionRow key={collection.id} collection={collection} />)}
      {adding && (
        <div className="mx-3 flex gap-1">
          <input
            autoFocus
            aria-label="Collection name"
            value={name}
            onChange={(event) => setName(event.target.value)}
            onKeyDown={(event) => {
              if (event.key === 'Enter') commit()
              if (event.key === 'Escape') { setName(''); setAdding(false) }
            }}
            placeholder="Collection name"
            className="min-w-0 flex-1 rounded px-2 py-1 text-xs outline-none"
            style={{ background: 'var(--surface)', color: 'var(--text)', border: '1px solid var(--border)' }}
          />
          <Button size="sm" variant="primary" disabled={!name.trim()} onClick={commit}>Create</Button>
        </div>
      )}
      {!adding && collections.length === 0 && (
        <button type="button" className="mx-3 flex items-center gap-1.5 py-1 text-xs" onClick={() => setAdding(true)} style={{ background: 'transparent', border: 'none', color: 'var(--text-3)' }}>
          <Link2 size={12} />Create a collection
        </button>
      )}
    </section>
  )
}
