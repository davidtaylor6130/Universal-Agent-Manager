import { useContext, type ReactNode } from 'react'
import { BookOpen, FileText, Folder, GitBranch, Image as ImageIcon, X } from 'lucide-react'
import { FileInspectionContext } from '../shared/fileInspectionContext'
import type { FileReference } from '../../utils/fileReferences'
import { IconButton, Tooltip, cx } from '../ui'
import './ContextChip.css'

export type ContextChipKind = 'file' | 'directory' | 'image' | 'skill' | 'branch' | 'worktree'

const KINDS: Record<ContextChipKind, { label: string; icon: ReactNode }> = {
  file: { label: 'file', icon: <FileText size={13} aria-hidden /> },
  directory: { label: 'folder', icon: <Folder size={13} aria-hidden /> },
  image: { label: 'image', icon: <ImageIcon size={13} aria-hidden /> },
  skill: { label: 'skill', icon: <BookOpen size={13} aria-hidden /> },
  branch: { label: 'branch', icon: <GitBranch size={13} aria-hidden /> },
  worktree: { label: 'worktree', icon: <Folder size={13} aria-hidden /> },
}

/** Shared pill for referenced context (attachments, skills) in the composer and sent messages. */
export function ContextChip({ kind, name, detail, path, reference, meta, error, partial, onInspect, onRemove, removeLabel }: {
  kind: ContextChipKind
  name: string
  /** Full path or detail, shown mono in the tooltip. */
  detail?: string
  /** When set (and not a folder) and a file inspector is available, the chip opens a preview. */
  path?: string
  /** Opens this (e.g. with a line range) instead of the bare path. */
  reference?: FileReference
  /** Overrides the default kind label (e.g. "copying"). */
  meta?: string
  error?: boolean
  partial?: boolean
  /** Inspect captured context without treating its display path as read authority. */
  onInspect?: () => void
  onRemove?: () => void
  removeLabel?: string
}) {
  const inspect = useContext(FileInspectionContext)
  const openable = Boolean(onInspect || (inspect && path && kind !== 'directory' && kind !== 'branch' && kind !== 'worktree'))
  const body = (
    <>
      <span className="uam-context-chip__icon">{KINDS[kind].icon}</span>
      <span className="uam-context-chip__name">{name}</span>
      <span className="uam-context-chip__meta">{meta ?? KINDS[kind].label}</span>
    </>
  )
  return (
    <span className={cx('uam-context-chip', error && 'is-error', partial && 'is-partial')} data-detail={detail}>
      <Tooltip label={detail && <span style={{ fontFamily: 'var(--font-mono)', whiteSpace: 'pre-line' }}>{detail}</span>}>
        {openable ? (
          <button type="button" className="uam-context-chip__body" onClick={() => onInspect ? onInspect() : inspect?.(reference ?? { path: path as string })}>{body}</button>
        ) : (
          <span className="uam-context-chip__body" tabIndex={0}>{body}</span>
        )}
      </Tooltip>
      {onRemove && (
        <IconButton className="uam-context-chip__remove" size="sm" icon={<X size={12} aria-hidden />} label={removeLabel ?? `Remove ${name}`} onClick={onRemove} />
      )}
    </span>
  )
}
