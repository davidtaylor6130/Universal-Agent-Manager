import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import type { PermissionCommandRule } from '../../types/permissionRules'
import { PermissionRulesSettings } from './PermissionRulesSettings'

vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const denyForce: PermissionCommandRule = { id: 'deny-force', action: 'deny', tool: 'command', pattern: 'git ** push ** --force*', providerId: '', workspace: '', enabled: true }
const readOnly: PermissionCommandRule = { id: 'read-only', action: 'allow', tool: 'readOnly', pattern: '', providerId: '', workspace: '', enabled: true }
const save = vi.fn(async (_patch: { rules?: PermissionCommandRule[]; aiReviewEnabled?: boolean }) => ({ ok: true } as { ok: boolean; error?: string }))
const testRules = vi.fn(async (_rules: PermissionCommandRule[], command: string) =>
  command.includes('--force') ? { outcome: 'deny' as const, ruleId: 'deny-force', reason: 'A deny rule matched.', readOnly: false } : { outcome: 'none' as const, ruleId: '', reason: '', readOnly: false })

const button = (name: string) => Array.from(document.querySelectorAll<HTMLButtonElement>('button')).find((item) => (item.getAttribute('aria-label') || item.textContent?.trim()) === name)!
const input = (name: string) => document.querySelector<HTMLInputElement>(`input[aria-label="${name}"]`)!
const click = async (name: string) => { await act(async () => { button(name).click() }) }
const type = (name: string, value: string) => act(() => {
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input(name), value)
  input(name).dispatchEvent(new Event('input', { bubbles: true }))
})
const choose = async (menu: string, option: string) => {
  await click(menu)
  const item = Array.from(document.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((candidate) => candidate.textContent?.includes(option))!
  await act(async () => { item.click() })
}

describe('PermissionRulesSettings', () => {
  let host: HTMLDivElement
  let root: Root
  const show = () => { host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host); act(() => root.render(<PermissionRulesSettings />)) }

  beforeEach(() => {
    save.mockClear()
    testRules.mockClear()
    useAppStore.setState({ permissionCommandRules: [], permissionAiReviewEnabled: true, providers: [], setPermissionCommandRules: save, testPermissionCommandRules: testRules })
  })
  afterEach(() => { act(() => root.unmount()); host.remove(); vi.useRealTimers() })

  it('shows the empty sentence and adds a trimmed deny rule through the editor', async () => {
    show()
    expect(document.body.textContent).toContain('No rules. Every request follows the chat’s permission level.')
    await click('Add rule')
    expect(document.querySelector('[role="dialog"]')?.getAttribute('aria-label')).toBe('New permission rule')
    expect(button('Save rule').disabled).toBe(true)
    type('Command pattern', '  rm -rf ')
    expect(button('Save rule').disabled).toBe(false)
    await click('Save rule')
    const saved = save.mock.calls[0][0].rules!
    expect(saved).toHaveLength(1)
    expect(saved[0]).toMatchObject({ action: 'deny', tool: 'command', pattern: 'rm -rf', providerId: '', workspace: '', enabled: true })
    expect(document.querySelector('[role="dialog"]')).toBeNull()
  })

  it('lists each rule with labelled actions and edits the pattern', async () => {
    useAppStore.setState({ permissionCommandRules: [readOnly, denyForce] })
    show()
    expect(document.body.textContent).toContain('Read-only commands and reads')
    expect(document.body.textContent).toContain('git ** push ** --force*')
    expect(button('Edit rule git ** push ** --force*')).toBeTruthy()
    expect(button('Delete rule Read-only commands and reads')).toBeTruthy()
    await click('Edit rule git ** push ** --force*')
    type('Command pattern', 'git ** push ** -f')
    await click('Save rule')
    expect(save.mock.calls[0][0].rules).toEqual([readOnly, { ...denyForce, pattern: 'git ** push ** -f' }])
  })

  it('blocks chained patterns and allow rules that start with a glob', async () => {
    show()
    await click('Add rule')
    type('Command pattern', 'npm test && rm -rf /')
    expect(button('Save rule').disabled).toBe(true)
    expect(document.body.textContent).toContain('A pattern is one command')
    await choose('Rule action', 'Allow')
    type('Command pattern', '** test')
    expect(button('Save rule').disabled).toBe(true)
    expect(document.body.textContent).toContain('An allow pattern must start with a program name.')
    type('Command pattern', 'npm test')
    expect(button('Save rule').disabled).toBe(false)
  })

  it('offers allow rules only for commands, read-only work and edits', async () => {
    show()
    await click('Add rule')
    await choose('Rule action', 'Allow')
    await click('Rule applies to')
    const labels = Array.from(document.querySelectorAll('[role="option"]')).map((option) => option.textContent ?? '')
    expect(labels.some((label) => label.includes('Web access'))).toBe(false)
    expect(labels.some((label) => label.includes('Any request'))).toBe(false)
    expect(labels.some((label) => label.includes('Read-only commands and reads'))).toBe(true)
  })

  it('keeps the editor and its draft when the native save refuses the rule', async () => {
    save.mockResolvedValueOnce({ ok: false, error: 'Two rules share the id x.' })
    show()
    await click('Add rule')
    type('Command pattern', 'rm')
    await click('Save rule')
    expect(document.querySelector('[role="dialog"]')).toBeTruthy()
    expect(document.body.textContent).toContain('Two rules share the id x.')
    expect(input('Command pattern').value).toBe('rm')
  })

  it('removes a rule only after confirming', async () => {
    useAppStore.setState({ permissionCommandRules: [denyForce] })
    show()
    await click('Delete rule git ** push ** --force*')
    expect(save).not.toHaveBeenCalled()
    await click('Delete')
    await act(async () => { await new Promise((resolve) => setTimeout(resolve, 400)) })
    expect(save.mock.calls[0][0].rules).toEqual([])
  })

  it('toggles a rule and AI review through the store', async () => {
    useAppStore.setState({ permissionCommandRules: [denyForce] })
    show()
    await act(async () => { input('Enable rule git ** push ** --force*').click() })
    expect(save.mock.calls[0][0].rules).toEqual([{ ...denyForce, enabled: false }])
    await act(async () => { input('Ask the AI reviewer').click() })
    expect(save.mock.calls[1][0]).toEqual({ aiReviewEnabled: false })
  })

  it('shows which rule decides a tested command', async () => {
    useAppStore.setState({ permissionCommandRules: [denyForce] })
    show()
    type('Command to test', 'git push --force origin main')
    await act(async () => { await new Promise((resolve) => setTimeout(resolve, 260)) })
    expect(testRules).toHaveBeenLastCalledWith([denyForce], 'git push --force origin main')
    expect(document.body.textContent).toContain('Denied by “git ** push ** --force*”.')
    type('Command to test', 'npm test')
    await act(async () => { await new Promise((resolve) => setTimeout(resolve, 260)) })
    expect(document.body.textContent).toContain('No rule matches. Under AI Review the reviewer decides.')
  })
})
