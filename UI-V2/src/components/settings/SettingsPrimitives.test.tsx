import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, expect, it } from 'vitest'
import { Switch } from '../ui'
import { SaveButton, SettingRow } from './SettingsPrimitives'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const host = document.createElement('div')
const root = createRoot(host)
document.body.appendChild(host)
afterEach(() => act(() => root.render(null)))

const describedText = (element: Element) => (element.getAttribute('aria-describedby') ?? '').split(' ').map((id) => document.getElementById(id)?.textContent).join(' | ')

it('associates row descriptions and status with each control or control group', () => {
  act(() => root.render(<>
    <SettingRow title="Git" description="Uses the local repository." status="Running" control={<Switch hideLabel label="Enable Git" />} />
    <SettingRow title="Policy" description="Applies to new chats." control={<div role="radiogroup" aria-label="Policy"><button role="radio">On</button><button role="radio">Off</button></div>} />
    <SettingRow title="Base" description="Empty uses HEAD." control={<><input aria-label="Base" aria-describedby="own-hint" /><span id="own-hint">Own hint</span></>} />
  </>))
  expect(describedText(host.querySelector('[aria-label="Enable Git"]')!)).toBe('Uses the local repository. | Running')
  expect(describedText(host.querySelector('[role="radiogroup"]')!)).toBe('Applies to new chats.')
  expect(host.querySelector('[role="radio"]')!.hasAttribute('aria-describedby')).toBe(false)
  expect(describedText(host.querySelector('input[aria-label="Base"]')!)).toBe('Own hint | Empty uses HEAD.')

  act(() => root.render(<SettingRow title="Git" description="Uses the local repository." control={<Switch hideLabel label="Enable Git" />} />))
  const git = host.querySelector('[aria-label="Enable Git"]')!
  expect(describedText(git)).toBe('Uses the local repository.')
})

it('re-links the row description after a tooltip rewrites the control description', async () => {
  act(() => root.render(<><span id="tip">Tooltip</span><SettingRow title="Agent" description="Built in · read only" control={<button aria-label="Edit agent" />} /></>))
  const edit = host.querySelector('[aria-label="Edit agent"]')!
  edit.setAttribute('aria-describedby', 'tip')
  await act(async () => { await Promise.resolve() })
  expect(describedText(edit)).toBe('Tooltip | Built in · read only')
  edit.removeAttribute('aria-describedby')
  await act(async () => { await Promise.resolve() })
  expect(describedText(edit)).toBe('Built in · read only')
})

it('marks a pending save busy and announces it in a polite live region', () => {
  act(() => root.render(<SaveButton saving={false}>Save VCS settings</SaveButton>))
  const status = host.querySelector('[role="status"]')!
  expect(status.getAttribute('aria-live')).toBe('polite')
  expect(status.textContent).toBe('')
  expect(host.querySelector('button')!.hasAttribute('aria-busy')).toBe(false)

  act(() => root.render(<SaveButton saving disabled>Save VCS settings</SaveButton>))
  expect(host.querySelector('[role="status"]')).toBe(status)
  expect(status.textContent).toBe('Saving…')
  const button = host.querySelector('button')!
  expect(button.getAttribute('aria-busy')).toBe('true')
  expect(button.textContent).toBe('Saving…')

  act(() => root.render(<SaveButton saving loading>Save routing</SaveButton>))
  expect(host.querySelector('button')!.textContent).toBe('Save routing')
  expect(host.querySelector('[role="status"]')!.textContent).toBe('Saving…')
})
