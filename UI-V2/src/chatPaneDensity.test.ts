import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'

const source = (path: string) => readFileSync(new URL(path, import.meta.url), 'utf8')
const styles = source('./index.css')
const mainPanel = source('./components/layout/MainPanel.tsx')
const chatView = source('./components/views/ChatView.tsx')

describe('multi-pane density contract', () => {
  it('scopes dense presentation to narrow multi-pane containers', () => {
    expect(mainPanel).toContain('data-multi-pane={multiPane}')
    expect(styles).toContain('@container (max-width: 720px)')
    expect(styles).toContain('.uam-chat-pane[data-multi-pane="true"] > .uam-chat-pane__header')
    expect(styles).toContain('.uam-chat-pane[data-multi-pane="true"] .uam-chat-transcript')
    expect(styles).toContain('.uam-chat-pane[data-multi-pane="true"] .uam-composer-region')
    expect(styles).not.toContain('.uam-chat-pane[data-multi-pane="false"]')
  })

  it('keeps compact composer controls reachable at 320px', () => {
    expect(styles).not.toMatch(/\.uam-composer-secondary-control\s*\{\s*display:\s*none/)
    expect(styles).toContain('@container (max-width: 320px)')
    expect(styles).toMatch(/\.uam-chat-pane\[data-multi-pane="true"\] \.uam-composer-toolbar\s*\{[^}]*overflow-x:\s*auto/s)
  })

  it('gives live and recovery rows stable compact hooks without removing semantics', () => {
    expect(chatView).toContain('uam-turn-starting')
    expect(chatView).toContain('uam-queued-prompt')
    expect(chatView).toContain('uam-composer-textarea')
    expect(chatView).toContain('role="status"')
    expect(chatView).toContain('aria-label="Queued prompt"')
  })

  it('keeps tool events compact but visibly distinct from prose', () => {
    expect(styles).toMatch(/\.uam-tool-row\s*\{[^}]*grid-template-columns:\s*auto auto minmax\(0,\s*1fr\) auto/s)
    expect(styles).toMatch(/\.uam-tool-row\s*\{[^}]*border:\s*1px solid var\(--border\)/s)
    expect(styles).toMatch(/\.uam-chat-pane\[data-multi-pane="true"\] \.uam-tool-row\s*\{[^}]*min-height:\s*26px/s)
  })

  it('uses only opposite-edge message gutters while keeping the composer inset', () => {
    expect(chatView).toContain('uam-chat-content w-full py-4')
    expect(chatView).not.toContain('uam-chat-content w-full px-4 py-4')
    expect(styles).toMatch(/\.uam-message-frame\[data-message-kind="user"\]\s*\{[^}]*width:\s*fit-content/s)
    expect(styles).toMatch(/\.uam-message-frame\[data-message-kind="user"\]\s*\{[^}]*max-width:\s*calc\(100% - 20px\)/s)
    expect(styles).toMatch(/\.uam-message-frame\[data-message-kind="assistant"\]\s*\{[^}]*max-width:\s*calc\(100% - 20px\)/s)
    expect(styles).toMatch(/\.uam-chat-pane\[data-multi-pane="true"\] \.uam-chat-transcript \.uam-chat-content\s*\{[^}]*padding-inline:\s*10px/s)
    expect(chatView).toContain('uam-chat-content uam-composer-region flex flex-col p-3')
  })
})
