import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { describe, expect, it } from 'vitest'

// jsdom cannot evaluate @media (forced-colors: active), so these check the rules themselves: in forced colours the
// browser paints author backgrounds as Canvas and drops gradients, so a mark drawn only by a fill disappears unless a
// forced-colours rule opts it out and gives it a system colour.
const css = (path: string) => readFileSync(resolve(process.cwd(), path), 'utf8')
/** Every rule inside each `@media (forced-colors: active)` block of a stylesheet. */
const forcedRules = (sheet: string) => {
  const rules: Array<{ selector: string; body: string }> = []
  for (const start of [...sheet.matchAll(/@media \(forced-colors: active\) \{/g)].map((match) => match.index! + match[0].length)) {
    let depth = 1
    let end = start
    while (depth > 0 && end < sheet.length) { if (sheet[end] === '{') depth++; else if (sheet[end] === '}') depth--; end++ }
    for (const rule of sheet.slice(start, end - 1).matchAll(/([^{}]+)\{([^{}]*)\}/g)) rules.push({ selector: rule[1].replace(/\/\*[\s\S]*?\*\//g, '').trim(), body: rule[2] })
  }
  return rules
}
const SYSTEM = /\b(CanvasText|Canvas|ButtonText|ButtonFace|Highlight|HighlightText|GrayText)\b/
const declares = (rules: ReturnType<typeof forcedRules>, selector: RegExp, property: RegExp) =>
  rules.filter((rule) => selector.test(rule.selector) && property.test(rule.body))

describe('forced colours', () => {
  const index = forcedRules(css('src/index.css'))
  const conversation = forcedRules(css('src/components/chat/conversation.css'))

  it('keeps the Switch thumb and the on track visible in system colours', () => {
    expect(declares(index, /\.uam-switch__track/, /forced-color-adjust:\s*none/)).not.toHaveLength(0)
    const thumb = declares(index, /\.uam-switch__thumb/, /background:/)
    expect(thumb.length).toBeGreaterThan(0)
    expect(thumb.every((rule) => SYSTEM.test(rule.body.match(/background:([^;]*)/)![1]))).toBe(true)
    expect(declares(index, /input:checked \+ \.uam-switch__track$/, /background:\s*Highlight/)).toHaveLength(1)
  })

  it('greys out a disabled Switch, on or off and inside Settings', () => {
    expect(declares(index, /input:disabled:checked \+ \.uam-switch__track$/m, /background:\s*ButtonFace/)).toHaveLength(1)
    expect(declares(index, /\.uam-settings-scope \.uam-switch input:disabled:checked \+ \.uam-switch__track \.uam-switch__thumb/, /background:\s*GrayText/)).toHaveLength(1)
  })

  it('draws the Tabs indicator in Highlight for both variants', () => {
    expect(declares(index, /^\.uam-tabs__indicator$/, /forced-color-adjust:\s*none/)).toHaveLength(1)
    expect(declares(index, /data-variant="underline"\] \.uam-tabs__indicator/, /background:\s*Highlight/)).toHaveLength(1)
    expect(declares(index, /data-variant="pill"\] \.uam-tabs__indicator/, /border-color:\s*Highlight/)).toHaveLength(1)
  })

  it('keeps the chosen selection card distinct with Highlight', () => {
    expect(declares(index, /\.uam-selection-option\[aria-pressed="true"\]/, /border-color:\s*Highlight/)).toHaveLength(1)
  })

  it('gives the message rail marks system colours through the properties their inline styles read', () => {
    const track = conversation.find((rule) => rule.selector === '.conversation-rail-track')!
    expect(track.body).toMatch(/--rail-tick:\s*GrayText/)
    expect(track.body).toMatch(/--rail-lit:\s*CanvasText/)
    expect(track.body).toMatch(/--rail-target:\s*Highlight/)
    expect(declares(conversation, /^\.conversation-rail-track > span$/, /forced-color-adjust:\s*none/)).toHaveLength(1)
  })
})
