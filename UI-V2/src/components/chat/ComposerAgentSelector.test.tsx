import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, expect, it, vi } from 'vitest'
import { ComposerAgentSelector, uamAgentMenuOptions } from './Composer'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const agents = [
  { id: 'build', name: 'Build', description: 'Implement changes.' },
  { id: 'reviewer', name: 'Reviewer', description: 'Reviews diffs.' },
  { id: 'helper', name: 'Helper', description: 'Imported from Claude Code.', delegateOnly: true },
]

afterEach(() => { document.body.innerHTML = '' })

it('lists a delegate-only agent with its badge but does not let the chat select it', () => {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  const onSelect = vi.fn()
  act(() => root.render(<div><ComposerAgentSelector agents={agents} agentId="build" nextTurn={false} onSelectUamAgent={onSelect} /><textarea /></div>))

  act(() => (host.querySelector('button[role="combobox"]') as HTMLButtonElement).click())
  const options = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]'))
  const helper = options.find((option) => option.textContent?.includes('Helper'))!
  expect(helper.disabled).toBe(true)
  expect(helper.querySelector('.uam-badge')?.textContent).toBe('Delegate-only')
  expect(helper.textContent).toContain('Only other agents can delegate to it.')
  act(() => helper.click())
  expect(onSelect).not.toHaveBeenCalled()

  const reviewer = options.find((option) => option.textContent?.includes('Reviewer'))!
  expect(reviewer.disabled).toBe(false)
  expect(reviewer.querySelector('.uam-badge')).toBeNull()
  act(() => reviewer.click())
  expect(onSelect).toHaveBeenCalledWith('reviewer')
  act(() => root.unmount())
})

it('keeps chat agents selectable in the toolbar options', () => {
  expect(uamAgentMenuOptions(agents).map(({ value, disabled, badge }) => ({ value, disabled, badge }))).toEqual([
    { value: 'build', disabled: undefined, badge: undefined },
    { value: 'reviewer', disabled: undefined, badge: undefined },
    { value: 'helper', disabled: true, badge: 'Delegate-only' },
  ])
})
