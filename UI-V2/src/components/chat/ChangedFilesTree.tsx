import { useId, useMemo, useRef, useState } from 'react'
import type { KeyboardEvent } from 'react'
import { ChevronRight, Folder, FolderOpen } from 'lucide-react'
import { Tooltip } from '../ui'
import type { VcsChangedFile } from '../../store/useAppStore'
import { buildChangedFilesTree, defaultExpandedFolderKeys, type ChangedTreeNode } from './changedFilesTreeModel'
import './ChangedFilesTree.css'

interface ChangedFilesTreeProps {
  files: VcsChangedFile[]
  selectedPath?: string | null
  onOpenFile: (file: VcsChangedFile) => void
}

interface VisibleRow {
  rowKey: string
  node: ChangedTreeNode<VcsChangedFile>
  parentRowKey: string | null
  expanded: boolean
}

const STATUS_TONE: Record<string, string> = { M: 'var(--warning)', A: 'var(--success)', D: 'var(--error)', R: 'var(--info)' }
/** Words for the status letter, which is otherwise only a coloured glyph. */
const STATUS_LABEL: Record<string, string> = { M: 'Modified', A: 'Added', D: 'Deleted', R: 'Renamed', C: 'Copied', T: 'Type changed', U: 'Conflicted', '?': 'Untracked', '!': 'Ignored' }

const lines = (count: number, verb: string) => `${count} ${count === 1 ? 'line' : 'lines'} ${verb}`

/** What a screen reader hears after a row's name: the change type for files, then the line counts (or binary). */
export function changedRowDescription(node: ChangedTreeNode<VcsChangedFile>): string {
  // Folders are named by their own name, so the description starts with the full path the tooltip shows.
  if (node.kind === 'folder') return `${node.path !== node.name ? `${node.path}, ` : ''}${lines(node.additions, 'added')}, ${lines(node.deletions, 'removed')}.`
  const status = (node.file.status.trim().charAt(0) || 'M').toUpperCase()
  const change = STATUS_LABEL[status] ?? 'Changed'
  return node.file.binary ? `${change}, binary file.` : `${change}, ${lines(node.file.additions, 'added')}, ${lines(node.file.deletions, 'removed')}.`
}

const rowKeyOf = (node: ChangedTreeNode<VcsChangedFile>) => `${node.kind}:${node.key}`

function flatten(nodes: ChangedTreeNode<VcsChangedFile>[], isExpanded: (node: ChangedTreeNode<VcsChangedFile>) => boolean, parentRowKey: string | null, out: VisibleRow[]): VisibleRow[] {
  for (const node of nodes) {
    const expanded = node.kind === 'folder' && isExpanded(node)
    out.push({ rowKey: rowKeyOf(node), node, parentRowKey, expanded })
    if (expanded && node.kind === 'folder') flatten(node.children, isExpanded, rowKeyOf(node), out)
  }
  return out
}

