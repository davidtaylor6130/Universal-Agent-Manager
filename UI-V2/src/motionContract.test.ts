import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'

// Static contract for the shared motion surfaces: panels, menus, the settings selection and sidebar rows.
// jsdom does not run CSS animations, so these rules are checked in the stylesheet itself.
const source = (path: string) => readFileSync(new URL(path, import.meta.url), 'utf8')
const styles = source('./index.css')
const settingsShell = source('./components/settings/settings-shell.css')
const sidebar = source('./components/sidebar/sidebar.css')
const sidebarGlide = source('./components/layout/SidebarGlide.css')
const mainPanel = source('./components/layout/MainPanel.css')
const sessionItem = source('./components/sidebar/SessionItem.tsx')

// Top-level rules only (unindented), so media-query overrides are not picked up instead.
const rule = (sheet: string, selector: string) => {
  const css = `\n${sheet}`
  const start = css.indexOf(`\n${selector} {`)
  expect(start, selector).toBeGreaterThanOrEqual(0)
  return css.slice(start, css.indexOf('}', start) + 1)
}
const keyframes = (css: string, name: string) => {
  const start = css.indexOf(`@keyframes ${name} {`)
  expect(start, name).toBeGreaterThanOrEqual(0)
  let depth = 0
  for (let index = css.indexOf('{', start); index < css.length; index += 1) {
    if (css[index] === '{') depth += 1
    if (css[index] === '}' && --depth === 0) return css.slice(start, index + 1)
  }
  return ''
}
const animatedProperties = (block: string) => [...block.matchAll(/([a-z-]+)\s*:/g)].map((match) => match[1])

describe('shared motion contract', () => {
  it('scales every token to zero under Motion Off', () => {
    expect(styles).toMatch(/\[data-motion="off"\]\s*\{\s*--motion-scale:\s*0;/)
    for (const token of ['--dur-fast', '--dur-base', '--dur-enter', '--dur-exit']) {
      expect(styles).toMatch(new RegExp(`${token}:\\s*calc\\(\\d+ms \\* var\\(--motion-scale, 1\\)\\)`))
    }
  })

  it('collapses every transition and animation under reduced motion', () => {
    expect(styles).toMatch(/@media \(prefers-reduced-motion: reduce\)\s*\{\s*\*, \*::before, \*::after \{[^}]*animation-duration: \.01ms !important;[^}]*transition-duration: \.01ms !important;/s)
    expect(settingsShell).toMatch(/@media \(prefers-reduced-motion: reduce\)\s*\{\s*\.uam-settings-nav__indicator \{ transition: none; \}/)
    expect(sidebar).toMatch(/\.uam-sb-row \{\s*animation: none;\s*transition: none;\s*\}/)
  })

  it('animates panels, menus and the settings selection with transform and opacity only', () => {
    for (const name of ['uam-panel-in', 'uam-panel-out', 'uam-menu-in', 'uam-menu-out']) {
      expect(animatedProperties(keyframes(styles, name)).every((property) => property === 'opacity' || property === 'transform'), name).toBe(true)
    }
    expect(styles).not.toMatch(/@keyframes uam-panel-(grow|shrink)/)
    // Drawer: panels travel their full width into the rail, which stays above them.
    expect(keyframes(styles, 'uam-panel-out')).toContain('translateX(calc(var(--panel-dir) * 100%))')
    const z = (css: string) => Number(/z-index: (\d+)/.exec(css)?.[1])
    const closingPanel = /\.uam-shell-panel\[data-state="closed"\] \{ position: absolute;[^}]*\}/.exec(styles)?.[0] ?? ''
    expect(z(closingPanel)).toBeLessThan(z(rule(styles, '.uam-side-rail')))
    expect(animatedProperties(keyframes(styles, 'uam-handle-in'))).toEqual(['opacity'])
    expect(rule(settingsShell, '.uam-settings-nav__indicator')).toContain('transition: transform var(--dur-enter) var(--ease-out-soft);')
    expect(rule(sidebar, '.uam-sb-row')).toContain('transition: background-color var(--dur-fast) var(--ease-out-soft), color var(--dur-fast) var(--ease-out-soft);')
    expect(sessionItem).not.toMatch(/uam-sb-row[^`]*transition-all/)
  })

  it('changes only paint when a sidebar row is selected', () => {
    // A selection rule that swaps the row's animation restarts its entrance, so the old row blinks out on deselect.
    expect(sidebarGlide).not.toMatch(/\[data-session-id\]\[(aria-current|data-selected)[^{]*\{[^}]*animation/)
    expect(sidebar).not.toMatch(/\.uam-sb-row--selected[^{]*\{[^}]*animation/)
  })

  it('takes a closing docked panel out of the layout instead of animating its width', () => {
    expect(styles).toMatch(/@media not all and \(max-width: 1000px\) \{\s*\/\*[^*]*\*\/\s*\.uam-shell-panel\[data-state="closed"\] \{ position: absolute; top: var\(--uam-window-strip-height, 0px\); bottom: 0; height: auto; z-index: 20; \}/)
    expect(styles).not.toMatch(/\.uam-shell-panel[^{]*\{[^}]*margin-inline/)
  })

  it('uses the shared duration tokens so Appearance motion speed and Off apply', () => {
    const timed = [
      rule(styles, '.uam-shell-panel'),
      rule(styles, '.uam-shell-panel[data-state="closed"]'),
      rule(styles, '[data-viewport-menu]'),
      rule(styles, '[data-viewport-menu][data-state="closed"]'),
      rule(sidebarGlide, '.uam-sidebar-glide .uam-folder-expand-glide'),
      rule(mainPanel, '.uam-pane-glide'),
    ]
    for (const block of timed) {
      expect(block).toMatch(/var\(--dur-(fast|base|enter|exit)\)/)
      expect(block).not.toMatch(/\b\d+m?s\b/)
      expect(block).not.toContain('infinite')
    }
    // Exits are faster than entrances.
    expect(rule(styles, '[data-viewport-menu][data-state="closed"]')).toContain('var(--dur-fast)')
    expect(rule(styles, '.uam-shell-panel[data-state="closed"]')).toContain('var(--dur-exit)')
  })
})
