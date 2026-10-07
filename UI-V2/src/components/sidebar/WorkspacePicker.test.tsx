import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { WorkspacePicker, type WorkspacePickerProps } from './WorkspacePicker'
import type { Folder } from '../../types/session'
import type { ResourceCollection } from '../../types/resourceCollection'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const folder = (id: string, name: string, directory: string, host = 'local'): Folder => ({ id, name, directory, executionHostId: host, parentId: null, isExpanded: false, createdAt: new Date(0) })
const collection = (id: string, name: string, ...ids: string[]): ResourceCollection => ({ id, name, collapsed: true, references: ids.map((target, index) => ({ id: `${id}-${index}`, type: 'workspace-folder', target, label: 'Ignored reference label' })) })
let host: HTMLDivElement
let root: Root
let props: WorkspacePickerProps
const trigger = () => host.querySelector<HTMLButtonElement>('[role="combobox"]')!
const search = () => document.querySelector<HTMLInputElement>('input[aria-label="Search workspaces"]')!
const options = () => Array.from(document.querySelectorAll<HTMLButtonElement>('[role="option"]'))
const render = () => act(() => root.render(<WorkspacePicker {...props} />))
const open = () => act(() => trigger().click())
const key = (value: string, target: HTMLElement = search(), shiftKey = false) => act(() => target.dispatchEvent(new KeyboardEvent('keydown', { key: value, shiftKey, bubbles: true, cancelable: true })))
const query = (value: string) => act(() => {
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(search(), value)
  search().dispatchEvent(new Event('input', { bubbles: true }))
})
const active = () => document.getElementById(search().getAttribute('aria-activedescendant') ?? '')
beforeEach(() => {
  host = document.createElement('div')
  document.body.appendChild(host)
  root = createRoot(host)
  props = { folders: [folder('b', 'Backend', '/repo/backend'), folder('a', 'App', '/repo/app'), folder('u', 'Loose', '/misc')], collections: [collection('team', 'Team projects', 'a', 'b'), collection('other', 'Other', 'a', 'missing')], executionHostId: 'local', value: 'b', onChange: vi.fn() }
  render()
})
afterEach(() => { act(() => root.unmount()); host.remove() })

