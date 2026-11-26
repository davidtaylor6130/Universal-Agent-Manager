import { forwardRef, useEffect, useImperativeHandle, useState } from 'react'
import { Button, Presence, Reveal } from '../ui'
import { sendToCEF } from '../../ipc/cefBridge'
import { reportAppFailure, reportAppNotice } from '../../utils/appFailure'
import { SaveButton, SettingRow, SettingsField, SettingsLoading, SettingsSection, StatusMark } from './SettingsPrimitives'
import { UnsavedChangesDialog } from './UnsavedChangesDialog'

/** The saved Custom ACP agent as the desktop app reports it (#523). */
export interface CustomAcpAgentView {
  configured: boolean
  name: string
  displayName: string
  command: string
  arguments: string[]
  environment: { name: string; value: string }[]
  /** The exact command, quoted for reading only; UAM launches the argument array, never this text. */
  displayCommand: string
  fingerprint: string
  /** The program file the command finds now, or empty with `programError`. */
  program: string
  programError: string
  allowed: boolean
  /** Why the saved file could not be read at startup, until a save replaces it. */
  loadError: string
}

export interface CustomAcpAgentSettingsHandle { requestLeave: (next: () => void) => void }

interface Draft { displayName: string; command: string; argumentsText: string; environmentText: string }

let loadErrorReported = false
/** Test hook: lets each case see the startup load error again. */
export function resetCustomAcpLoadErrorReport() { loadErrorReported = false }

const field = 'uam-field w-full'
const monoArea = { minHeight: 96, resize: 'vertical' as const, fontFamily: 'var(--font-mono)', fontSize: 'var(--fs-sm)' }

function draftFrom(view: CustomAcpAgentView): Draft {
  return {
    displayName: view.displayName,
    command: view.command,
    argumentsText: view.arguments.join('\n'),
    environmentText: view.environment.map(({ name, value }) => `${name}=${value}`).join('\n'),
  }
}

/** One argument per line, kept exactly as typed; blank lines are ignored. */
export function parseArguments(text: string): string[] {
  return text.split(/\r?\n/).filter((line) => line.length > 0)
}

/** One NAME=value per line; the value is everything after the first `=`. Returns an error line instead when a line has no `=`. */
export function parseEnvironment(text: string): { name: string; value: string }[] | string {
  const variables: { name: string; value: string }[] = []
  for (const line of text.split(/\r?\n/)) {
    if (!line.trim()) continue
    const split = line.indexOf('=')
    if (split <= 0) return 'Write each environment variable on its own line as NAME=value.'
    variables.push({ name: line.slice(0, split).trim(), value: line.slice(split + 1) })
  }
  return variables
}

function isCustomAcpAgentView(value: unknown): value is CustomAcpAgentView {
  const view = value as CustomAcpAgentView
  return Boolean(view) && typeof view.displayCommand === 'string' && typeof view.fingerprint === 'string' && typeof view.allowed === 'boolean' &&
    typeof view.program === 'string' && Array.isArray(view.arguments) && Array.isArray(view.environment)
}

/**
 * Settings for the one Custom ACP agent: name, command, arguments and environment. The configuration lives only in
 * these settings, and a changed command must be allowed here, after reading it, before its first launch.
 */
