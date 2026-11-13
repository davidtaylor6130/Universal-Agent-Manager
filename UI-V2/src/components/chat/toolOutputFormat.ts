// Readable text for tool output that is a JSON object or array. Returns null for anything else,
// so plain-text output keeps its exact original form.

const isNested = (value: unknown): value is object => typeof value === 'object' && value !== null
const isEmpty = (value: object) => Array.isArray(value) ? value.length === 0 : Object.keys(value).length === 0

function scalar(value: unknown): string {
  if (value === null) return 'none'
  if (typeof value === 'boolean') return value ? 'yes' : 'no'
  return String(value)
}

function render(value: object, indent: string, lines: string[]) {
  if (Array.isArray(value)) {
    for (const item of value) {
      if (!isNested(item)) lines.push(`${indent}- ${scalar(item)}`)
      else if (isEmpty(item)) lines.push(`${indent}- none`)
      else {
        const nested: string[] = []
        render(item, `${indent}  `, nested)
        nested[0] = `${indent}- ${nested[0].slice(indent.length + 2)}`
        lines.push(...nested)
      }
    }
    return
  }
  for (const [key, item] of Object.entries(value)) {
    if (!isNested(item)) lines.push(`${indent}${key}: ${scalar(item)}`)
    else if (isEmpty(item)) lines.push(`${indent}${key}: none`)
    else {
      lines.push(`${indent}${key}:`)
      render(item, `${indent}  `, lines)
    }
  }
}

export function formatReadableToolOutput(text: string): string | null {
  const trimmed = text.trim()
  if (!/^[[{]/.test(trimmed)) return null
  let parsed: unknown
  try { parsed = JSON.parse(trimmed) } catch { return null }
  if (!isNested(parsed)) return null
  if (isEmpty(parsed)) return 'none'
  const lines: string[] = []
  render(parsed, '', lines)
  return lines.join('\n')
}

// UAM-control secret_request, as each provider names it (Codex, Claude, OpenCode, bare).
const SECRET_REQUEST_TOOL = /^(?:secret_request|uam-control[._]secret_request|mcp__uam[-_]control__secret_request)$/
type SummarisableTool = { title: string; status: string; content: string; contentDeferred?: boolean }

function parsedObject(text: string): Record<string, unknown> | undefined {
  try {
    const value: unknown = JSON.parse(text)
    return isNested(value) && !Array.isArray(value) ? value as Record<string, unknown> : undefined
  } catch { return undefined }
}

/**
 * A secret_request row shows only "Secret: <label>", the reason and the outcome; its other arguments and the
 * delivered path never reach the transcript. Any other tool is returned unchanged.
 */
export function redactSecretRequestTool<T extends SummarisableTool>(tool: T): T {
  if (!SECRET_REQUEST_TOOL.test(tool.title)) return tool
  let text = tool.contentDeferred ? '' : tool.content.trim().replace(/^Approval: [^\n]*\n\n/, '')
  let result = ''
  if (text.startsWith('Arguments:\n')) {
    const at = text.lastIndexOf('\n\nResult:')
    result = at < 0 ? '' : text.slice(at + '\n\nResult:'.length).trim()
    text = text.slice('Arguments:\n'.length, at < 0 ? undefined : at)
  }
  const args = parsedObject(text.trim())
  const label = typeof args?.label === 'string' ? args.label.trim() : ''
  const reason = typeof args?.reason === 'string' ? args.reason.trim() : ''
  const status = tool.status.trim().toLowerCase()
  const outcomeRow = parsedObject(result)
  const firstBlock = Array.isArray(outcomeRow?.content) && isNested(outcomeRow.content[0]) ? outcomeRow.content[0] as Record<string, unknown> : undefined
  const resultText = typeof outcomeRow?.error === 'string' ? outcomeRow.error : typeof firstBlock?.text === 'string' ? firstBlock.text : outcomeRow ? '' : result
  const cancelled = ['cancelled', 'canceled'].includes(status) || /\bcancel/i.test(resultText)
  const outcome = ['pending', 'running', 'in_progress', 'inprogress', ''].includes(status) ? ''
    : cancelled ? 'cancelled'
    : ['failed', 'error'].includes(status) || outcomeRow?.isError === true ? (resultText.trim().slice(0, 200) || 'failed')
    : 'provided as a one-read path'
  const title = label ? `Secret: ${label}` : 'Secret'
  return {
    ...tool,
    title: outcome ? `${title} · ${outcome}` : title,
    content: [label && `Label: ${label}`, reason && `Reason: ${reason}`, `Outcome: ${outcome || 'waiting for you'}`].filter(Boolean).join('\n'),
    contentDeferred: false,
  }
}
