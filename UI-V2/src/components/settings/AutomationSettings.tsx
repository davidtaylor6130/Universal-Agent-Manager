import { useEffect, useState } from 'react'
import { Plus, X } from 'lucide-react'
import { useShallow } from 'zustand/react/shallow'
import { sendToCEF } from '../../ipc/cefBridge'
import { useAppStore } from '../../store/useAppStore'
import { reportAppFailure } from '../../utils/appFailure'
import { copyTextToClipboard } from '../../utils/copySelection'
import { Button, ConfirmDialog, IconButton, MenuSelect, Notice, Overlay, Presence, Switch } from '../ui'
import { SettingRow, SettingsField, SettingsLoading, SettingsSection } from './SettingsPrimitives'

export interface AutomationToken {
  id: string
  name: string
  trusted: boolean
  workspaces: string[]
  maxGitPermission: string
  createdAt: string
}

const GIT_OPTIONS = [
  { value: 'commit', label: 'Commit to its branch' },
  { value: 'pr', label: 'Open a pull request' },
]

const describeToken = (token: AutomationToken) =>
  `${token.trusted ? 'Starts straight away' : 'Waits for approval'} · ${token.maxGitPermission === 'pr' ? 'Pull requests allowed' : 'Commit only'} · ${token.workspaces.length} ${token.workspaces.length === 1 ? 'workspace' : 'workspaces'}`

