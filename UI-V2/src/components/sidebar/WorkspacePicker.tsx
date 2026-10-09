import { useEffect, useId, useMemo, useRef, useState, type KeyboardEvent } from 'react'
import { Check, ChevronDown, ChevronRight } from 'lucide-react'
import type { Folder } from '../../types/session'
import type { ResourceCollection } from '../../types/resourceCollection'
import { ViewportMenu } from '../ui'

export interface WorkspacePickerProps {
  folders: readonly Folder[]
  collections: readonly ResourceCollection[]
  executionHostId: string
  value: string | null
  onChange: (folderId: string | null) => void
  remoteEmptyOption?: { label: string; description?: string }
  disabled?: boolean
  label?: string
}

interface WorkspaceRow {
  key: string
  folderId: string | null
  label: string
  path: string
  group: string
  groupId: string
}

/** Mirrors sidebar collection ownership without reading the store or changing durable folder identities. */
export function WorkspacePicker({ folders, collections, executionHostId, value, onChange, remoteEmptyOption, disabled = false, label = 'Workspace' }: WorkspacePickerProps) {
  const [open, setOpen] = useState(false)
  const [query, setQuery] = useState('')
  const [activeKey, setActiveKey] = useState('')
  const [disclosureOverrides, setDisclosureOverrides] = useState<ReadonlyMap<string, boolean>>(() => new Map())
  const [revealedGroupId, setRevealedGroupId] = useState('')
  const triggerRef = useRef<HTMLButtonElement>(null)
  const menuRef = useRef<HTMLDivElement>(null)
  const searchRef = useRef<HTMLInputElement>(null)
  const listId = useId()
  const hostId = executionHostId || 'local'
  const rows = useMemo(() => {
    const compatible = new Map<string, Folder>()
    for (const folder of folders) {
      if (folder.parentId === null && (folder.executionHostId || 'local') === hostId && !compatible.has(folder.id)) compatible.set(folder.id, folder)
    }
    const result: WorkspaceRow[] = []
    const assigned = new Set<string>()
    const add = (folder: Folder, group: string, groupId: string) => {
      assigned.add(folder.id)
      result.push({ key: `folder:${folder.id}`, folderId: folder.id, label: folder.name, path: folder.directory, group, groupId })
    }
    if (hostId !== 'local' && remoteEmptyOption) result.push({ key: 'empty', folderId: null, label: remoteEmptyOption.label, path: remoteEmptyOption.description ?? '', group: '', groupId: '' })
    for (const collection of collections) {
      for (const reference of collection.references) {
        const folder = reference.type === 'workspace-folder' ? compatible.get(reference.target) : undefined
        if (folder && !assigned.has(folder.id)) add(folder, collection.name, collection.id)
      }
    }
    for (const folder of compatible.values()) if (!assigned.has(folder.id)) add(folder, '', '')
    return result
  }, [folders, collections, hostId, remoteEmptyOption])
  const tokens = query.trim().toLowerCase().split(/\s+/).filter(Boolean)
  const filtered = rows.filter((row) => tokens.every((token) => `${row.label} ${row.path} ${row.group}`.toLowerCase().includes(token)))
  const savedCollapseByGroupId = new Map(collections.map((collection) => [collection.id, collection.collapsed]))
  const collapsed = (groupId: string) => Boolean(groupId && !tokens.length &&
    (disclosureOverrides.get(groupId) ?? (groupId !== revealedGroupId && savedCollapseByGroupId.get(groupId))))
  const visible = filtered.filter((row) => !collapsed(row.groupId))
  const optionIndexByKey = new Map(visible.map((row, index) => [row.key, index]))
  const activeIndex = Math.max(0, visible.findIndex((row) => row.key === activeKey))
  const selected = rows.find((row) => row.folderId === value)

  const close = (returnFocus = true) => {
    setOpen(false)
    if (returnFocus) triggerRef.current?.focus()
  }
  const show = () => {
    if (disabled) return
    setQuery('')
    setDisclosureOverrides(new Map())
    setRevealedGroupId(selected?.groupId ?? '')
    setActiveKey(selected?.key ?? rows[0]?.key ?? '')
    setOpen(true)
  }
  const choose = (row: WorkspaceRow) => {
    onChange(row.folderId)
    close()
  }

  useEffect(() => {
    if (!open) return
    searchRef.current?.focus()
    const dismiss = (event: MouseEvent) => {
      const target = event.target as Node
      if (!menuRef.current?.contains(target) && !triggerRef.current?.contains(target)) setOpen(false)
    }
    document.addEventListener('mousedown', dismiss)
    return () => document.removeEventListener('mousedown', dismiss)
  }, [open])
  useEffect(() => {
    if (disabled) setOpen(false)
  }, [disabled])
  useEffect(() => {
    if (!open) return
    menuRef.current?.querySelector<HTMLElement>(`[data-workspace-option-index="${activeIndex}"]`)?.scrollIntoView?.({ block: 'nearest' })
  }, [open, activeIndex, query, hostId])

  const onKeyDown = (event: KeyboardEvent<HTMLDivElement>) => {
    if (event.key === 'Escape') {
      event.preventDefault()
      event.stopPropagation()
      close()
    } else if (event.key === 'Tab') {
      event.stopPropagation()
      close()
    } else if ((event.target as HTMLElement).hasAttribute('data-workspace-group')) {
      // Disclosure buttons keep native Enter/Space activation instead of selecting a folder.
      event.stopPropagation()
    } else if (event.key === 'Enter') {
      event.preventDefault()
      event.stopPropagation()
      if (visible[activeIndex]) choose(visible[activeIndex])
    } else if (['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) {
      event.preventDefault()
      event.stopPropagation()
      if (!visible.length) return
      const index = event.key === 'Home' ? 0 : event.key === 'End' ? visible.length - 1
        : (activeIndex + (event.key === 'ArrowDown' ? 1 : -1) + visible.length) % visible.length
      setActiveKey(visible[index].key)
    }
  }

  return <>
    <button ref={triggerRef} type="button" role="combobox" aria-label={label} aria-haspopup="listbox" aria-expanded={open} aria-controls={open ? listId : undefined} disabled={disabled}
      className="flex w-full items-center justify-between gap-2 rounded-md border border-[var(--border)] px-3 py-2 text-left text-sm outline-none"
      style={{ background: 'var(--surface-up)', color: 'var(--text)' }}
      onFocus={(event) => { event.currentTarget.style.borderColor = 'var(--accent)' }}
      onBlur={(event) => { event.currentTarget.style.borderColor = 'var(--border)' }}
      onClick={() => open ? close() : show()} onKeyDown={(event) => {
        if (event.key === 'ArrowDown' || event.key === 'ArrowUp') { event.preventDefault(); event.stopPropagation(); show() }
      }}>
      <span className="truncate">{selected?.label ?? 'Choose workspace'}</span><ChevronDown size={14} aria-hidden />
    </button>
    {open && <ViewportMenu ref={menuRef} anchorRef={triggerRef} manageFocus={false} onRequestClose={() => close()} onKeyDown={onKeyDown}
      className="p-1 text-white" style={{ minWidth: Math.max(triggerRef.current?.getBoundingClientRect().width ?? 0, 260), maxHeight: 320, background: '#000', border: '1px solid var(--border-bright)' }}>
      <input ref={searchRef} type="search" aria-label="Search workspaces" aria-controls={listId} aria-activedescendant={visible.length ? `${listId}-option-${activeIndex}` : undefined} value={query}
        onChange={(event) => { setQuery(event.target.value); setActiveKey('') }} className="w-full bg-black px-2 py-1.5 text-sm text-white border-b border-[var(--border)]" placeholder="Search workspaces" />
      <div id={listId} role="listbox" aria-label={label}>
        {filtered.map((row, index) => {
          const optionIndex = optionIndexByKey.get(row.key) ?? -1
          const groupCollapsed = collapsed(row.groupId)
          return <div key={row.key}>
            {row.group && (index === 0 || filtered[index - 1].groupId !== row.groupId) && <button type="button" data-workspace-group={row.groupId} aria-expanded={!groupCollapsed}
              className="flex w-full items-center gap-1 px-2 pt-2 pb-1 text-left text-xs font-medium" disabled={tokens.length > 0}
              onMouseDown={(event) => event.preventDefault()} onClick={() => setDisclosureOverrides((current) => new Map(current).set(row.groupId, !groupCollapsed))}>
              {groupCollapsed ? <ChevronRight size={12} aria-hidden /> : <ChevronDown size={12} aria-hidden />}{row.group}
            </button>}
            {!groupCollapsed && <button type="button" role="option" id={`${listId}-option-${optionIndex}`} data-workspace-option-index={optionIndex} aria-selected={row.folderId === value} tabIndex={-1}
              className="flex w-full items-center gap-2 px-2 py-1.5 text-left" style={{ background: optionIndex === activeIndex ? 'var(--sidebar-item-hover)' : undefined }}
              onMouseEnter={() => setActiveKey(row.key)} onMouseDown={(event) => event.preventDefault()} onClick={() => choose(row)}>
              <span className="min-w-0 flex-1"><span className="block truncate text-sm">{row.label}</span>{row.path && <span className="block truncate text-xs" title={row.path}>{row.path}</span>}</span>
              {row.folderId === value && <Check size={13} aria-hidden />}
            </button>}
          </div>
        })}
      </div>
      {filtered.length === 0 && <div role="status" className="px-2 py-2 text-xs">{query.trim() ? 'No matching workspaces' : 'No workspaces on this host'}</div>}
    </ViewportMenu>}
  </>
}
