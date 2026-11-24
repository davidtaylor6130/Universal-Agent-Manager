import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import type { Session } from '../../types/session'
import { PhoneRestartButton, runningChatNames } from './PhoneRestartButton'

const chat = (id: string, name = `Chat ${id}`) => ({ id, name, viewMode: 'chat', folderId: null, createdAt: new Date(), updatedAt: new Date() }) as Session

let root: Root | undefined
let host: HTMLDivElement | undefined
afterEach(() => {
  if (root) act(() => root!.unmount())
  host?.remove(); root = undefined; host = undefined
  useAppStore.setState({ sessions: [], acpBindingBySessionId: {}, cliBindingBySessionId: {} })
})

function render(onRestart = vi.fn()) {
  host = document.createElement('div'); document.body.append(host)
  root = createRoot(host)
  act(() => root!.render(<PhoneRestartButton disabled={false} restarting={false} onRestart={onRestart} />))
  act(() => host!.querySelector('button')!.click())
  return onRestart
}

const dialog = () => document.querySelector<HTMLElement>('[role="alertdialog"]')
const dialogButton = (label: string) => [...dialog()!.querySelectorAll('button')].find(button => button.textContent === label)!

describe('phone access application restart', () => {
  it('counts working, waiting and live CLI chats but not idle or finished ones', () => {
    expect(runningChatNames({
      sessions: [chat('turn'), chat('question'), chat('cli'), chat('busy-cli'), chat('done'), chat('idle'), chat('failed'), chat('blank', '  ')],
      acpBindingBySessionId: {
        turn: { processing: true },
        question: { attentionKind: 'question' },
        done: { readySinceLastSelect: true, running: true },
        idle: { running: true },
        failed: { attentionKind: 'error', running: false },
        blank: { lifecycleState: 'waitingPermission' },
      } as never,
      cliBindingBySessionId: { cli: { running: true }, 'busy-cli': { running: false, lifecycleState: 'busy' } } as never,
    })).toEqual(['Chat turn', 'Chat question', 'Chat cli', 'Chat busy-cli', 'Untitled chat'])
  })

  it('restarts straight away when no chat is running', () => {
    useAppStore.setState({ sessions: [chat('idle')], acpBindingBySessionId: { idle: { running: true, processing: false } as never } })
    const onRestart = render()
    expect(dialog()).toBeNull()
    expect(onRestart).toHaveBeenCalledOnce()
  })

  it('names the running chats and restarts only after confirmation', () => {
    useAppStore.setState({
      sessions: [chat('a', 'Refactor parser'), chat('b', 'Release notes'), chat('c', 'CLI session')],
      acpBindingBySessionId: { a: { processing: true } as never, b: { processing: true } as never },
      cliBindingBySessionId: { c: { running: true } as never },
    })
    const onRestart = render()
    expect(onRestart).not.toHaveBeenCalled()
    expect(dialog()!.textContent).toContain('Restart will stop 3 running chats.')
    expect([...dialog()!.querySelectorAll('li')].map(item => item.textContent)).toEqual(['Refactor parser', 'Release notes', 'CLI session'])
    act(() => dialogButton('Cancel').click())
    expect(dialog()).toBeNull()
    expect(onRestart).not.toHaveBeenCalled()

    act(() => host!.querySelector('button')!.click())
    act(() => dialogButton('Restart').click())
    expect(onRestart).toHaveBeenCalledOnce()
    expect(dialog()).toBeNull()
  })

  it('lists five chats and summarises the rest', () => {
    const sessions = Array.from({ length: 7 }, (_, index) => chat(String(index)))
    useAppStore.setState({ sessions, acpBindingBySessionId: Object.fromEntries(sessions.map(session => [session.id, { processing: true } as never])) })
    render()
    expect(dialog()!.textContent).toContain('Restart will stop 7 running chats.')
    expect([...dialog()!.querySelectorAll('li')].map(item => item.textContent)).toEqual(['Chat 0', 'Chat 1', 'Chat 2', 'Chat 3', 'Chat 4', 'and 2 more'])
  })
})