// Pure, so a build without the Custom ACP agent drops this component entirely.
export const CustomAcpAgentSettings = /* @__PURE__ */ forwardRef<CustomAcpAgentSettingsHandle>(function CustomAcpAgentSettings(_props, ref) {
  const [saved, setSaved] = useState<CustomAcpAgentView | null>(null)
  const [draft, setDraft] = useState<Draft | null>(null)
  const [saving, setSaving] = useState(false)
  const [allowing, setAllowing] = useState(false)
  const [exit, setExit] = useState<(() => void) | null>(null)
  const dirty = Boolean(saved && draft && JSON.stringify(draft) !== JSON.stringify(draftFrom(saved)))
  const environment = draft ? parseEnvironment(draft.environmentText) : []
  const environmentError = typeof environment === 'string' ? environment : ''

  useEffect(() => {
    let active = true
    void sendToCEF<CustomAcpAgentView>({ action: 'getCustomAcpAgent' }).then((response) => {
      if (!active) return
      if (response.ok && isCustomAcpAgentView(response.data)) {
        setSaved(response.data)
        setDraft(draftFrom(response.data))
        // Reported once per app run, not on every visit to the page.
        if (response.data.loadError && !loadErrorReported) {
          loadErrorReported = true
          reportAppFailure(response.data.loadError, 'The form shows an empty agent until you save one.')
        }
      } else reportAppFailure(response.error || 'The custom ACP agent settings could not be loaded.', 'Reopen Settings to try again.')
    })
    return () => { active = false }
  }, [])

  useEffect(() => {
    if (!dirty) return
    const guard = (event: BeforeUnloadEvent) => { event.preventDefault(); event.returnValue = '' }
    window.addEventListener('beforeunload', guard)
    return () => window.removeEventListener('beforeunload', guard)
  }, [dirty])

  const accept = (view: unknown, message: string) => {
    if (!isCustomAcpAgentView(view)) {
      reportAppFailure('The custom ACP agent settings could not be read back.', 'Reopen Settings to check them.')
      return false
    }
    setSaved(view)
    setDraft(draftFrom(view))
    reportAppNotice(message, 'success')
    return true
  }

  const save = async (next?: () => void) => {
    if (!draft || saving || typeof environment === 'string') return
    setSaving(true)
    try {
      const response = await sendToCEF<CustomAcpAgentView>({ action: 'setCustomAcpAgent', payload: {
        displayName: draft.displayName, command: draft.command, arguments: parseArguments(draft.argumentsText), environment,
      } })
      if (!response.ok) reportAppFailure(response.error || 'The custom ACP agent could not be saved.', 'Your changes are kept.')
      else if (accept(response.data, 'Custom ACP agent saved.') && next) { setExit(null); next() }
    } finally { setSaving(false) }
  }

  useImperativeHandle(ref, () => ({ requestLeave: (next) => { if (saving) return; if (dirty) setExit(() => next); else next() } }))

  if (!saved || !draft) return <SettingsLoading label="Loading custom ACP agent" rows={4} />

  const allow = async () => {
    setAllowing(true)
    try {
      const response = await sendToCEF<CustomAcpAgentView>({ action: 'allowCustomAcpAgent', payload: { fingerprint: saved.fingerprint, program: saved.program } })
      if (response.ok) accept(response.data, 'Custom ACP agent command allowed.')
      else {
        reportAppFailure(response.error || 'The custom ACP agent command could not be allowed.', 'Review the command and try again.')
        // The command or the program it finds may have changed; show the current one so it can be reviewed.
        const current = await sendToCEF<CustomAcpAgentView>({ action: 'getCustomAcpAgent' })
        if (current.ok && isCustomAcpAgentView(current.data)) setSaved(current.data)
      }
    } finally { setAllowing(false) }
  }

  const set = (patch: Partial<Draft>) => setDraft({ ...draft, ...patch })
  const environmentNames = saved.environment.map(({ name }) => name)

  return (
    <div className="grid gap-0">
      <SettingsSection title="Custom ACP agent">
        <SettingRow title="Best effort" description="Works only with what the agent reports. No CLI View." />
        <SettingsField label="Name" htmlFor="custom-acp-name">
          <input id="custom-acp-name" className={field} value={draft.displayName} placeholder="Custom ACP agent" maxLength={80} disabled={saving}
            onChange={(event) => set({ displayName: event.currentTarget.value })} />
        </SettingsField>
        <SettingsField label="Command" htmlFor="custom-acp-command" hint="The program's name or full path. It is never run through a shell.">
          <input id="custom-acp-command" className={`${field} font-mono`} value={draft.command} spellCheck={false} autoComplete="off" disabled={saving}
            onChange={(event) => set({ command: event.currentTarget.value })} />
        </SettingsField>
        <SettingsField label="Arguments" htmlFor="custom-acp-arguments" hint="One per line, passed exactly as written.">
          <textarea id="custom-acp-arguments" className={field} rows={4} style={monoArea} value={draft.argumentsText} spellCheck={false} disabled={saving}
            onChange={(event) => set({ argumentsText: event.currentTarget.value })} />
        </SettingsField>
        <SettingsField label="Environment" htmlFor="custom-acp-environment" hint={environmentError || "One NAME=value per line, added to the agent's environment."}>
          <textarea id="custom-acp-environment" className={field} rows={3} style={monoArea} value={draft.environmentText} spellCheck={false} disabled={saving}
            aria-invalid={environmentError ? true : undefined} onChange={(event) => set({ environmentText: event.currentTarget.value })} />
        </SettingsField>
        <div className="uam-set-actions flex gap-2">
          <SaveButton saving={saving} disabled={!dirty || saving || Boolean(environmentError)} onClick={() => void save()}>Save</SaveButton>
          <Button size="sm" variant="ghost" disabled={!dirty || saving} onClick={() => setDraft(draftFrom(saved))}>Discard</Button>
        </div>
      </SettingsSection>
      <Reveal open={saved.configured}>
        <SettingsSection title="Command to run">
          <SettingRow
            title="Approval"
            description={saved.allowed ? 'A new command, argument, variable or program needs a new approval.' : 'Read the command below, then allow it before the first launch.'}
            control={saved.allowed
              ? <StatusMark tone="success" label="Allowed" />
              : <Button size="sm" variant="secondary" disabled={allowing || dirty || !saved.program} aria-busy={allowing || undefined} onClick={() => void allow()}>Allow this command</Button>}
            status={dirty ? 'Save your changes to review the new command.' : saved.programError || undefined}
          >
            <pre role="group" aria-label="Exact command" dir="ltr" className="uam-set-field__hint" style={{ whiteSpace: 'pre-wrap', wordBreak: 'break-all', fontFamily: 'var(--font-mono)', unicodeBidi: 'isolate' }}>{saved.displayCommand}</pre>
            {saved.program && <p className="uam-set-field__hint">Runs <code dir="ltr" style={{ unicodeBidi: 'isolate' }}>{saved.program}</code>.</p>}
            {environmentNames.length > 0 && <p className="uam-set-field__hint">Also sets {environmentNames.join(', ')}.</p>}
          </SettingRow>
        </SettingsSection>
      </Reveal>
      <Presence open={Boolean(exit)}>{exit && <UnsavedChangesDialog title="Unsaved custom ACP agent" busy={saving} saveDisabled={Boolean(environmentError)}
        onStay={() => setExit(null)} onDiscard={() => { const next = exit; setExit(null); setDraft(draftFrom(saved)); next() }} onSave={() => void save(exit)} />}</Presence>
    </div>
  )
})
