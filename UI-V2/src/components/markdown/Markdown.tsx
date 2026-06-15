import { memo, ReactNode } from 'react'
import { isPotentialTableRow, parseTableSeparator, safeHref, splitTableRow } from './markdownParsing'

function renderInlineMarkdown(text: string, keyPrefix: string): ReactNode[] {
  const nodes: ReactNode[] = []
  const pattern = /(`[^`]+`|\*\*[^*]+?\*\*|\[[^\]]+\]\([^)]+\))/g
  let lastIndex = 0
  let match: RegExpExecArray | null

  while ((match = pattern.exec(text)) !== null) {
    if (match.index > lastIndex) {
      nodes.push(text.slice(lastIndex, match.index))
    }

    const token = match[0]
    const key = `${keyPrefix}-${match.index}`
    if (token.startsWith('`')) {
      nodes.push(<code key={key}>{token.slice(1, -1)}</code>)
    } else if (token.startsWith('**')) {
      nodes.push(<strong key={key}>{token.slice(2, -2)}</strong>)
    } else {
      const link = token.match(/^\[([^\]]+)\]\(([^)]+)\)$/)
      const href = link ? safeHref(link[2]) : undefined
      nodes.push(
        href ? (
          <a key={key} href={href} target="_blank" rel="noreferrer">
            {link?.[1]}
          </a>
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

function MarkdownTextBlock({ text, blockKey }: { text: string; blockKey: string }) {
  const lines = text.replace(/\r\n/g, '\n').split('\n')
  const nodes: ReactNode[] = []
  let paragraph: string[] = []
  let index = 0

  const flushParagraph = () => {
    if (paragraph.length === 0) return
    const content = paragraph.join(' ')
    nodes.push(<p key={`${blockKey}-p-${nodes.length}`}>{renderInlineMarkdown(content, `${blockKey}-p-${nodes.length}`)}</p>)
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
                      {renderInlineMarkdown(cell, `${blockKey}-th-${index}-${cellIndex}`)}
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
                        {renderInlineMarkdown(cell, `${blockKey}-td-${index}-${rowIndex}-${cellIndex}`)}
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
      const content = renderInlineMarkdown(heading[2], `${blockKey}-h-${index}`)
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
              {renderInlineMarkdown(item, `${blockKey}-ul-${index}-${itemIndex}`)}
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
              {renderInlineMarkdown(item, `${blockKey}-ol-${index}-${itemIndex}`)}
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
          {renderInlineMarkdown(quoteLines.join(' '), `${blockKey}-quote-${index}`)}
        </blockquote>
      )
      continue
    }

    paragraph.push(trimmed)
    index++
  }

  flushParagraph()
  return <>{nodes}</>
}

export const MarkdownContent = memo(function MarkdownContent({ content }: { content: string }) {
  const parts: ReactNode[] = []
  const fencePattern = /```([A-Za-z0-9_-]+)?\n?([\s\S]*?)```/g
  let lastIndex = 0
  let match: RegExpExecArray | null

  while ((match = fencePattern.exec(content)) !== null) {
    if (match.index > lastIndex) {
      parts.push(
        <MarkdownTextBlock
          key={`text-${lastIndex}`}
          blockKey={`text-${lastIndex}`}
          text={content.slice(lastIndex, match.index)}
        />
      )
    }

    const language = match[1]?.trim()
    parts.push(
      <pre key={`code-${match.index}`}>
        {language && <div className="mb-2 text-[10px] uppercase" style={{ color: 'var(--text-3)' }}>{language}</div>}
        <code>{match[2].replace(/\n$/, '')}</code>
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
      />
    )
  }

  return <div className="prose-msg">{parts}</div>
})