describe('WorkspacePicker', () => {
  it('mirrors first sidebar collection ownership and searches names, paths and group text', () => {
    open()
    expect(options().map((entry) => entry.textContent)).toEqual(['App/repo/app', 'Backend/repo/backend', 'Loose/misc'])
    expect(document.body.textContent).not.toContain('Other')
    query('team backend')
    expect(options().map((entry) => entry.textContent)).toEqual(['Backend/repo/backend'])
    query('/repo/app')
    expect(options()).toHaveLength(1)
    expect(options()[0].textContent).toContain('App')
    query('no such project')
    expect(options()).toHaveLength(0)
    expect(document.querySelector('[role="status"]')?.textContent).toBe('No matching workspaces')
    key('Enter')
    expect(props.onChange).not.toHaveBeenCalled()
  })
  it('navigates from the selected item, wraps, and selects using keyboard without closing a parent modal', () => {
    const parentEscape = vi.fn()
    window.addEventListener('keydown', parentEscape)
    try {
      key('ArrowDown', trigger())
      expect(document.activeElement).toBe(search())
      expect(active()?.textContent).toContain('Backend')
      key('ArrowDown')
      expect(active()?.textContent).toContain('Loose')
      key('ArrowDown')
      expect(active()?.textContent).toContain('App')
      key('ArrowUp')
      expect(active()?.textContent).toContain('Loose')
      key('Home')
      expect(active()?.textContent).toContain('App')
      key('End')
      key('Enter')
      expect(props.onChange).toHaveBeenCalledWith('u')
      expect(document.activeElement).toBe(trigger())
      expect(trigger().getAttribute('aria-expanded')).toBe('false')
      open()
      key('Escape')
      expect(parentEscape).not.toHaveBeenCalled()
      expect(document.activeElement).toBe(trigger())
      expect(props.onChange).toHaveBeenCalledTimes(1)
    } finally { window.removeEventListener('keydown', parentEscape) }
  })
  it('filters host ownership before collection grouping and keeps same-path projects distinct across hosts', () => {
    props = { ...props, folders: [folder('local', 'Same name', '/repo'), folder('remote-a', 'Same name', '/repo', 'ssh-a'), folder('remote-b', 'Same name', '/repo', 'ssh-b')], collections: [collection('shared', 'Projects', 'local', 'remote-a', 'remote-b')], executionHostId: 'ssh-a', value: 'remote-a' }
    render()
    open()
    expect(options()).toHaveLength(1)
    expect(options()[0].getAttribute('aria-selected')).toBe('true')
    props = { ...props, executionHostId: 'ssh-b' }
    render()
    expect(trigger().textContent).toBe('Choose workspace')
    expect(options()).toHaveLength(1)
    expect(options()[0].getAttribute('aria-selected')).toBe('false')
    key('Enter')
    expect(props.onChange).toHaveBeenCalledWith('remote-b')
  })
  it('retains the remote empty choice and handles an empty host without inventing a project', () => {
    props = { ...props, executionHostId: 'ssh-new', value: null, remoteEmptyOption: { label: 'New remote workspace…', description: 'Create on Remote' } }
    render()
    open()
    expect(options()).toHaveLength(1)
    key('Enter')
    expect(props.onChange).toHaveBeenCalledWith(null)
    props = { ...props, remoteEmptyOption: undefined }
    render()
    open()
    expect(options()).toHaveLength(0)
    expect(document.querySelector('[role="status"]')?.textContent).toBe('No workspaces on this host')
    key('ArrowDown')
    key('Enter')
    expect(props.onChange).toHaveBeenCalledTimes(1)
    key('Escape')
  })
  it('does not silently select a replacement when the selected or active folder is removed', () => {
    open()
    props = { ...props, folders: [folder('u', 'Loose', '/misc')] }
    render()
    expect(trigger().textContent).toBe('Choose workspace')
    expect(document.activeElement).toBe(search())
    expect(active()?.textContent).toContain('Loose')
    expect(props.onChange).not.toHaveBeenCalled()
    key('Enter')
    expect(props.onChange).toHaveBeenCalledWith('u')
  })
  it('returns focus on Tab, selects by pointer and closes outside without stealing focus', () => {
    open()
    key('Tab', search(), true)
    expect(document.activeElement).toBe(trigger())
    expect(trigger().getAttribute('aria-expanded')).toBe('false')
    open()
    act(() => options()[0].click())
    expect(props.onChange).toHaveBeenCalledWith('a')
    expect(document.activeElement).toBe(trigger())
    open()
    const outside = document.createElement('input')
    document.body.appendChild(outside)
    try {
      act(() => { outside.focus(); outside.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })) })
      expect(trigger().getAttribute('aria-expanded')).toBe('false')
      expect(document.activeElement).toBe(outside)
      props = { ...props, disabled: true }
      render()
      act(() => trigger().click())
      expect(trigger().getAttribute('aria-expanded')).toBe('false')
    } finally { outside.remove() }
  })
  it('reveals the selected parent on open and excludes collapsed children from keyboard navigation', () => {
    open()
    const disclosure = document.querySelector<HTMLButtonElement>('[data-workspace-group="team"]')!
    expect(disclosure.getAttribute('aria-expanded')).toBe('true')
    expect(props.collections[0].collapsed).toBe(true)
    act(() => disclosure.click())
    expect(disclosure.getAttribute('aria-expanded')).toBe('false')
    expect(options().map((entry) => entry.textContent)).toEqual(['Loose/misc'])
    key('Home')
    expect(active()?.textContent).toContain('Loose')
    key('ArrowDown')
    expect(active()?.textContent).toContain('Loose')
    query('backend')
    expect(document.querySelector('[data-workspace-group="team"]')?.getAttribute('aria-expanded')).toBe('true')
    expect(options().map((entry) => entry.textContent)).toEqual(['Backend/repo/backend'])
    query('')
    expect(document.querySelector('[data-workspace-group="team"]')?.getAttribute('aria-expanded')).toBe('false')
    key('Escape')
    open()
    expect(document.querySelector('[data-workspace-group="team"]')?.getAttribute('aria-expanded')).toBe('true')
  })
  it('keeps equal collection labels independent and reveals searched parents without changing saved collapse', () => {
    props = { ...props, value: 'u', collections: [collection('first', 'Same group', 'a'), collection('second', 'Same group', 'b')] }
    render()
    open()
    const first = document.querySelector<HTMLButtonElement>('[data-workspace-group="first"]')!
    const second = document.querySelector<HTMLButtonElement>('[data-workspace-group="second"]')!
    expect(first.getAttribute('aria-expanded')).toBe('false')
    expect(second.getAttribute('aria-expanded')).toBe('false')
    act(() => first.click())
    expect(first.getAttribute('aria-expanded')).toBe('true')
    expect(second.getAttribute('aria-expanded')).toBe('false')
    query('same group')
    expect(options().map((entry) => entry.textContent)).toEqual(['App/repo/app', 'Backend/repo/backend'])
    key('End')
    expect(active()?.textContent).toContain('Backend')
    key('Enter')
    expect(props.onChange).toHaveBeenCalledWith('b')
    expect(props.collections.map((entry) => entry.collapsed)).toEqual([true, true])
  })
  it('closes an already open picker when disabled without submitting or retaining a focused stale option', () => {
    open()
    expect(document.activeElement).toBe(search())
    props = { ...props, disabled: true, folders: [] }
    render()
    expect(trigger().disabled).toBe(true)
    expect(trigger().getAttribute('aria-expanded')).toBe('false')
    expect(document.querySelector('input[aria-label="Search workspaces"]')).toBeNull()
    expect(document.activeElement?.isConnected).toBe(true)
    expect(props.onChange).not.toHaveBeenCalled()
  })

})
