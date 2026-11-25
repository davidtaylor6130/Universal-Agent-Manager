import { useState } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { useAppStore } from '../../store/useAppStore'
import { displayedChatStatus } from '../sidebar/chatSearch'
import { Button, ConfirmDialog } from '../ui'

type RunningChatState = Pick<ReturnType<typeof useAppStore.getState>, 'sessions' | 'acpBindingBySessionId' | 'cliBindingBySessionId'>

const NO_CHATS: string[] = []
const LISTED_CHATS = 5

/**
 * Names the chats a restart would interrupt: a provider turn that is working or waiting for the user,
 * or a live native CLI session, whose process ends with the app.
 */
export function runningChatNames({ sessions, acpBindingBySessionId, cliBindingBySessionId }: RunningChatState): string[] {
  return sessions.flatMap((session) => {
    const cli = cliBindingBySessionId[session.id]
    const status = displayedChatStatus([cli], [acpBindingBySessionId[session.id]])
    return cli?.running || (status && status.type !== 'done') ? [session.name.trim() || 'Untitled chat'] : []
  })
}

interface PhoneRestartButtonProps {
  disabled: boolean
  restarting: boolean
  onRestart: () => void
}

/** Restart Universal Agent Manager beside a phone access failure; asks first when the restart would stop running chats. */
export function PhoneRestartButton({ disabled, restarting, onRestart }: PhoneRestartButtonProps) {
  const [open, setOpen] = useState(false)
  const running = useAppStore(useShallow((state) => open ? runningChatNames(state) : NO_CHATS))
  const count = `${running.length} running ${running.length === 1 ? 'chat' : 'chats'}`
  return <>
    <Button size="sm" variant="secondary" disabled={disabled}
      onClick={() => { if (runningChatNames(useAppStore.getState()).length > 0) setOpen(true); else onRestart() }}>
      {restarting ? 'Restarting Universal Agent Manager…' : 'Restart Universal Agent Manager'}
    </Button>
    <ConfirmDialog open={open} settingsOwned title="Restart Universal Agent Manager?" confirmLabel="Restart"
      onCancel={() => setOpen(false)} onConfirm={() => { setOpen(false); onRestart() }}>
      {running.length === 0 ? <p>No chats are running now.</p> : <>
        <p>Restart will stop {count}. Their current turns and CLI sessions end when the app closes.</p>
        <ul aria-label="Running chats" className="mt-2 list-disc pl-4">
          {running.slice(0, LISTED_CHATS).map((name, index) => <li key={index}>{name}</li>)}
          {running.length > LISTED_CHATS && <li>and {running.length - LISTED_CHATS} more</li>}
        </ul>
      </>}
    </ConfirmDialog>
  </>
}
