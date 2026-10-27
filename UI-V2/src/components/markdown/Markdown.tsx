import { FileInspectionContext, KnownBranchesContext } from '../shared/fileInspectionContext'
import { inlineFileReference, parseFileReference, type FileReference } from '../../utils/fileReferences'
import { memo, ReactNode, useContext } from 'react'
import { CopyTextButton } from '../chat/StatusHelpers'
import { ContextChip } from '../chat/ContextChip'
import { inlineChipFor, isPotentialTableRow, parseTableSeparator, safeHref, splitTableRow } from './markdownParsing'

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
      nodes.push(text.slice(lastIndex, match.index))
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
      nodes.push(<code key={key}>{reference ? <button type="button" style={{ color: 'var(--accent)', background: 'transparent', border: 'none', padding: 0, font: 'inherit', cursor: 'pointer' }} onClick={() => inspect?.(reference)} title="Inspect file">{value}</button> : value}</code>)
    } else if (token.startsWith('**')) {
      nodes.push(<strong key={key}>{token.slice(2, -2)}</strong>)
    } else {
      const link = token.match(/^\[([^\[\]]+)\]\(([^)]+)\)$/)
      const href = link ? safeHref(link[2]) : undefined
      const file = !href && inspect && link ? parseFileReference(link[2]) : null
      nodes.push(
        href ? (
          <a key={key} href={href} target="_blank" rel="noreferrer">
            {link?.[1]}
          </a>
        ) : file ? (
          <button key={key} type="button" style={{ color: 'var(--accent)', background: 'transparent', border: 'none', padding: 0, font: 'inherit', cursor: 'pointer' }} onClick={() => inspect?.(file)} title="Inspect file">{link?.[1]}</button>
        ) : (
          <span key={key}>{link?.[1] ?? token}</span>
        )
      )
    }

    lastIndex = match.index + token.length
  }

  if (lastIndex < text.length) {
    nodes.push(text.slice(lastIndex))
  }

  return nodes
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

    if (/^[-*]\s+/.test(trimmed)) {
      flushParagraph()
      const items: string[] = []
      while (index < lines.length && /^[-*]\s+/.test(lines[index].trim())) {
        items.push(lines[index].trim().replace(/^[-*]\s+/, ''))
        index++
      }
      nodes.push(
        <ul key={`${blockKey}-ul-${index}`}>
          {items.map((item, itemIndex) => (
            <li key={`${blockKey}-ul-${index}-${itemIndex}`}>
              {renderInlineMarkdown(item, `${blockKey}-ul-${index}-${itemIndex}`, inspect, chipBranches)}
            </li>
          ))}
        </ul>
      )
      continue
    }

    if (/^\d+[.)]\s+/.test(trimmed)) {
      flushParagraph()
      const items: string[] = []
      while (index < lines.length && /^\d+[.)]\s+/.test(lines[index].trim())) {
        items.push(lines[index].trim().replace(/^\d+[.)]\s+/, ''))
        index++
      }
      nodes.push(
        <ol key={`${blockKey}-ol-${index}`}>
          {items.map((item, itemIndex) => (
            <li key={`${blockKey}-ol-${index}-${itemIndex}`}>
              {renderInlineMarkdown(item, `${blockKey}-ol-${index}-${itemIndex}`, inspect, chipBranches)}
            </li>
          ))}
        </ol>
      )
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
