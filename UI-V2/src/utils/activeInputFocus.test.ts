import { afterEach, describe, expect, it, vi } from 'vitest'
import { focusActiveInput, FOCUS_ACTIVE_INPUT_EVENT } from './activeInputFocus'

afterEach(() => document.body.replaceChildren())
describe('active input focus', () => {
  it('focuses only the active composer and preserves its draft and caret', () => {
    document.body.innerHTML = '<div class="uam-chat-pane" data-focused="false"><textarea class="uam-composer-textarea">Inactive</textarea></div><div class="uam-chat-pane" data-focused="true"><textarea class="uam-composer-textarea">Active draft</textarea></div>'
    const inputs = document.querySelectorAll('textarea')
    inputs[1].setSelectionRange(3, 5)
    expect(focusActiveInput()).toBe(true)
    expect(document.activeElement).toBe(inputs[1])
    expect(inputs[1].value).toBe('Active draft')
    expect([inputs[1].selectionStart, inputs[1].selectionEnd]).toEqual([3, 5])
  })
  it('refuses modal, disabled, read-only, hidden and ambiguous panes without falling back', () => {
    document.body.innerHTML = '<button>Owner</button><div class="uam-chat-pane" data-focused="true"><textarea class="uam-composer-textarea"></textarea></div>'
    const owner = document.querySelector('button')!
    const pane = document.querySelector<HTMLElement>('.uam-chat-pane')!
    const input = pane.querySelector('textarea')!
    owner.focus()
    input.disabled = true
    expect(focusActiveInput()).toBe(false)
    input.disabled = false; input.readOnly = true
    expect(focusActiveInput()).toBe(false)
    input.readOnly = false; pane.hidden = true
    expect(focusActiveInput()).toBe(false)
    pane.hidden = false; pane.style.display = 'none'
    expect(focusActiveInput()).toBe(false)
    pane.style.display = ''; owner.setAttribute('aria-modal', 'true')
    expect(focusActiveInput()).toBe(false)
    owner.removeAttribute('aria-modal'); document.body.appendChild(pane.cloneNode(true))
    expect(focusActiveInput()).toBe(false)
    expect(document.activeElement).toBe(owner)
  })
  it('addresses a non-bubbling request only to the active terminal and requires a live acknowledgment', () => {
    document.body.innerHTML = '<div class="uam-chat-pane" data-focused="false"><div data-uam-terminal></div></div><div class="uam-chat-pane" data-focused="true"><div data-uam-terminal></div></div>'
    const nodes = document.querySelectorAll('[data-uam-terminal]')
    const inactive = vi.fn()
    const parent = vi.fn()
    nodes[0].addEventListener(FOCUS_ACTIVE_INPUT_EVENT, inactive)
    document.body.addEventListener(FOCUS_ACTIVE_INPUT_EVENT, parent)
    expect(focusActiveInput()).toBe(false)
    const active = vi.fn((event: Event) => event.preventDefault())
    nodes[1].addEventListener(FOCUS_ACTIVE_INPUT_EVENT, active)
    expect(focusActiveInput()).toBe(true)
    expect(active).toHaveBeenCalledOnce()
    expect(inactive).not.toHaveBeenCalled()
    expect(parent).not.toHaveBeenCalled()
    document.body.removeEventListener(FOCUS_ACTIVE_INPUT_EVENT, parent)
  })
})
