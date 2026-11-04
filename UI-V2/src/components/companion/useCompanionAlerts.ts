import { useEffect, useRef, useState } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { displayedChatStatus } from '../sidebar/chatSearch'

const ALERTS_KEY = 'uam-companion-input-alerts'
const BROWSER_KEY = 'uam-companion-browser-alerts'

/** Watches input requests without changing desktop notification preferences. */
export function useCompanionAlerts(connected: boolean) {
  const [enabled, setEnabledState] = useState(() => window.localStorage.getItem(ALERTS_KEY) !== 'false')
  const [browserEnabled, setBrowserEnabled] = useState(() => window.localStorage.getItem(BROWSER_KEY) === 'true')
  const [notificationError, setNotificationError] = useState('')
  const sessions = useAppStore((state) => state.sessions)
  const bindings = useAppStore((state) => state.acpBindingBySessionId)
  const cliBindings = useAppStore((state) => state.cliBindingBySessionId)
  const seen = useRef(new Map<string, string>())
  const canNotify = useRef(false)
  canNotify.current = connected && enabled && browserEnabled
  const attention = connected ? sessions.filter((chat) => displayedChatStatus(
    cliBindings[chat.id] ? [cliBindings[chat.id]] : [], bindings[chat.id] ? [bindings[chat.id]] : [],
  )?.type === 'attention') : []
  const setEnabled = (value: boolean) => { window.localStorage.setItem(ALERTS_KEY, String(value)); setEnabledState(value) }
  const disableBrowser = () => { window.localStorage.setItem(BROWSER_KEY, 'false'); setBrowserEnabled(false) }
  const enableBrowser = async () => {
    setNotificationError('')
    try {
      if (!('Notification' in window) || !('serviceWorker' in navigator) || !window.isSecureContext) {
        throw new Error('Browser notifications are unavailable. Input alerts still appear in this app.')
      }
      const permission = await Notification.requestPermission()
      if (permission !== 'granted') throw new Error('Notifications are blocked. Allow them in your browser settings.')
      await navigator.serviceWorker.register('/companion-sw.js', { scope: '/companion' })
      await navigator.serviceWorker.ready
      if (Notification.permission === 'denied') throw new Error('Notifications are blocked. Allow them in your browser settings.')
      window.localStorage.setItem(BROWSER_KEY, 'true')
      seen.current.clear()
      setBrowserEnabled(true)
    } catch (error) { setNotificationError(error instanceof Error ? error.message : 'Could not enable notifications.') }
  }
  useEffect(() => {
    if (!connected) { seen.current.clear(); return }
    const next = new Map<string, string>()
    for (const chat of attention) {
      const binding = bindings[chat.id]
      const key = `${binding?.attentionKind}:${binding?.pendingPermission?.requestId ?? binding?.pendingUserInput?.requestId ?? ''}`
      next.set(chat.id, key)
      if (!enabled || !browserEnabled || seen.current.get(chat.id) === key) continue
      void (async () => {
        try {
          const registration = await navigator.serviceWorker.getRegistration('/companion')
          if (!canNotify.current) return
          if (!registration) throw new Error('Browser notifications are unavailable. Input alerts still appear in this app.')
          await registration.showNotification('UAM needs input', { body: chat.name, tag: `uam-input-${chat.id}`, data: { chatId: chat.id } })
        } catch { if (canNotify.current) setNotificationError('Could not show a browser notification. Input alerts still appear in this app.') }
      })()
    }
    seen.current = next
  }, [connected, enabled, browserEnabled, sessions, bindings, cliBindings])
  return { enabled, setEnabled, attention, browserEnabled, enableBrowser, disableBrowser, notificationError }
}
