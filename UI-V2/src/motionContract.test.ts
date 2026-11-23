import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'

// Static contract for the shared motion surfaces: panels, menus, the settings selection and sidebar rows.
// jsdom does not run CSS animations, so these rules are checked in the stylesheet itself.
const source = (path: string) => readFileSync(new URL(path, import.meta.url), 'utf8')
const styles = source('./index.css')
const viewportMenu = source('./components/ui/viewport-menu.css')
const conversation = source('./components/chat/conversation.css')
const companion = source('./components/companion/companion.css')
const settingsShell = source('./components/settings/settings-shell.css')
const sidebar = source('./components/sidebar/sidebar.css')
const sidebarGlide = source('./components/layout/SidebarGlide.css')
const mainPanel = source('./components/layout/MainPanel.css')
const sessionItem = source('./components/sidebar/SessionItem.tsx')
const usage = source('./components/usage/usage.css')
const providerSettings = source('./components/settings/provider-settings.css')
const review = source('./components/shared/review.css')
const notice = source('./components/ui/notice-motion.css')
const noticeComponent = source('./components/ui/Notice.tsx')

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
  it('uses scaled shared timings for notices and their details control', () => {
    expect(rule(notice, '.uam-notice')).toContain('uam-notice-enter var(--dur-enter) var(--ease-out-soft)')
    expect(rule(notice, '.uam-notice[data-state="closed"]')).toContain('uam-notice-exit var(--dur-exit) var(--ease)')
    expect(rule(notice, '.uam-notice__chevron')).toContain('transform var(--dur-fast) var(--ease)')
    expect(noticeComponent).toContain('className="uam-notice__chevron inline-block"')
    expect(noticeComponent).not.toContain('duration-150')
  })

  it('scales every token to zero under Motion Off', () => {
    expect(styles).toMatch(/\[data-motion="off"\]\s*\{\s*--motion-scale:\s*0;/)
    for (const token of ['--dur-fast', '--dur-base', '--dur-enter', '--dur-exit']) {
      expect(styles).toMatch(new RegExp(`${token}:\\s*calc\\(\\d+ms \\* var\\(--motion-scale, 1\\)\\)`))
    }
  })

  it('animates shared button loading with the selected motion speed', () => {
    expect(rule(styles, '.uam-btn__spinner')).toContain('uam-status-spin calc(1000ms * var(--motion-scale, 1))')
    expect(rule(styles, '.animate-spin')).toContain('animation-duration: calc(1000ms * var(--motion-scale, 1))')
    expect(styles.indexOf('@tailwind utilities')).toBeLessThan(styles.indexOf('\n.animate-spin {'))
    for (const selector of ['.session-status--processing > span', '.uam-working-ring', '.uam-reference-pending__spinner']) {
      expect(rule(styles, selector)).toContain('uam-status-spin calc(1000ms * var(--motion-scale, 1))')
    }
    expect(rule(usage, '.uam-usage-refresh.is-busy svg')).toContain('uam-status-spin calc(1000ms * var(--motion-scale, 1))')
    expect(rule(providerSettings, '.uam-prov-spin')).toContain('uam-prov-spin calc(900ms * var(--motion-scale, 1))')
    expect(rule(review, '.uam-rv-spin')).toContain('uam-rv-spin calc(900ms * var(--motion-scale, 1))')
  })

  it('scales indeterminate progress sweeps with the selected motion speed', () => {
    for (const selector of ['.uam-progress-fill[data-indeterminate]::before', '.uam-meter[data-indeterminate]::before']) {
      expect(rule(styles, selector)).toContain('uam-progress-band calc(1400ms * var(--motion-scale, 1))')
    }
    // The meter uses the global pseudo-element stop; the filled row also has an explicit stop.
    expect(styles).toMatch(/\[data-motion="off"\] \*::before[^}]*animation-duration: \.01ms !important;[^}]*animation-iteration-count: 1 !important/s)
    expect(styles).toMatch(/@media \(prefers-reduced-motion: reduce\)\s*\{\s*\*, \*::before, \*::after \{[^}]*animation-duration: \.01ms !important;[^}]*animation-iteration-count: 1 !important/s)
    expect(styles).toMatch(/@media \(prefers-reduced-motion: reduce\)\s*\{\s*\.uam-progress-fill::before \{[^}]*\}\s*\.uam-progress-fill\[data-indeterminate\]::before \{ animation: none; \}/s)
  })

  it('sweeps every loading placeholder through one compositor-only shared skeleton that follows motion speed', () => {
    expect(rule(styles, '.uam-skeleton')).toContain('background: var(--surface-up)')
    const sweep = rule(styles, '.uam-skeleton::after')
    expect(sweep).toContain('uam-skeleton-sweep calc(1400ms * var(--motion-scale, 1)) ease-in-out infinite')
    expect(sweep).toContain('transform: translateX(-100%)')
    expect(animatedProperties(keyframes(styles, 'uam-skeleton-sweep'))).toEqual(['transform'])
    expect(styles).toMatch(/\[data-motion="off"\] \.uam-skeleton::after \{ animation: none; \}/)
    expect(styles).toMatch(/@media \(prefers-reduced-motion: reduce\) \{ \.uam-skeleton::after \{ animation: none; \} \}/)
    // Feature placeholders only size the shared block; none repaints a background-position shimmer of its own.
    for (const [sheet, selector] of [[styles, '.uam-icon-skeleton'], [settingsShell, '.uam-set-skeleton'],
      [conversation, '.conversation-skeleton__bubble, .conversation-skeleton__line']] as const) {
      expect(rule(sheet, selector)).not.toMatch(/animation|gradient/)
    }
    for (const sheet of [styles, settingsShell, conversation]) expect(sheet).not.toMatch(/@keyframes [a-z-]*shimmer/)
    for (const [file, family] of [['./components/layout/Sidebar.tsx', 'uam-icon-skeleton'], ['./components/shared/CustomIcon.tsx', 'uam-icon-skeleton'],
      ['./components/settings/SettingsPrimitives.tsx', 'uam-set-skeleton'], ['./components/views/ChatView.tsx', 'conversation-skeleton__'],
      ['./components/chat/SubagentTranscript.tsx', 'conversation-skeleton__']]) {
      const uses = [...source(file).matchAll(/className="([^"]*)"/g)].map((match) => match[1]).filter((name) => name.includes(family) && !name.includes('__text'))
      expect(uses.length, file).toBeGreaterThan(0)
      for (const name of uses) expect(name.split(' '), `${file}: ${name}`).toContain('uam-skeleton')
    }
  })

  it('scales coordinated status-scene timing and offsets together', () => {
    for (const [selector, time] of [
      ['.status-scene__roller', '1200ms'], ['.status-scene__press', '600ms'],
      ['.status-scene__box', '1800ms'], ['.status-scene__box--2', '-600ms'],
      ['.status-scene__box--3', '-1200ms'], ['.status-agent__antenna-tip', '1600ms'],
      ['.status-agent__screen', '900ms'], ['.status-agent__body', '500ms'],
      ['.status-agent--asleep .status-agent__z', '2400ms'], ['.status-bed__blanket', '3200ms'],
      ['.status-bed__z', '3200ms'], ['.status-bed__z--2', '1600ms'],
      ['.status-page__line', '1800ms'], ['.status-page__line--2', '300ms'],
      ['.status-page__line--3', '600ms'],
    ]) expect(rule(conversation, selector)).toContain(`calc(${time} * var(--motion-scale, 1))`)
    for (const selector of ['.status-agent__antenna-tip', '.status-agent__screen', '.status-agent__body', '.status-agent--asleep .status-agent__z']) {
      const block = rule(conversation, selector)
      expect(block).toContain('calc(var(--agent-delay, 0ms) * var(--motion-scale, 1))')
      expect(block.indexOf('animation:')).toBeLessThan(block.indexOf('animation-delay:'))
    }
    const time = (selector: string) => Number(rule(conversation, selector).match(/calc\((-?\d+)ms \* var\(--motion-scale, 1\)\)/)?.[1])
    const beat = time('.status-scene__press')
    expect(time('.status-scene__box')).toBe(3 * beat)
    expect(time('.status-scene__box--2')).toBe(-beat)
    expect(time('.status-scene__box--3')).toBe(-2 * beat)
    expect(conversation.indexOf('\n.status-scene__box {')).toBeLessThan(conversation.indexOf('\n.status-scene__box--2 {'))
    expect(conversation.indexOf('\n.status-scene__box {')).toBeLessThan(conversation.indexOf('\n.status-scene__box--3 {'))
    expect(rule(conversation, '.status-page__line')).toContain('infinite backwards')
    expect(rule(conversation, '.conversation-work__live')).toContain('calc(1400ms * var(--motion-scale, 1))')
  })

  it('collapses every transition and animation under reduced motion', () => {
    expect(styles).toMatch(/@media \(prefers-reduced-motion: reduce\)\s*\{\s*\*, \*::before, \*::after \{[^}]*animation-duration: \.01ms !important;[^}]*transition-duration: \.01ms !important;/s)
    expect(settingsShell).toMatch(/@media \(prefers-reduced-motion: reduce\)\s*\{\s*\.uam-settings-nav__indicator \{ transition: none; \}/)
    expect(sidebar).toMatch(/\.uam-sb-row \{\s*animation: none;\s*transition: none;\s*\}/)
  })

  it('retains usage loading thresholds when motion is disabled', () => {
    expect(rule(usage, '[data-motion="off"] .uam-usage-skeleton')).toContain('animation-delay: 250ms !important')
    expect(rule(usage, '[data-motion="off"] .uam-usage-body[data-kept]')).toContain('transition-delay: 250ms !important')
    const reduced = usage.slice(usage.indexOf('@media (prefers-reduced-motion: reduce)'))
    expect(reduced).toMatch(/\.uam-usage-skeleton\s*\{\s*animation-delay: 250ms !important;/)
    expect(reduced).toMatch(/\.uam-usage-body\[data-kept\]\s*\{\s*transition-delay: 250ms !important;/)
    expect(usage).toMatch(/\.uam-usage-skel-tile:not\(\.uam-usage-skel-label\),\s*\.uam-usage-skel-chart\s*\{\s*animation: uam-usage-skeleton-pulse/)
    expect(usage).toMatch(/\[data-motion="off"\] \.uam-usage-skel-tile:not\(\.uam-usage-skel-label\),\s*\[data-motion="off"\] \.uam-usage-skel-chart\s*\{\s*animation: none;/)
    expect(reduced).toMatch(/\.uam-usage-skel-tile:not\(\.uam-usage-skel-label\),\s*\.uam-usage-skel-chart\s*\{\s*animation: none;/)
    expect(usage).toContain('uam-usage-skeleton-pulse calc(1500ms * var(--motion-scale, 1))')
  })

  it('animates panels, menus and the settings selection with transform and opacity only', () => {
    for (const name of ['uam-panel-in', 'uam-panel-out', 'uam-tooltip-out']) {
      expect(animatedProperties(keyframes(styles, name)).every((property) => property === 'opacity' || property === 'transform'), name).toBe(true)
    }
    for (const name of ['uam-vmenu-in', 'uam-vmenu-out']) {
      expect(animatedProperties(keyframes(viewportMenu, name)).every((property) => property === 'opacity' || property === 'transform'), name).toBe(true)
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
      rule(viewportMenu, '.uam-viewport-menu[data-viewport-menu]'),
      rule(viewportMenu, '.uam-viewport-menu[data-viewport-menu][data-state="closed"]'),
      rule(sidebarGlide, '.uam-sidebar-glide .uam-folder-expand-glide'),
      rule(mainPanel, '.uam-pane-glide'),
    ]
    for (const block of timed) {
      expect(block).toMatch(/var\(--dur-(fast|base|enter|exit)\)/)
      expect(block).not.toMatch(/\b\d+m?s\b/)
      expect(block).not.toContain('infinite')
    }
    // Exits are faster than entrances.
    expect(rule(viewportMenu, '.uam-viewport-menu[data-viewport-menu]')).toContain('var(--dur-base)')
    expect(rule(viewportMenu, '.uam-viewport-menu[data-viewport-menu][data-state="closed"]')).toContain('var(--dur-fast)')
    expect(rule(styles, '.uam-tooltip')).toContain('var(--dur-base)')
    expect(rule(styles, '.uam-tooltip[data-state="closed"]')).toContain('var(--dur-fast)')
    expect(rule(conversation, '.chat-status-strip[data-state="closed"]')).toContain('var(--dur-exit)')
    expect(rule(companion, '.uam-companion-nav-indicator')).toContain('transition: transform var(--dur-base)')
    expect(rule(companion, '.uam-companion-page-track')).toContain('transition: transform var(--dur-base)')
    expect(rule(styles, '.uam-shell-panel[data-state="closed"]')).toContain('var(--dur-exit)')
  })
})
