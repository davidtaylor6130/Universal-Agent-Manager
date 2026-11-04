// @vitest-environment node
import { readFileSync } from 'node:fs'
import { expect, it } from 'vitest'

const css = readFileSync(new URL('./usage.css', import.meta.url), 'utf8')

// The column tooltips are absolutely positioned; a fixed or shrink-fit width clips their nowrap values.
function rule(selector: string) {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
  const match = css.match(new RegExp(`^${escaped}\\s*\\{([^}]*)\\}`, 'm'))
  return match?.[1] ?? ''
}
// Values of every width / min-width / max-width declaration in a rule body.
const widths = (body: string) => [...body.matchAll(/(?:^|[;{\s])(?:min-|max-)?width:\s*([^;]+);/g)].map(m => m[1].trim())

it('sizes usage column tooltips to their content instead of a fixed or shrink-fit width', () => {
  expect(widths(rule('.uam-usage-col-tip'))).toEqual(['max-content'])
  expect(widths(rule('.uam-usage-col-tip--split'))).toEqual([])
  expect(rule('.uam-usage-tip-cols')).not.toMatch(/minmax\(0/)
})