/** Tokens are listed as metadata only; the secret exists solely in the create response shown once here. */
export function AutomationSettings() {
  const folders = useAppStore(useShallow(state => state.folders))
  const [tokens, setTokens] = useState<AutomationToken[] | null>(null)
  const [endpoint, setEndpoint] = useState('')
  const [editing, setEditing] = useState(false)
  const [name, setName] = useState('')
  const [trusted, setTrusted] = useState(false)
  const [git, setGit] = useState('commit')
  const [workspaces, setWorkspaces] = useState<string[]>([])
  const [secret, setSecret] = useState('')
  const [error, setError] = useState('')
  const [busy, setBusy] = useState(false)
  const [copied, setCopied] = useState(false)
  const [revoking, setRevoking] = useState<AutomationToken | null>(null)

  useEffect(() => {
    let current = true
    void sendToCEF<{ tokens: AutomationToken[] }>({ action: 'getAutomationTokens' }).then(response => {
      if (!current) return
      if (response.ok && response.data) setTokens(response.data.tokens)
      else { setTokens([]); reportAppFailure(response.error || 'Could not load automation tokens.', 'Try again.') }
    })
    void sendToCEF<{ effectiveUrl?: string; state?: string }>({ action: 'getCompanionSettings' }).then(response => {
      if (!current || !response.ok || !response.data?.effectiveUrl) return
      setEndpoint(`${new URL(response.data.effectiveUrl).origin}/automation/chats`)
    })
    return () => { current = false }
  }, [])

  const close = () => {
    if (busy) return
    setEditing(false); setSecret(''); setError(''); setCopied(false)
  }
  const open = () => { setName(''); setTrusted(false); setGit('commit'); setWorkspaces([]); setError(''); setSecret(''); setCopied(false); setEditing(true) }
  const create = async () => {
    setBusy(true); setError('')
    const response = await sendToCEF<{ secret: string; token: AutomationToken }>({ action: 'createAutomationToken', payload: { name, trusted, maxGitPermission: git, workspaces } })
    setBusy(false)
    if (!response.ok || !response.data) { setError(response.error || 'Could not create the token.'); return }
    setTokens(current => [...(current ?? []), response.data!.token])
    setSecret(response.data.secret)
  }
  const revoke = async (token: AutomationToken) => {
    setRevoking(null)
    const response = await sendToCEF({ action: 'revokeAutomationToken', payload: { id: token.id } })
    if (!response.ok) { reportAppFailure(response.error || 'Could not revoke the token.', 'Try again.'); return }
    setTokens(current => current?.filter(value => value.id !== token.id) ?? [])
  }
  const copy = async () => setCopied(await copyTextToClipboard(secret, document))

  return <div className="grid gap-0">
    <SettingsSection title="API access" actions={<Button size="sm" variant="secondary" leadingIcon={<Plus size={14} aria-hidden />} onClick={open}>New token</Button>}>
      <SettingRow title="Endpoint" description={endpoint || 'Turn on Phone access to reach this API.'} />
      {tokens === null && <SettingsLoading label="Loading automation tokens" rows={2} />}
      {tokens !== null && tokens.length === 0 && <SettingRow title={<span style={{ color: 'var(--text-3)' }}>No automation tokens.</span>} />}
      {tokens?.map(token => <SettingRow key={token.id} title={token.name} description={describeToken(token)}
        control={<IconButton size="sm" icon={<X size={14} />} label={`Revoke ${token.name}`} onClick={() => setRevoking(token)} />} />)}
    </SettingsSection>

    <Presence open={editing}>
      <Overlay open zIndex={60} onBackdropMouseDown={secret ? undefined : close}>
        <div role="dialog" aria-modal="true" data-settings-owned-overlay aria-label="New automation token" tabIndex={-1} className="uam-mcp-dialog uam-central-dialog"
          onKeyDown={event => { if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); close() } }}>
          <header className="uam-central-dialog__head">
            <span className="text-sm font-semibold">{secret ? 'Copy your token' : 'New automation token'}</span>
            <IconButton size="sm" icon={<X size={14} />} label="Close token dialog" disabled={busy} onClick={close} />
          </header>
          {secret
            ? <div className="uam-central-dialog__body">
              <Notice tone="warning" dismissible={false} dismissLabel="Dismiss">This token is shown once. UAM keeps only its hash.</Notice>
              <SettingsField label="Token">
                <input readOnly autoFocus aria-label="Automation token" className="uam-field" value={secret} onFocus={event => event.currentTarget.select()} style={{ fontFamily: 'var(--font-mono)' }} />
              </SettingsField>
              <div className="flex justify-end gap-2">
                <Button size="sm" variant="secondary" onClick={() => void copy()}>{copied ? 'Copied' : 'Copy token'}</Button>
                <Button size="sm" onClick={close}>Done</Button>
              </div>
            </div>
            : <div className="uam-central-dialog__body">
              <SettingsField label="Name">
                <input autoFocus aria-label="Token name" className="uam-field" value={name} placeholder="n8n homelab" maxLength={80} onChange={event => setName(event.currentTarget.value)} />
              </SettingsField>
              <SettingRow title="Start chats straight away" description="Off: chats wait in Active until you approve them."
                control={<Switch hideLabel label="Start chats straight away" checked={trusted} onChange={event => setTrusted(event.currentTarget.checked)} />} />
              <SettingsField label="Most Git access a request can ask for">
                <MenuSelect label="Most Git access a request can ask for" value={git} options={GIT_OPTIONS} onChange={setGit} />
              </SettingsField>
              <SettingsField label="Allowed workspaces">
                <div className="grid gap-1">
                  {folders.map(folder => <SettingRow key={folder.id} title={folder.name} description={folder.directory}
                    control={<Switch hideLabel label={`Allow ${folder.name}`} checked={workspaces.includes(folder.id)}
                      onChange={event => { const checked = event.currentTarget.checked; setWorkspaces(current => checked ? [...current, folder.id] : current.filter(value => value !== folder.id)) }} />} />)}
                </div>
              </SettingsField>
              {error && <Notice tone="error" dismissible={false} dismissLabel="Dismiss">{error}</Notice>}
              <div className="flex justify-end gap-2">
                <Button size="sm" variant="ghost" disabled={busy} onClick={close}>Cancel</Button>
                <Button size="sm" disabled={busy || !name.trim() || workspaces.length === 0} onClick={() => void create()}>{busy ? 'Creating…' : 'Create token'}</Button>
              </div>
            </div>}
        </div>
      </Overlay>
    </Presence>

    <ConfirmDialog open={revoking !== null} tone="danger" title="Revoke this token?" label="Revoke automation token" confirmLabel="Revoke token"
      onCancel={() => setRevoking(null)} onConfirm={() => { if (revoking) void revoke(revoking) }}>
      Requests using {revoking?.name} are refused immediately. Chats it already created are kept.
    </ConfirmDialog>
  </div>
}
