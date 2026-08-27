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

  it('lets the composer shrink to one line and grow natively to its cap', () => {
    expect(chatView).toContain('rows={1}')
    expect(styles).toMatch(/\.uam-composer-textarea\s*\{[^}]*field-sizing:\s*content/s)
    expect(styles).toMatch(/\.uam-composer-textarea\s*\{[^}]*min-height:\s*40px/s)
    expect(styles).toMatch(/\.uam-composer-textarea\s*\{[^}]*max-height:\s*160px/s)
    expect(styles).not.toMatch(/\.uam-chat-pane\[data-multi-pane="true"\] \.uam-composer-textarea\s*\{[^}]*(?:^|[;{]\s*)height\s*:/s)
  })

  it('keeps pane identity colour visible without requiring focus', () => {
    expect(styles).toContain('.uam-chat-pane[data-multi-pane="true"] > .uam-chat-pane__header')
    expect(styles).not.toContain('.uam-chat-pane[data-multi-pane="true"][data-focused="true"] > .uam-chat-pane__header')
  })

  it('bounds expanded goal details to the pane and gives them their own scroll region', () => {
    expect(styles).toMatch(/\.uam-chat-pane\s*\{[^}]*container-type:\s*size/s)
    expect(styles).toMatch(/\.uam-goal-banner__details\s*\{[^}]*max-height:\s*min\([^;]*100cqh/s)
    expect(styles).toMatch(/\.uam-goal-banner__details\s*\{[^}]*overflow-y:\s*auto/s)
    expect(styles).toMatch(/\.uam-goal-banner__details\s*\{[^}]*overscroll-behavior:\s*contain/s)
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
    expect(styles).toMatch(/\.uam-chat-transcript \.uam-chat-content\s*\{[^}]*padding-inline:\s*6px/s)
    expect(styles).not.toMatch(/\.uam-chat-content\s*\{[^}]*max-width/s)
    expect(styles).toMatch(/\.uam-composer-region\s*\{[^}]*max-width:\s*960px/s)
    expect(styles).toMatch(/\.uam-composer-region\s*\{[^}]*margin-inline:\s*auto/s)
    expect(styles).toMatch(/\.uam-message-frame\[data-message-kind="user"\]\s*\{[^}]*width:\s*fit-content/s)
    expect(styles).toMatch(/\.uam-message-frame\[data-message-kind="user"\]\s*\{[^}]*max-width:\s*75%/s)
    expect(styles).toMatch(/\.uam-message-frame\[data-message-kind="assistant"\]\s*\{[^}]*max-width:\s*75%/s)
    expect(styles).toMatch(/\.uam-chat-pane\[data-multi-pane="true"\] \.uam-message-frame\[data-message-kind="user"\]\s*\{[^}]*max-width:\s*75%/s)
    expect(styles).toMatch(/\.uam-chat-pane\[data-multi-pane="true"\] \.uam-message-frame\[data-message-kind="assistant"\]\s*\{[^}]*max-width:\s*75%/s)
    expect(styles).toMatch(/\.uam-chat-pane\[data-multi-pane="true"\] \.uam-chat-transcript \.uam-chat-content\s*\{[^}]*padding-inline:\s*5px/s)
    expect(chatView).toContain('uam-chat-content uam-composer-region flex flex-col p-3')
  })
})