/** Collapsible directory browser for the turn's changed files. Arrow keys move, Left/Right fold, Enter opens. */
export function ChangedFilesTree({ files, selectedPath, onOpenFile }: ChangedFilesTreeProps) {
  const tree = useMemo(() => buildChangedFilesTree(files), [files])
  const defaultOpen = useMemo(() => defaultExpandedFolderKeys(tree, files.length), [tree, files.length])
  // Only user toggles are stored; everything else follows the default for the current file count.
  const [overrides, setOverrides] = useState<Record<string, boolean>>({})
  const [activeKey, setActiveKey] = useState<string | null>(null)
  const rowRefs = useRef(new Map<string, HTMLDivElement>())
  const idPrefix = useId()

  const rows = useMemo(() => flatten(tree, (node) => overrides[node.key] ?? defaultOpen.has(node.key), null, []), [tree, overrides, defaultOpen])
  const currentKey = rows.some((row) => row.rowKey === activeKey) ? activeKey : rows[0]?.rowKey ?? null

  if (rows.length === 0) return null

  const focusRow = (rowKey: string | null | undefined) => {
    if (!rowKey) return
    setActiveKey(rowKey)
    rowRefs.current.get(rowKey)?.focus()
  }
  const toggleFolder = (node: ChangedTreeNode<VcsChangedFile>, open: boolean) => {
    setOverrides((current) => ({ ...current, [node.key]: open }))
  }
  const activate = (row: VisibleRow) => {
    if (row.node.kind === 'file') onOpenFile(row.node.file)
    else toggleFolder(row.node, !row.expanded)
  }

  const onKeyDown = (event: KeyboardEvent<HTMLDivElement>) => {
    const index = rows.findIndex((row) => row.rowKey === currentKey)
    const row = rows[index]
    if (!row) return
    const folder = row.node.kind === 'folder'
    switch (event.key) {
      case 'ArrowDown':
        focusRow(rows[index + 1]?.rowKey)
        break
      case 'ArrowUp':
        focusRow(rows[index - 1]?.rowKey)
        break
      case 'ArrowRight':
        if (folder && !row.expanded) toggleFolder(row.node, true)
        else if (folder) focusRow(rows[index + 1]?.rowKey)
        break
      case 'ArrowLeft':
        if (folder && row.expanded) toggleFolder(row.node, false)
        else focusRow(row.parentRowKey)
        break
      case 'Enter':
      case ' ':
        activate(row)
        break
      default:
        return
    }
    event.preventDefault()
  }

  return (
    <div role="tree" aria-label="Changed files" className="uam-changes-tree" onKeyDown={onKeyDown}>
      {rows.map((row, rowIndex) => {
        const { node } = row
        const active = row.rowKey === currentKey
        const selected = node.kind === 'file' && node.path === selectedPath
        const status = node.kind === 'file' ? (node.file.status.trim().charAt(0) || 'M').toUpperCase() : ''
        const descriptionId = `${idPrefix}-${rowIndex}`
        // The letter and counts are visual; the row's description carries them in words.
        const counts = node.kind === 'folder' ? (
          <span className="uam-tree-counts" aria-hidden><span style={{ color: 'var(--success)' }}>+{node.additions}</span><span style={{ color: 'var(--error)' }}>-{node.deletions}</span></span>
        ) : node.file.binary ? (
          <span className="uam-tree-counts" aria-hidden style={{ color: 'var(--text-3)' }}>BIN</span>
        ) : (
          <span className="uam-tree-counts" aria-hidden><span style={{ color: 'var(--success)' }}>+{node.file.additions}</span><span style={{ color: 'var(--error)' }}>-{node.file.deletions}</span></span>
        )
        return (
          <Tooltip key={row.rowKey} label={node.path}>
          <div
            role="treeitem"
            ref={(element) => {
              if (element) rowRefs.current.set(row.rowKey, element)
              else rowRefs.current.delete(row.rowKey)
            }}
            tabIndex={active ? 0 : -1}
            aria-level={node.depth + 1}
            aria-expanded={node.kind === 'folder' ? row.expanded : undefined}
            aria-selected={node.kind === 'file' ? selected : undefined}
            aria-label={node.kind === 'file' ? `Review changes to ${node.path}` : node.name}
            aria-describedby={descriptionId}
            className="uam-tree-row uam-choice-button"
            style={{ paddingLeft: node.depth * 12 + 8 }}
            onFocus={() => setActiveKey(row.rowKey)}
            onClick={() => { setActiveKey(row.rowKey); activate(row) }}
          >
            {node.kind === 'folder' ? (
              <>
                <ChevronRight className="uam-tree-chevron" data-open={row.expanded} size={12} aria-hidden style={{ color: 'var(--text-3)' }} />
                {row.expanded ? <FolderOpen size={14} aria-hidden style={{ color: 'var(--text-3)' }} /> : <Folder size={14} aria-hidden style={{ color: 'var(--text-3)' }} />}
                <span className="uam-tree-name">{node.name}</span>
              </>
            ) : (
              <>
                <span className="uam-tree-status" aria-hidden style={{ color: STATUS_TONE[status] ?? 'var(--text-3)' }}>{status}</span>
                <span className="uam-tree-name">{node.name}</span>
              </>
            )}
            {counts}
            <span id={descriptionId} hidden>{changedRowDescription(node)}</span>
          </div>
          </Tooltip>
        )
      })}
    </div>
  )
}
