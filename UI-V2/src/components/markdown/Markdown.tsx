import { FileInspectionContext, KnownBranchesContext } from '../shared/fileInspectionContext'
import { inlineFileReference, parseFileReference, type FileReference } from '../../utils/fileReferences'
import { memo, ReactNode, useContext } from 'react'
import { CopyTextButton } from '../chat/StatusHelpers'
import { ContextChip } from '../chat/ContextChip'
import { Tooltip } from '../ui'
import { inlineChipFor, isPotentialTableRow, parseTableSeparator, replaceInlineLatexSymbols, safeHref, splitTableRow } from './markdownParsing'

/** chipBranches is null when inline code should stay plain (chips disabled for this surface). */
function renderInlineMarkdown(text: string, keyPrefix: string, inspect: ((reference: FileReference) => void) | null, chipBranches: readonly string[] | null): ReactNode[] {
  const nodes: ReactNode[] = []
  let pattern = /(`[^`]+`|\*\*[^*]+?\*\*|\[[^\[\]]+\]\()/g
  let lastIndex = 0
  let match: RegExpExecArray | null

  while ((match = pattern.exec(text)) !== null) {
    let token = match[0]
    if (token.startsWith('[')) {
      const end = text.indexOf(')', pattern.lastIndex)
      if (end < 0) {
        // No later link can close; keep parsing code and bold without rescanning the suffix.
        pattern = /(`[^`]+`|\*\*[^*]+?\*\*)/g
        pattern.lastIndex = match.index
        continue
      }
      if (end === pattern.lastIndex) {
        pattern.lastIndex = match.index + 1
        continue
      }
      token = text.slice(match.index, end + 1)
      pattern.lastIndex = end + 1
    }
    if (match.index > lastIndex) {
      nodes.push(replaceInlineLatexSymbols(text.slice(lastIndex, match.index)))
    }

    const key = `${keyPrefix}-${match.index}`
    if (token.startsWith('`')) {
      const value = token.slice(1, -1)
      const chip = chipBranches ? inlineChipFor(value, chipBranches) : null
      const reference = inspect ? inlineFileReference(value) : null
      if (chip) {
        nodes.push(<ContextChip key={key} kind={chip.kind} name={chip.name} detail={value} path={chip.reference?.path} reference={chip.reference} />)
        lastIndex = match.index + token.length
        continue
      }
      // ui: raw button kept — Button asLink is inline-flex with pill height and padding, which breaks text flow inside code
      nodes.push(<code key={key}>{reference ? <Tooltip label="Inspect file"><button type="button" style={{ color: 'var(--accent)', background: 'transparent', border: 'none', padding: 0, font: 'inherit', cursor: 'pointer' }} onClick={() => inspect?.(reference)}>{value}</button></Tooltip> : value}</code>)
    } else if (token.startsWith('**')) {
      nodes.push(<strong key={key}>{replaceInlineLatexSymbols(token.slice(2, -2))}</strong>)
    } else {
      const link = token.match(/^\[([^\[\]]+)\]\(([^)]+)\)$/)
      const href = link ? safeHref(link[2]) : undefined
      const file = !href && inspect && link ? parseFileReference(link[2]) : null
      nodes.push(
        href ? (
          <a key={key} href={href} target="_blank" rel="noreferrer">
            {replaceInlineLatexSymbols(link?.[1] ?? '')}
          </a>
        ) : file ? (
          // ui: raw button kept — Button asLink is inline-flex with pill height and padding, which breaks text flow inside prose
          <Tooltip key={key} label="Inspect file"><button type="button" style={{ color: 'var(--accent)', background: 'transparent', border: 'none', padding: 0, font: 'inherit', cursor: 'pointer' }} onClick={() => inspect?.(file)}>{replaceInlineLatexSymbols(link?.[1] ?? '')}</button></Tooltip>
        ) : (
          <span key={key}>{link ? replaceInlineLatexSymbols(link[1]) : token}</span>
        )
      )
    }

    lastIndex = match.index + token.length
  }

  if (lastIndex < text.length) {
    nodes.push(replaceInlineLatexSymbols(text.slice(lastIndex)))
  }

  return nodes
}

