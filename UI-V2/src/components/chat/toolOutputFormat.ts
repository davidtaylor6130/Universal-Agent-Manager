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
