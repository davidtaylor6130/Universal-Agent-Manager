import { readFileSync } from 'node:fs'
import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it } from 'vitest'
import { useAppStore, type AcpBinding } from '../../store/useAppStore'
import type { Session } from '../../types/session'
import { SessionItem, chatRowStatusText } from './SessionItem'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const initial = useAppStore.getState()
const now = new Date()
const chat = (id: string, name: string, extra: Partial<Session> = {}): Session =>
  ({ id, name, viewMode: 'chat', folderId: null, createdAt: now, updatedAt: now, lastOpenedAt: now, ...extra })
const binding = (overrides: Partial<AcpBinding>) => overrides as AcpBinding

function render(node: React.ReactNode) {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(node))
  return { host, unmount: () => { act(() => root.unmount()); host.remove() } }
}

const openElement = (host: ParentNode, id: string) => host.querySelector<HTMLElement>(`[data-session-id="${id}"]`)!
const visualRow = (host: ParentNode, id: string) => openElement(host, id).closest<HTMLElement>('.uam-sb-row')!

afterEach(() => { useAppStore.setState(initial, true) })

describe('sidebar chat row accessibility', () => {
  it('names each row with the status its icons show: running, waiting, unread and failed', () => {
    expect(chatRowStatusText({ type: 'processing' })).toBe('running')
    expect(chatRowStatusText({ type: 'attention', kind: 'permission' })).toBe('waiting, permission needed')
    expect(chatRowStatusText({ type: 'attention', kind: 'question' })).toBe('waiting, needs answer')
    expect(chatRowStatusText({ type: 'attention', kind: 'error' })).toBe('failed')
    expect(chatRowStatusText({ type: 'done' })).toBe('done, unread')
    expect(chatRowStatusText(null, { automationHeld: true })).toBe('waiting for approval')
    expect(chatRowStatusText({ type: 'done' }, { watchingPullRequest: true })).toBe('watching pull request')
    expect(chatRowStatusText(null)).toBe('')

    useAppStore.setState({
      sessions: [chat('run', 'Runner'), chat('ask', 'Asker'), chat('fin', 'Finisher'), chat('idle', 'Idle'), chat('nap', 'Napper', { snoozedUntilMs: Date.now() + 3_600_000 })],
      activeSessionId: null,
      acpBindingBySessionId: {
        run: binding({ processing: true, lifecycleState: 'processing' }),
        ask: binding({ attentionKind: 'permission' }),
        fin: binding({ readySinceLastSelect: true }),
      },
      cliBindingBySessionId: {},
    })
    for (const activityLayout of [false, true]) {
      const { host, unmount } = render(<>{['run', 'ask', 'fin', 'idle', 'nap'].map((id) => <SessionItem key={id} sessionId={id} activityLayout={activityLayout} />)}</>)
      try {
        expect(openElement(host, 'run').getAttribute('aria-label')).toBe('Open chat Runner, running')
        expect(openElement(host, 'ask').getAttribute('aria-label')).toBe('Open chat Asker, waiting, permission needed')
        expect(openElement(host, 'fin').getAttribute('aria-label')).toBe('Open chat Finisher, done, unread')
        expect(openElement(host, 'idle').getAttribute('aria-label')).toBe('Open chat Idle')
        if (!activityLayout) expect(openElement(host, 'nap').getAttribute('aria-label')).toMatch(/^Open chat Napper, snoozed until /)
        // Status is in each name; no row is its own live region.
        expect(host.querySelector('[role="status"], [aria-live]')).toBeNull()
      } finally { unmount() }
    }
  })

  it('keeps one interactive element per row, with every action button beside it', () => {
    useAppStore.setState({
      sessions: [chat('pinned', 'Pinned one', { isPinned: true, snoozedUntilMs: Date.now() + 3_600_000 }), chat('done', 'Done one')],
      activeSessionId: null,
      acpBindingBySessionId: { done: binding({ readySinceLastSelect: true }) },
      cliBindingBySessionId: {},
    })
    for (const activityLayout of [false, true]) {
      const { host, unmount } = render(<>
        <SessionItem sessionId="pinned" activityLayout={activityLayout} />
        <SessionItem sessionId="done" activityLayout={activityLayout} />
      </>)
      try {
        for (const id of ['pinned', 'done']) {
          const open = openElement(host, id)
          expect(open.getAttribute('role')).toBe('button')
          expect(open.tabIndex).toBe(0)
          expect(open.querySelector('button, input, a[href], [role="button"], [tabindex]')).toBeNull()
          const actions = Array.from(visualRow(host, id).querySelectorAll('button'))
          expect(actions.length).toBeGreaterThan(0)
          for (const action of actions) expect(open.contains(action)).toBe(false)
        }
      } finally { unmount() }
    }
  })

  it('gives every row action a name that includes its chat', () => {
    useAppStore.setState({
      sessions: [chat('a', 'Alpha', { isPinned: true, snoozedUntilMs: Date.now() + 3_600_000 }), chat('b', 'Beta', { isPinned: true, snoozedUntilMs: Date.now() + 3_600_000 })],
      activeSessionId: null,
      acpBindingBySessionId: { a: binding({ readySinceLastSelect: true }), b: binding({ readySinceLastSelect: true }) },
      cliBindingBySessionId: {},
    })
    for (const activityLayout of [false, true]) {
      const { host, unmount } = render(<>
        <SessionItem sessionId="a" activityLayout={activityLayout} />
        <SessionItem sessionId="b" activityLayout={activityLayout} />
      </>)
      try {
        const names = (id: string, name: string) => {
          const labels = Array.from(visualRow(host, id).querySelectorAll('button')).map((button) => button.getAttribute('aria-label') ?? '')
          for (const label of labels) expect(label).toContain(name)
          return labels
        }
        const alpha = names('a', 'Alpha')
        const beta = names('b', 'Beta')
        expect(alpha.filter((label) => beta.includes(label))).toEqual([])
        if (!activityLayout) expect(alpha).toEqual(expect.arrayContaining(['Unpin Alpha', 'Unsnooze Alpha', 'More actions for Alpha']))
        else expect(alpha).toEqual(['Mark Alpha done', 'Snooze Alpha'])
      } finally { unmount() }
    }
  })

  it('uses the IconButton primitive at a 24px hit area for sidebar icon controls', () => {
    useAppStore.setState({ sessions: [chat('p', 'Pinned', { isPinned: true })], activeSessionId: null, acpBindingBySessionId: {}, cliBindingBySessionId: {} })
    const { host, unmount } = render(<SessionItem sessionId="p" />)
    try {
      for (const label of ['Unpin Pinned', 'More actions for Pinned']) {
        const button = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((candidate) => candidate.getAttribute('aria-label') === label)!
        expect(button.classList.contains('uam-iconbtn')).toBe(true)
        expect(button.classList.contains('uam-sb-iconbtn')).toBe(true)
      }
      // The inline pin keeps its 12px place in the row while its hit area is 24px.
      const inlinePin = host.querySelectorAll<HTMLButtonElement>('button[aria-label="Unpin Pinned"]')[0]
      expect(inlinePin.classList.contains('uam-sb-iconbtn--inline')).toBe(true)
      expect(inlinePin.style.getPropertyValue('--sb-paint')).toBe('12px')
    } finally { unmount() }

    // A variable path keeps Vite from rewriting the URL into an asset import.
    const source = (path: string) => readFileSync(new URL(path, import.meta.url), 'utf8')
    const css = source('./sidebar.css')
    const rule = (selector: string) => css.slice(css.indexOf(`${selector} {`), css.indexOf('}', css.indexOf(`${selector} {`)))
    expect(rule('.uam-iconbtn.uam-sb-iconbtn')).toMatch(/width: 24px;\s*height: 24px;/)
    expect(rule('.uam-iconbtn.uam-sb-iconbtn--inline')).toContain('margin: calc((var(--sb-paint) - 24px) / 2)')
    expect(rule('.uam-sb-target::after')).toContain('height: max(24px, 100%)')
    // The row draws the focus ring for its one interactive element.
    expect(css).toContain('.uam-sb-row:has(> .uam-sb-row__open:focus-visible) { outline: var(--focus-ring); outline-offset: var(--focus-inset); }')
  })
})
