import { useEffect, useRef, useState } from 'react'
import { Search, X } from 'lucide-react'
import type { RoutingFolderGroup } from '../../types/executionRouting'
import type { ExecutionHost, Folder } from '../../types/session'
import { fromViewportMenu, getTopmostModal, trapModalTab } from '../../utils/modalFocus'
import { Button, IconButton, MenuSelect, Overlay, SegmentedControl } from '../ui'
import '../shared/review.css'

export interface FolderMappingChoice { hostId: string; folderId: string; destinationFolderId: string }

const keyOf = (hostId: string, folderId: string) => `${hostId}\n${folderId}`
const lastName = (path: string) => path.split(/[\\/]/).filter(Boolean).pop() ?? path

/**
 * "Map folder…" chooser, built like Import chats: pick a computer, tick workspaces, give each a registered remote folder, then Map N.
 * It only stages the mappings (onApply); Save routing still persists them. `inline` renders it as an in-place step of the
 * New chat routing dialog (a dialog never opens a second dialog); otherwise it is its own modal with focus trap and Escape.
 */
export function MapFolderChooser({ hosts, workspaces, folders, preselect, inline = false, onApply, onClose }: {
  hosts: ExecutionHost[]
  /** Every local workspace with its current entries (mapped and unmapped). */
  workspaces: RoutingFolderGroup[]
  folders: Folder[]
  preselect?: { hostId: string; folderId: string }
  inline?: boolean
  onApply: (choices: FolderMappingChoice[]) => void
  onClose: () => void
}) {
  const [hostId, setHostId] = useState(preselect?.hostId ?? hosts[0]?.id ?? '')
  const [filter, setFilter] = useState('')
  const [selected, setSelected] = useState<string[]>(preselect ? [keyOf(preselect.hostId, preselect.folderId)] : [])
  const [choices, setChoices] = useState<Record<string, string>>(() => {
    // Keep a still-valid chosen folder even when the row shows a problem (a stale duplicate), so Map 1 only replaces the duplicate.
    const current = preselect && workspaces.find((group) => group.folder.id === preselect.folderId)?.entries.find((entry) => entry.hostId === preselect.hostId)?.destinationFolderId
    const registered = current && folders.some((folder) => folder.id === current && folder.executionHostId === preselect.hostId && folder.directory)
    return preselect && registered ? { [keyOf(preselect.hostId, preselect.folderId)]: current } : {}
  })
  const panel = useRef<HTMLDivElement>(null)
  const closeRef = useRef(onClose); closeRef.current = onClose
  useEffect(() => {
    panel.current?.focus()
    if (inline) return
    const key = (event: KeyboardEvent) => {
      if (event.defaultPrevented || fromViewportMenu(event) || getTopmostModal() !== panel.current) return
      if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); closeRef.current() }
      else trapModalTab(event)
    }
    window.addEventListener('keydown', key, true)
    return () => window.removeEventListener('keydown', key, true)
  }, [inline])

  const host = hosts.find((item) => item.id === hostId)
  const remote = folders.filter((folder) => folder.executionHostId === hostId && folder.directory)
  // Already-mapped workspaces are hidden, except the one a row's Choose… opened.
  const listedOn = (on: string, group: RoutingFolderGroup) => !group.entries.some((entry) => entry.hostId === on && !entry.error) || (preselect?.hostId === on && preselect.folderId === group.folder.id)
  const candidates = workspaces.filter((group) => listedOn(hostId, group))
  const hidden = workspaces.filter((group) => !candidates.includes(group))
  const needle = filter.trim().toLowerCase()
  const shown = candidates.filter((group) => !needle || group.folder.name.toLowerCase().includes(needle) || group.folder.directory!.toLowerCase().includes(needle))
  const shownKeys = shown.map((group) => keyOf(hostId, group.folder.id))
  const allShown = shownKeys.length > 0 && shownKeys.every((key) => selected.includes(key))
  const hereCount = selected.filter((key) => key.startsWith(`${hostId}\n`)).length
  const elsewhere = selected.length - hereCount
  const missing = selected.filter((key) => !choices[key]).length
  // Switching computers carries ticks without a remote folder over to the new computer (the user is still choosing where they
  // go); one that would be hidden there is dropped, since an invisible tick must not block Map N. Ticks with a folder stay put.
  const switchHost = (next: string) => {
    setSelected((current) => [...new Set(current.flatMap((key) => {
      if (key.startsWith(`${next}\n`) || choices[key]) return [key]
      const folderId = key.slice(key.indexOf('\n') + 1)
      const group = workspaces.find((item) => item.folder.id === folderId)
      return group && listedOn(next, group) ? [keyOf(next, folderId)] : []
    }))])
    setHostId(next)
  }
  const toggle = (keys: string[], on: boolean) => setSelected((current) => on ? [...new Set([...current, ...keys])] : current.filter((key) => !keys.includes(key)))
  const apply = () => onApply(selected.map((key) => { const [host, folderId] = key.split('\n'); return { hostId: host, folderId, destinationFolderId: choices[key] } }))
  const names = hidden.map((group) => group.folder.name)
  const hiddenNote = names.length === 0 ? '' : names.length <= 3
    ? `${names.length > 1 ? `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}` : names[0]} already ${names.length === 1 ? 'has' : 'have'} a folder on ${host?.label}.`
    : `${names.length} workspaces already have a folder on ${host?.label}.`

  const body = <>
    <header className="uam-rv-dialog__header">
      <div className="min-w-0 flex-1">
        <h2 className="uam-rv-title">Map folders to a computer</h2>
        <p className="uam-rv-muted">Chats in these workspaces can then run on the computer you pick.</p>
      </div>
      {!inline && <IconButton icon={<X size={16} />} label="Close map folders" onClick={onClose} />}
    </header>
    <div className="uam-rv-dialog__body">
      <div className="uam-rv-selectbar uam-rt-chooser__bar">
        {hosts.length <= 4
          ? <SegmentedControl aria-label="Computer" value={hostId} onChange={switchHost} options={hosts.map((item) => ({ value: item.id, label: item.label }))} />
          : <MenuSelect label="Computer" value={hostId} onChange={switchHost} options={hosts.map((item) => ({ value: item.id, label: item.label }))} />}
        <label className="uam-rt-chooser__filter">
          <Search size={12} aria-hidden />
          <input type="search" className="uam-field" aria-label="Filter workspaces" placeholder="Filter workspaces" value={filter} onChange={(event) => setFilter(event.target.value)} />
        </label>
      </div>
      {remote.length === 0
        ? <p className="uam-rt-chooser__note">No folders on {host?.label} yet. Add a folder on {host?.label} first.</p>
        : <>
          {shown.length > 0 && <div className="uam-rv-selectbar">
            <label className="uam-rv-selectall">
              <input type="checkbox" className="uam-rv-check" aria-label="Select all workspaces" checked={allShown} onChange={(event) => toggle(shownKeys, event.target.checked)} />
              <span>Select all</span>
            </label>
            <span className="uam-rv-muted">{hereCount} selected{elsewhere ? `, +${elsewhere} on other computers` : ''}</span>
          </div>}
          <div className="uam-rv-list" aria-label={`Workspaces to map to ${host?.label}`}>
            {shown.map((group) => {
              const key = keyOf(hostId, group.folder.id)
              const directory = group.folder.directory!
              return <div key={key} className="uam-rv-row" data-selected={selected.includes(key)}>
                <label className="uam-rv-row__main">
                  <input type="checkbox" className="uam-rv-check" aria-label={`Select ${group.folder.name}`} checked={selected.includes(key)} onChange={(event) => toggle([key], event.target.checked)} />
                  <span className="uam-rv-row__meta uam-rt-chooser__dir" title={directory}>{lastName(directory)}</span>
                  <span className="uam-rv-row__title">{group.folder.name}</span>
                </label>
                <span className="uam-rt-chooser__pick">
                  <MenuSelect label={`Remote folder for ${group.folder.name} on ${host?.label}`} placeholder="Choose remote folder" value={choices[key] ?? ''}
                    onChange={(value) => { setChoices((current) => ({ ...current, [key]: value })); toggle([key], true) }}
                    options={remote.map((folder) => ({ value: folder.id, label: `${folder.name} · ${folder.directory}` }))} />
                </span>
              </div>
            })}
            {shown.length === 0 && <p className="uam-rt-chooser__note">{candidates.length ? 'No workspaces match the filter.' : `Every workspace already has a folder on ${host?.label}.`}</p>}
          </div>
        </>}
      {hiddenNote && candidates.length > 0 && <p className="uam-rt-chooser__note">{hiddenNote}</p>}
      <div className="uam-rv-actions">
        <span className="uam-rv-muted" role="status">{missing ? `Choose a remote folder for ${missing === 1 ? 'the selected workspace' : `all ${missing} selected workspaces`}.` : 'Added to your unsaved routing changes.'}</span>
        <span className="uam-rv-actions__spacer" />
        <Button size="sm" variant="ghost" onClick={onClose}>Cancel</Button>
        <Button size="sm" variant="primary" disabled={selected.length === 0 || missing > 0} onClick={apply}>{selected.length ? `Map ${selected.length}` : 'Map'}</Button>
      </div>
    </div>
  </>
  return inline
    ? <div ref={panel} tabIndex={-1} role="group" aria-label="Map folders to a computer" className="uam-rt-chooser uam-rt-chooser--inline">{body}</div>
    : <Overlay open zIndex={80} onBackdropMouseDown={onClose}>
      <div ref={panel} tabIndex={-1} role="dialog" aria-modal="true" aria-label="Map folders to a computer" className="uam-rv-dialog uam-rt-chooser w-full max-w-2xl max-h-[85vh]">{body}</div>
    </Overlay>
}
