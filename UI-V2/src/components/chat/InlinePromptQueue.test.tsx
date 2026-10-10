// @vitest-environment jsdom
import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { InlinePromptQueue } from './InlinePromptQueue'
import type { AcpQueuedPrompt } from '../../store/cpp/types'

const prompt = (id: string, text = id, attachments: AcpQueuedPrompt['attachments'] = []): AcpQueuedPrompt => ({ id, revision: 1, text, uamAgentId: 'agent', markdownStoreFiles: ['/skill.md'], attachments, goalMode: false, goalId: '' })
let host: HTMLDivElement
let root: Root
let props: Parameters<typeof InlinePromptQueue>[0]
const button = (label: string, index = 0) => host.querySelectorAll<HTMLButtonElement>(`button[aria-label="${label}"]`)[index]
const click = async (target: HTMLButtonElement) => { await act(async () => target.click()) }
const editText = (value: string) => act(() => {
  const textarea = host.querySelector('textarea')!
  Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(textarea, value)
  textarea.dispatchEvent(new Event('input', { bubbles: true }))
})
beforeEach(() => {
  globalThis.IS_REACT_ACT_ENVIRONMENT = true
  host = document.createElement('div')
  document.body.appendChild(host)
  root = createRoot(host)
  props = { prompts: [prompt('a'), prompt('b')], onEdit: vi.fn(async () => true), onReorder: vi.fn(async () => true), onSteer: vi.fn(async () => true), onRemove: vi.fn(async () => true) }
  act(() => root.render(<InlinePromptQueue {...props} />))
})
afterEach(() => { act(() => root.unmount()); host.remove() })