const BULLET_ITEM = /^[-*]\s+/
const ORDERED_ITEM = /^\d+[.)]\s+/
/** Lines that open another block, so they end a list item rather than continue it. */
const BLOCK_START = /^(#{1,3}\s|>|[-*]\s|\d+[.)]\s)/

/** A list item runs on through indented or lazy lines, as in Markdown; a blank line only ends the list when no item or indented line follows. */
function readListItems(lines: string[], start: number, marker: RegExp): { items: string[]; next: number } {
  const items: string[] = []
  let index = start
  while (index < lines.length) {
    const trimmed = lines[index].trim()
    if (marker.test(trimmed)) {
      items.push(trimmed.replace(marker, ''))
      index++
    } else if (!trimmed) {
      let following = index + 1
      while (following < lines.length && !lines[following].trim()) following++
      if (following >= lines.length) break
      const next = lines[following]
      if (!marker.test(next.trim()) && !(/^\s/.test(next) && !BLOCK_START.test(next.trim()))) break
      index = following
    } else if (BLOCK_START.test(trimmed) || isPotentialTableRow(trimmed)) {
      break
    } else {
      items[items.length - 1] += ` ${trimmed}`
      index++
    }
  }
  return { items, next: index }
}

// Settled text before a streaming code fence keeps its props, so memo skips re-parsing it.
export const MarkdownTextBlock = memo(function MarkdownTextBlock({ text, blockKey, chips }: { text: string; blockKey: string; chips: boolean }) {
  const inspect = useContext(FileInspectionContext)
  const knownBranches = useContext(KnownBranchesContext)
  const chipBranches = chips ? knownBranches : null
  const lines = text.replace(/\r\n/g, '\n').split('\n')
  const nodes: ReactNode[] = []
  let paragraph: string[] = []
  let index = 0

  const flushParagraph = () => {
    if (paragraph.length === 0) return
    const content = paragraph.join(' ')
    nodes.push(<p key={`${blockKey}-p-${nodes.length}`}>{renderInlineMarkdown(content, `${blockKey}-p-${nodes.length}`, inspect, chipBranches)}</p>)
    paragraph = []
  }

  while (index < lines.length) {
    const line = lines[index]
    const trimmed = line.trim()

    if (!trimmed) {
      flushParagraph()
      index++
      continue
    }

    if (index + 1 < lines.length && isPotentialTableRow(trimmed)) {
      const headerCells = splitTableRow(trimmed)
      const alignments = parseTableSeparator(lines[index + 1].trim(), headerCells.length)
      if (alignments) {
        flushParagraph()
        index += 2
        const bodyRows: string[][] = []
        while (index < lines.length && isPotentialTableRow(lines[index].trim())) {
          const rowCells = splitTableRow(lines[index].trim())
          if (rowCells.length !== headerCells.length) break
          bodyRows.push(rowCells)
          index++
        }

        nodes.push(
          <div key={`${blockKey}-table-${index}`} className="prose-msg-table-scroll">
            <table>
              <thead>
                <tr>
                  {headerCells.map((cell, cellIndex) => (
                    <th key={`${blockKey}-th-${index}-${cellIndex}`} style={{ textAlign: alignments[cellIndex] }}>
                      {renderInlineMarkdown(cell, `${blockKey}-th-${index}-${cellIndex}`, inspect, chipBranches)}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {bodyRows.map((row, rowIndex) => (
                  <tr key={`${blockKey}-tr-${index}-${rowIndex}`}>
                    {row.map((cell, cellIndex) => (
                      <td
                        key={`${blockKey}-td-${index}-${rowIndex}-${cellIndex}`}
                        style={{ textAlign: alignments[cellIndex] }}
                      >
                        {renderInlineMarkdown(cell, `${blockKey}-td-${index}-${rowIndex}-${cellIndex}`, inspect, chipBranches)}
                      </td>
                    ))}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )
        continue
      }
    }

    const heading = trimmed.match(/^(#{1,3})\s+(.+)$/)
    if (heading) {
      flushParagraph()
      const level = heading[1].length
      const content = renderInlineMarkdown(heading[2], `${blockKey}-h-${index}`, inspect, chipBranches)
      if (level === 1) nodes.push(<h1 key={`${blockKey}-h-${index}`}>{content}</h1>)
      else if (level === 2) nodes.push(<h2 key={`${blockKey}-h-${index}`}>{content}</h2>)
      else nodes.push(<h3 key={`${blockKey}-h-${index}`}>{content}</h3>)
      index++
      continue
    }

    if (BULLET_ITEM.test(trimmed) || ORDERED_ITEM.test(trimmed)) {
      flushParagraph()
      const ordered = !BULLET_ITEM.test(trimmed)
      const list = readListItems(lines, index, ordered ? ORDERED_ITEM : BULLET_ITEM)
      const key = `${blockKey}-${ordered ? 'ol' : 'ul'}-${index}`
      index = list.next
      const items = list.items.map((item, itemIndex) => <li key={`${key}-${itemIndex}`}>{renderInlineMarkdown(item, `${key}-${itemIndex}`, inspect, chipBranches)}</li>)
      nodes.push(ordered ? <ol key={key}>{items}</ol> : <ul key={key}>{items}</ul>)
      continue
    }

    if (/^>\s?/.test(trimmed)) {
      flushParagraph()
      const quoteLines: string[] = []
      while (index < lines.length && /^>\s?/.test(lines[index].trim())) {
        quoteLines.push(lines[index].trim().replace(/^>\s?/, ''))
        index++
      }
      nodes.push(
        <blockquote key={`${blockKey}-quote-${index}`}>
          {renderInlineMarkdown(quoteLines.join(' '), `${blockKey}-quote-${index}`, inspect, chipBranches)}
        </blockquote>
      )
      continue
    }

    paragraph.push(trimmed)
    index++
  }

  flushParagraph()
  return <>{nodes}</>
})

export const MarkdownContent = memo(function MarkdownContent({ content, chips = false }: { content: string; chips?: boolean }) {
  if (!content.trim()) return null
  const parts: ReactNode[] = []
  const fencePattern = /```(?:([A-Za-z0-9_-]+)\r?\n|\r?\n)?([\s\S]*?)```/g
  let lastIndex = 0
  let match: RegExpExecArray | null

  while ((match = fencePattern.exec(content)) !== null) {
    if (match.index > lastIndex) {
      parts.push(
        <MarkdownTextBlock
          key={`text-${lastIndex}`}
          blockKey={`text-${lastIndex}`}
          text={content.slice(lastIndex, match.index)}
          chips={chips}
        />
      )
    }

    const language = match[1]?.trim()
    const code = match[2].replace(/\r?\n$/, '')
    parts.push(
      <pre key={`code-${match.index}`}>
        <div className="mb-2 flex items-center justify-between gap-2 text-[10px]" style={{ color: 'var(--text-3)' }}>
          <span className="uppercase">{language}</span>
          <CopyTextButton text={code} label="Copy code block" title="Copy code block" />
        </div>
        <code>{code}</code>
      </pre>
    )
    lastIndex = match.index + match[0].length
  }

  if (lastIndex < content.length) {
    parts.push(
      <MarkdownTextBlock
        key={`text-${lastIndex}`}
        blockKey={`text-${lastIndex}`}
        text={content.slice(lastIndex)}
        chips={chips}
      />
    )
  }

  return <div className="prose-msg">{parts}</div>
})
