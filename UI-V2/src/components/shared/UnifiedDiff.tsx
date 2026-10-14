import { useEffect, useMemo, useRef, useState } from 'react'
import './review.css'

export interface DiffLine {
  text: string
  kind: 'added' | 'removed' | 'context' | 'header'
  oldLine?: number
  newLine?: number
}

/** Line numbers come from each hunk, never from the rendered row position. */
export function parseUnifiedDiff(diff: string): DiffLine[] {
  let oldLine = 0
  let newLine = 0
  let inHunk = false
  return diff.split('\n').map((text) => {
    const hunk = /^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@/.exec(text)
    if (hunk) {
      oldLine = Number(hunk[1])
      newLine = Number(hunk[2])
      inHunk = true
      return { text, kind: 'header' }
    }
    if (text.startsWith('diff ') || text.startsWith('Index: ')) inHunk = false
    if (!inHunk) return { text, kind: 'header' }
    if (text.startsWith('+')) return { text, kind: 'added', newLine: newLine++ }
    if (text.startsWith('-')) return { text, kind: 'removed', oldLine: oldLine++ }
    if (text.startsWith(' ')) return { text, kind: 'context', oldLine: oldLine++, newLine: newLine++ }
    return { text, kind: 'header' }
  })
}

export interface SplitDiffRow {
  left?: DiffLine
  right?: DiffLine
  header?: string
}

/** Pair each change block without deriving source line numbers from its visual position. */
export function pairDiffLines(lines: DiffLine[]): SplitDiffRow[] {
  const result: SplitDiffRow[] = []
  let removed: DiffLine[] = []
  let added: DiffLine[] = []
  let previousKind: DiffLine['kind'] = 'header'
  const flush = () => {
    for (let index = 0; index < Math.max(removed.length, added.length); index++) result.push({ left: removed[index], right: added[index] })
    removed = []; added = []
  }
  // Hide framing before text hunks, preserving rename and permission metadata.
  const firstHunk = lines.findIndex(line => line.kind === 'header' && /^@@ -\d+(?:,\d+)? \+\d+(?:,\d+)? @@/.test(line.text))
  for (let index = 0; index < lines.length; index++) {
    const line = lines[index]
    if (index < firstHunk && /^(?:diff |Index: |={3,}$|index |new file mode 100644$|deleted file mode |--- |\+\+\+ )/.test(line.text)) continue
    if (line.kind === 'removed') removed.push(line)
    else if (line.kind === 'added') added.push(line)
    else if (line.text.startsWith('\\ No newline') && (previousKind === 'removed' || previousKind === 'added')) {
      const target = previousKind === 'removed' ? removed : added
      target.push(line)
      continue
    } else {
      flush()
      result.push(line.kind === 'context' ? { left: line, right: line } : { header: line.text })
    }
    previousKind = line.kind
  }
  flush()
  return result
}

const ROW_HEIGHT = 20
const OVERSCAN = 12

/** Fixed-height rows keep large patches from mounting thousands of DOM nodes. */
export function UnifiedDiff({ diff, path, layout = 'unified' }: { diff: string; path: string; layout?: 'unified' | 'split' }) {
  const lines = useMemo(() => parseUnifiedDiff(diff), [diff])
  const splitRows = useMemo(() => layout === 'split' ? pairDiffLines(lines) : [], [lines, layout])
  const rows = layout === 'split' ? splitRows : lines
  const viewportRef = useRef<HTMLDivElement>(null)
  const [viewport, setViewport] = useState({ top: 0, height: 600 })
  useEffect(() => {
    const element = viewportRef.current
    if (!element || typeof ResizeObserver === 'undefined') return
    const measure = () => setViewport({ top: element.scrollTop, height: element.clientHeight })
    const observer = new ResizeObserver(measure)
    observer.observe(element)
    measure()
    return () => observer.disconnect()
  }, [])
  useEffect(() => {
    const element = viewportRef.current
    if (!element) return
    element.scrollTop = 0
    setViewport({ top: 0, height: element.clientHeight || 600 })
  }, [layout])
  const start = Math.max(0, Math.floor(viewport.top / ROW_HEIGHT) - OVERSCAN)
  const end = Math.min(rows.length, Math.ceil((viewport.top + viewport.height) / ROW_HEIGHT) + OVERSCAN)
  const width = useMemo(() => layout === 'split'
    ? splitRows.reduce((longest, row) => Math.max(longest, row.header?.length ?? 0, (row.left?.text.length ?? 0) * 2, (row.right?.text.length ?? 0) * 2), 80)
    : lines.reduce((longest, row) => Math.max(longest, row.text.length), 40), [layout, lines, splitRows])
  return <div ref={viewportRef} className="uam-rv-diff min-h-0 flex-1 overflow-auto" role="region" aria-label={`${layout === 'split' ? 'Split' : 'Unified'} diff for ${path}`} tabIndex={0}
    onScroll={(event) => setViewport({ top: event.currentTarget.scrollTop, height: event.currentTarget.clientHeight })}>
    <div style={{ height: rows.length * ROW_HEIGHT, minWidth: `calc(${width}ch + 100px)`, position: 'relative' }}>
      <div style={{ position: 'absolute', top: start * ROW_HEIGHT, width: '100%' }}>
        {layout === 'split' ? splitRows.slice(start, end).map((row, offset) => <div key={start + offset} data-diff-row={start + offset}
          className="uam-rv-diff-row" style={{ height: ROW_HEIGHT, lineHeight: `${ROW_HEIGHT}px` }}>
          {row.header !== undefined ? <span className="uam-rv-diff-text" style={{ color: 'var(--text-3)', paddingLeft: 8 }}>{row.header || ' '}</span> : (['left', 'right'] as const).map((side) => {
            const line = row[side]
            return <div key={side} data-diff-side={side} data-kind={line?.kind ?? 'empty'} className="uam-rv-diff-side" style={{ lineHeight: `${ROW_HEIGHT}px` }}>
              <span aria-label={side === 'left' ? 'Old line' : 'New line'} className="uam-rv-gutter">{side === 'left' ? line?.oldLine : line?.newLine}</span>
              {line && line.kind !== 'header' && <span className="uam-rv-sign" aria-hidden>{line.text.charAt(0)}</span>}
              <span className="uam-rv-diff-text">{line ? line.kind === 'header' ? line.text : line.text.slice(1) : ' '}</span>
            </div>
          })}
        </div>) : lines.slice(start, end).map((row, offset) => <div key={start + offset} data-diff-row={start + offset}
          data-kind={row.kind} className="uam-rv-diff-row" style={{ height: ROW_HEIGHT, lineHeight: `${ROW_HEIGHT}px` }}>
          <span aria-label="Old line" className="uam-rv-gutter">{row.oldLine}</span>
          <span aria-label="New line" className="uam-rv-gutter">{row.newLine}</span>
          {row.kind !== 'header' && <span className="uam-rv-sign" aria-hidden>{row.text.charAt(0)}</span>}
          <span className="uam-rv-diff-text">{row.kind === 'header' ? row.text || ' ' : row.text.slice(1) || ' '}</span>
        </div>)}
      </div>
    </div>
  </div>
}
