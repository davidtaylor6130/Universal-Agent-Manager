import { useAppStore } from '../../store/useAppStore'
import { ThemeSelect } from '../settings/ThemeSelect'
import { Button, Switch } from '../ui'
import type { useCompanionAlerts } from './useCompanionAlerts'

/** Phone-local browser notification permissions. */
export function CompanionPreferences({ alerts }: { alerts: ReturnType<typeof useCompanionAlerts> }) {
  const theme = useAppStore((state) => state.theme)
  const setTheme = useAppStore((state) => state.setTheme)
  return <>
    <label>Theme</label><ThemeSelect value={theme} onChange={setTheme} />
    <Switch className="uam-companion-setting" label="Input alerts" checked={alerts.enabled} onChange={(event) => alerts.setEnabled(event.target.checked)} />
    <Button variant="secondary" onClick={() => alerts.browserEnabled ? alerts.disableBrowser() : void alerts.enableBrowser()}>{alerts.browserEnabled ? 'Disable browser notifications' : 'Enable browser notifications'}</Button>
    <p>Alerts update while this app is open.</p>
    {alerts.notificationError && <p role="alert">{alerts.notificationError}</p>}
  </>
}