describe('inline queued prompts', () => {
  it.each(['stable', 'legacy'] as const)('identifies files in an attachment-only %s row without changing the queue', (identity) => {
    const queued = { ...prompt('a', '', [{ id: 'image', name: 'photo.png', path: '/project/photo.png', type: 'image', size: 1 }]), markdownStoreFiles: ['C:\\skills\\review.md'] }
    props.prompts = [identity === 'legacy' ? { ...queued, id: undefined, revision: undefined } : queued]
    act(() => root.render(<InlinePromptQueue {...props} />))
    const files = host.querySelector('[aria-label="Queued 1 files"]')!
    expect(files.textContent).toContain('photo.png')
    expect(files.textContent).toContain('review.md')
    expect(files.querySelector('[title="/project/photo.png"]')).not.toBeNull()
    expect(files.querySelector('button')).toBeNull()
    expect(props.onEdit).not.toHaveBeenCalled()
    expect(props.onSteer).not.toHaveBeenCalled()
    expect(props.prompts[0].attachments).toEqual(queued.attachments)
  })
  it('edits only the queue snapshot and submits its stable ID and revision', async () => {
    await click(button('Edit queued prompt'))
    editText('Changed queued text')
    await click(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((entry) => entry.textContent === 'Save')!)
    expect(props.onEdit).toHaveBeenCalledWith('a', 1, 'Changed queued text', undefined)
    expect(host.querySelector('textarea')).toBeNull()
    expect(props.prompts[0]).toEqual(prompt('a'))
  })
  it('keeps the edit on save failure and prevents duplicate admission while pending', async () => {
    let finish!: (ok: boolean) => void
    props.onEdit = vi.fn(() => new Promise((resolve) => { finish = resolve }))
    act(() => root.render(<InlinePromptQueue {...props} />))
    await click(button('Edit queued prompt'))
    editText('Keep my edit')
    const save = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((entry) => entry.textContent === 'Save')!
    act(() => { save.click(); save.click() })
    expect(props.onEdit).toHaveBeenCalledTimes(1)
    expect(save.disabled).toBe(true)
    expect(button('Remove queued prompt').disabled).toBe(true)
    await act(async () => finish(false))
    expect((host.querySelector('textarea') as HTMLTextAreaElement).value).toBe('Keep my edit')
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('could not be saved')
  })
  it('removes only the selected queued attachment and saves the remaining IDs', async () => {
    const attachments = [
      { id: 'keep', name: 'keep.png', type: 'image', size: 1 },
      { id: 'remove', name: 'remove.txt', type: 'file', size: 1 },
    ]
    props.prompts = [prompt('a', 'queued text', attachments)]
    act(() => root.render(<InlinePromptQueue {...props} />))
    await click(button('Edit queued prompt'))
    expect(host.textContent).toContain('Editing queued 1')
    expect(host.textContent).toContain('keep.png')
    await click(button('Remove remove.txt attachment'))
    expect(host.textContent).toContain('keep.png')
    expect(host.textContent).not.toContain('remove.txt')
    await click(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((entry) => entry.textContent === 'Save')!)
    expect(props.onEdit).toHaveBeenCalledWith('a', 1, 'queued text', ['keep'])
  })
  it.each(['button', 'keyboard'] as const)('saves an attachment-only prompt from the %s', async (submit) => {
    props.prompts = [{ ...prompt('a', '', [
      { id: 'keep', name: 'keep.png', type: 'image', size: 1 },
      { id: 'remove', name: 'remove.txt', type: 'file', size: 1 },
    ]), markdownStoreFiles: [] }]
    act(() => root.render(<InlinePromptQueue {...props} />))
    await click(button('Edit queued prompt'))
    await click(button('Remove remove.txt attachment'))
    const save = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((entry) => entry.textContent === 'Save')!
    expect(save.disabled).toBe(false)
    if (submit === 'button') await click(save)
    else await act(async () => host.querySelector('textarea')!.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', metaKey: true, bubbles: true, cancelable: true })))
    expect(props.onEdit).toHaveBeenCalledWith('a', 1, '', ['keep'])
    expect(host.querySelector('textarea')).toBeNull()
  })
  it('disables blank save after every attachment is removed and preserves the draft on failure', async () => {
    props.prompts = [{ ...prompt('a', 'queued text', [{ id: 'only', name: 'only.png', type: 'image', size: 1 }]), markdownStoreFiles: [] }]
    props.onEdit = vi.fn(async () => false)
    act(() => root.render(<InlinePromptQueue {...props} />))
    await click(button('Edit queued prompt'))
    await click(button('Remove only.png attachment'))
    editText('')
    const save = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((entry) => entry.textContent === 'Save')!
    expect(save.disabled).toBe(true)
    editText('keep this text')
    await click(save)
    expect((host.querySelector('textarea') as HTMLTextAreaElement).value).toBe('keep this text')
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('could not be saved')
  })
  it.each(['stale', 'prepared'] as const)('blocks attachment removal from a %s queue row', async (change) => {
    const attachments = [{ id: 'keep', name: 'keep.png', type: 'image' as const, size: 1 }]
    props.prompts = [prompt('a', 'queued text', attachments)]
    act(() => root.render(<InlinePromptQueue {...props} />))
    await click(button('Edit queued prompt'))
    const updated = prompt('a', 'queued text', attachments)
    props.prompts = [change === 'stale' ? { ...updated, revision: 2 } : { ...updated, prepared: true }]
    act(() => root.render(<InlinePromptQueue {...props} />))
    const remove = button('Remove keep.png attachment')
    expect(remove.disabled).toBe(true)
    await click(remove)
    expect(host.textContent).toContain('keep.png')
    expect(props.onEdit).not.toHaveBeenCalled()
  })
  it('blocks attachment removal while a save is pending', async () => {
    let finish!: (ok: boolean) => void
    props.prompts = [prompt('a', 'queued text', [
      { id: 'keep', name: 'keep.png', type: 'image', size: 1 },
      { id: 'remove', name: 'remove.txt', type: 'file', size: 1 },
    ])]
    props.onEdit = vi.fn(() => new Promise((resolve) => { finish = resolve }))
    act(() => root.render(<InlinePromptQueue {...props} />))
    await click(button('Edit queued prompt'))
    const save = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((entry) => entry.textContent === 'Save')!
    const remove = button('Remove remove.txt attachment')
    act(() => { save.click(); remove.click() })
    expect(remove.disabled).toBe(true)
    await click(remove)
    expect(host.textContent).toContain('remove.txt')
    expect(props.onEdit).toHaveBeenCalledTimes(1)
    await act(async () => finish(true))
  })
  it('keeps markdown-store files readonly and permits a blank markdown-only prompt save', async () => {
    props.prompts = [{ ...prompt('a', ''), markdownStoreFiles: ['/skills/workflow.md'] }]
    act(() => root.render(<InlinePromptQueue {...props} />))
    await click(button('Edit queued prompt'))
    expect(host.querySelector('[aria-label="Queued Skills context"]')?.textContent).toBe('workflow.md')
    const save = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((entry) => entry.textContent === 'Save')!
    expect(save.disabled).toBe(false)
    expect(host.querySelector('[aria-label="Remove workflow.md attachment"]')).toBeNull()
    await click(save)
    expect(props.onEdit).toHaveBeenCalledWith('a', 1, '', undefined)
  })
  it('reorders a complete expected queue snapshot and shows rejection', async () => {
    props.onReorder = vi.fn(async () => false)
    act(() => root.render(<InlinePromptQueue {...props} />))
    await click(button('Move queued prompt down'))
    expect(props.onReorder).toHaveBeenCalledWith([{ id: 'a', revision: 1 }, { id: 'b', revision: 1 }], ['b', 'a'])
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('could not be reordered')
    expect(button('Move queued prompt up').disabled).toBe(true)
  })
  it('retains text when a queued item changes or dispatches during editing', async () => {
    await click(button('Edit queued prompt'))
    editText('Unsent queue edit')
    props.prompts = [prompt('b')]
    act(() => root.render(<InlinePromptQueue {...props} />))
    expect((host.querySelector('textarea') as HTMLTextAreaElement).value).toBe('Unsent queue edit')
    const save = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((entry) => entry.textContent === 'Save')!
    expect(save.disabled).toBe(true)
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Queued prompt changed')
    await click(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((entry) => entry.textContent === 'Cancel')!)
    expect(host.querySelector('textarea')).toBeNull()
  })
  it('disables prepared rows and reorder without disabling an independent editable row', () => {
    props.prompts = [{ ...prompt('a'), prepared: true }, prompt('b')]
    act(() => root.render(<InlinePromptQueue {...props} />))
    expect(button('Edit queued prompt').disabled).toBe(true)
    expect(button('Remove queued prompt').disabled).toBe(true)
    expect(button('Steer with this prompt now').disabled).toBe(true)
    expect(button('Move queued prompt up', 1).disabled).toBe(true)
    expect(button('Edit queued prompt', 1).disabled).toBe(false)
  })
  it('keeps existing steer and remove actions at each visible queue index', async () => {
    await click(button('Steer with this prompt now', 1))
    await click(button('Remove queued prompt'))
    expect(props.onSteer).toHaveBeenCalledWith(1, { id: 'b', revision: 1 })
    expect(props.onRemove).toHaveBeenCalledWith(0, { id: 'a', revision: 1 })
  })
  it('preserves legacy index fallback when an older runtime provides no identity', async () => {
    props.prompts = [{ ...prompt('legacy'), id: undefined, revision: undefined }]
    act(() => root.render(<InlinePromptQueue {...props} />))
    await click(button('Remove queued prompt'))
    expect(props.onRemove).toHaveBeenCalledWith(0, undefined)
    expect(button('Edit queued prompt').disabled).toBe(true)
  })

  it('keeps held edits available, blocks steering, and admits Resume once without losing queued work on failure', async () => {
    let finish!: (ok: boolean) => void
    props.held = true
    props.onResume = vi.fn(() => new Promise((resolve) => { finish = resolve }))
    const submitComposer = vi.fn((event: React.FormEvent) => event.preventDefault())
    act(() => root.render(<form onSubmit={submitComposer}><InlinePromptQueue {...props} /></form>))
    const resume = () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((entry) => entry.textContent === 'Resume queue')!
    expect(button('Edit queued prompt').disabled).toBe(false)
    expect(button('Remove queued prompt').disabled).toBe(false)
    expect(button('Move queued prompt down').disabled).toBe(false)
    expect(button('Steer with this prompt now').disabled).toBe(true)
    await click(button('Edit queued prompt'))
    editText('Keep this queued edit')
    expect(resume().disabled).toBe(true)
    expect((host.querySelector('textarea') as HTMLTextAreaElement).value).toBe('Keep this queued edit')
    await click(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((entry) => entry.textContent === 'Cancel')!)
    act(() => { resume().click(); resume().click() })
    expect(props.onResume).toHaveBeenCalledTimes(1)
    expect(props.onResume).toHaveBeenCalledWith([{ id: 'a', revision: 1 }, { id: 'b', revision: 1 }])
    await act(async () => finish(false))
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('could not be resumed')
    expect(props.prompts).toEqual([prompt('a'), prompt('b')])
    expect(resume().disabled).toBe(false)
    expect(submitComposer).not.toHaveBeenCalled()
  })

})

it('handles queued edit shortcuts without escaping into chat cancellation', async () => {
  act(() => root.render(<InlinePromptQueue {...props} />))
  const escaped = vi.fn()
  document.addEventListener('keydown', escaped)
  await click(button('Edit queued prompt'))
  editText('Saved with shortcut')
  await act(async () => host.querySelector('textarea')!.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', ctrlKey: true, bubbles: true, cancelable: true })))
  expect(props.onEdit).toHaveBeenCalledWith('a', 1, 'Saved with shortcut', undefined)
  expect(escaped).not.toHaveBeenCalled()
  await click(button('Edit queued prompt'))
  await act(async () => host.querySelector('textarea')!.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })))
  expect(host.querySelector('textarea')).toBeNull()
  expect(escaped).not.toHaveBeenCalled()
  document.removeEventListener('keydown', escaped)
})
