import { useEffect, useRef, useState, type KeyboardEvent } from 'react'
import { Ban, CircleCheck, CircleHelp, Pencil, Plus, X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { permissionRuleError, type PermissionCommandRule, type PermissionRuleAction, type PermissionRuleTestResult, type PermissionRuleTool } from '../../types/permissionRules'
import { providerShortName } from '../../utils/providerMetadata'
import { ProviderLogo } from '../shared/ProviderLogo'
import { Button, ConfirmDialog, EXIT_MS, IconButton, MenuSelect, Overlay, Presence, Reveal, Switch, motionAllowed, scaledMs } from '../ui'
import { SaveButton, SettingRow, SettingsField, SettingsSection } from './SettingsPrimitives'
import './provider-settings.css'

const ACTION_OPTIONS: { value: PermissionRuleAction; label: string; description: string }[] = [
  { value: 'allow', label: 'Allow', description: 'Approve once without AI review, within AI Review’s limits.' },
  { value: 'ask', label: 'Ask', description: 'Always leave it for you, even under YOLO.' },
  { value: 'deny', label: 'Deny', description: 'Reject it once without asking.' },
]

const TOOL_LABELS: Record<PermissionRuleTool, string> = {
  command: 'Shell command',
  readOnly: 'Read-only commands and reads',
  edit: 'File edits in the workspace',
  fetch: 'Web access',
  other: 'Other tools',
  any: 'Any request',
}

const TOOL_OPTIONS: { value: PermissionRuleTool; label: string; description: string; allow: boolean }[] = [
  { value: 'command', label: TOOL_LABELS.command, description: 'Matched by pattern against the typed command.', allow: true },
  { value: 'readOnly', label: TOOL_LABELS.readOnly, description: 'Commands the read filter admits and in-workspace reads.', allow: true },
  { value: 'edit', label: TOOL_LABELS.edit, description: 'File changes the provider asks to make.', allow: true },
  { value: 'fetch', label: TOOL_LABELS.fetch, description: 'Web fetches and searches.', allow: false },
  { value: 'other', label: TOOL_LABELS.other, description: 'MCP and tools UAM cannot classify.', allow: false },
  { value: 'any', label: TOOL_LABELS.any, description: 'Every permission request.', allow: false },
]

const ACTION_ICONS: Record<PermissionRuleAction, typeof Ban> = { allow: CircleCheck, ask: CircleHelp, deny: Ban }

function newRule(): PermissionCommandRule {
  const token = globalThis.crypto?.randomUUID?.() ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`
  return { id: `rule-${token}`, action: 'deny', tool: 'command', pattern: '', providerId: '', workspace: '', enabled: true }
}

export function ruleTitle(rule: PermissionCommandRule) {
  return rule.tool === 'command' ? rule.pattern : TOOL_LABELS[rule.tool]
}

function testSummary(result: PermissionRuleTestResult, rules: PermissionCommandRule[], aiReviewEnabled: boolean) {
  const rule = rules.find((candidate) => candidate.id === result.ruleId)
  const name = rule ? `“${ruleTitle(rule)}”` : ''
  if (result.outcome === 'deny') return `Denied by ${name}.`
  if (result.outcome === 'ask') return rule ? `Always asks you: ${name}.` : result.reason
  if (result.outcome === 'allow') return `Allowed by ${name} under AI Review; no model review runs.`
  return aiReviewEnabled ? 'No rule matches. Under AI Review the reviewer decides.' : 'No rule matches. It waits for you.'
}

export function PermissionRulesSettings() {
  const rules = useAppStore((s) => s.permissionCommandRules)
  const aiReviewEnabled = useAppStore((s) => s.permissionAiReviewEnabled)
  const providers = useAppStore((s) => s.providers)
  const save = useAppStore((s) => s.setPermissionCommandRules)
  const testRules = useAppStore((s) => s.testPermissionCommandRules)
  const [editing, setEditing] = useState<{ draft: PermissionCommandRule; isNew: boolean; original: string } | null>(null)
  const [exitPrompt, setExitPrompt] = useState(false)
  const [pendingDelete, setPendingDelete] = useState<PermissionCommandRule | null>(null)
  const [busy, setBusy] = useState(false)
  const [saveError, setSaveError] = useState('')
  const [listError, setListError] = useState('')
  const [switchError, setSwitchError] = useState('')
  const [removingIds, setRemovingIds] = useState<Set<string>>(() => new Set())
  const [testCommand, setTestCommand] = useState('')
  const [testResult, setTestResult] = useState<PermissionRuleTestResult | { error: string } | null>(null)
  const dialogRef = useRef<HTMLDivElement>(null)
  const editorOpen = editing !== null

  useEffect(() => {
    if (!editorOpen) return
    const opener = document.activeElement as HTMLElement | null
    dialogRef.current?.querySelector<HTMLElement>('button, input')?.focus()
    return () => opener?.focus()
  }, [editorOpen])

  // The native matcher answers for the saved rules; a short pause keeps typing from flooding the bridge.
  useEffect(() => {
    const command = testCommand.trim()
    if (!command) { setTestResult(null); return }
    let cancelled = false
    const timer = window.setTimeout(() => {
      void testRules(rules, command).then((result) => { if (!cancelled) setTestResult(result) })
    }, 200)
    return () => { cancelled = true; window.clearTimeout(timer) }
  }, [testCommand, rules, testRules])

  const keepFocusInDialog = (event: KeyboardEvent<HTMLDivElement>) => {
    if (event.key !== 'Tab') return
    const focusable = Array.from(dialogRef.current?.querySelectorAll<HTMLElement>('button:not([disabled]), input:not([disabled])') ?? [])
    const edge = focusable[event.shiftKey ? 0 : focusable.length - 1]
    if (document.activeElement !== edge && !(event.shiftKey && document.activeElement === dialogRef.current)) return
    event.preventDefault()
    focusable[event.shiftKey ? focusable.length - 1 : 0]?.focus()
  }

  const providerName = (id: string) => providerShortName(providers.find((provider) => provider.id === id), id)
  const describe = (rule: PermissionCommandRule) => [
    ACTION_OPTIONS.find((option) => option.value === rule.action)?.label,
    rule.tool === 'command' ? TOOL_LABELS.command : undefined,
    rule.providerId ? providerName(rule.providerId) : undefined,
    rule.workspace || undefined,
  ].filter(Boolean).join(' · ')

  const startEditing = (draft: PermissionCommandRule, isNew: boolean) => { setExitPrompt(false); setSaveError(''); setEditing({ draft, isNew, original: JSON.stringify(draft) }) }
  const draft = editing?.draft
  const dirty = editing ? JSON.stringify(editing.draft) !== editing.original : false
  const draftError = draft ? permissionRuleError(draft) : ''
  const closeEditor = () => { if (busy) return; if (dirty) { setExitPrompt(true); dialogRef.current?.focus() } else setEditing(null) }
  const setDraft = (change: Partial<PermissionCommandRule>) => {
    setExitPrompt(false)
    setSaveError('')
    setEditing((current) => {
      if (!current) return current
      const next = { ...current.draft, ...change }
      if (next.tool !== 'command') next.pattern = ''
      // An allow rule cannot target web access, unclassified tools or every request.
      if (next.action === 'allow' && !TOOL_OPTIONS.find((option) => option.value === next.tool)?.allow) next.tool = 'command'
      return { ...current, draft: next }
    })
  }

  const persist = async (next: PermissionCommandRule[]) => {
    setBusy(true)
    try { return await save({ rules: next }) } finally { setBusy(false) }
  }
  const saveDraft = async () => {
    if (!editing || draftError) return
    const prepared = { ...editing.draft, pattern: editing.draft.pattern.trim(), workspace: editing.draft.workspace.trim() }
    const next = editing.isNew ? [...rules, prepared] : rules.map((rule) => rule.id === prepared.id ? prepared : rule)
    const result = await persist(next)
    if (result.ok) setEditing(null)
    else setSaveError(result.error ?? 'Could not save the rule.')
  }
  const toggleRule = async (rule: PermissionCommandRule, enabled: boolean) => {
    const result = await persist(rules.map((candidate) => candidate.id === rule.id ? { ...candidate, enabled } : candidate))
    setListError(result.ok ? '' : result.error ?? 'Could not save the rule.')
  }
  const deleteRule = async () => {
    if (!pendingDelete) return
    const { id } = pendingDelete
    setPendingDelete(null)
    // The row leaves through its exit before the store drops it; a failed save brings it back.
    setRemovingIds((current) => new Set(current).add(id))
    if (motionAllowed()) await new Promise((resolve) => window.setTimeout(resolve, scaledMs(EXIT_MS)))
    const result = await persist(rules.filter((rule) => rule.id !== id))
    setListError(result.ok ? '' : result.error ?? 'Could not delete the rule.')
    setRemovingIds((current) => { const next = new Set(current); next.delete(id); return next })
  }
  const toggleAiReview = async (enabled: boolean) => {
    setBusy(true)
    try {
      const result = await save({ aiReviewEnabled: enabled })
      setSwitchError(result.ok ? '' : result.error ?? 'Could not save AI review.')
    } finally { setBusy(false) }
  }

  const toolOptions = TOOL_OPTIONS.filter((option) => draft?.action !== 'allow' || option.allow)
  const testError = testResult && 'error' in testResult ? testResult.error : ''

  return <div className="grid gap-0">
    <ConfirmDialog open={pendingDelete !== null} title="Delete permission rule?" confirmLabel="Delete" busy={busy} onCancel={() => setPendingDelete(null)} onConfirm={() => void deleteRule()}>
      Requests this rule matched go back to the normal permission flow.
    </ConfirmDialog>

    <SettingsSection title="AI review">
      <SettingRow title="Ask the AI reviewer" description="Only for requests no rule decides, under AI Review."
        status={switchError ? <span role="alert" style={{ color: 'var(--error)' }}>{switchError}</span> : undefined}
        control={<Switch hideLabel label="Ask the AI reviewer" checked={aiReviewEnabled} disabled={busy} onChange={(event) => void toggleAiReview(event.target.checked)} />} />
    </SettingsSection>

    <SettingsSection title="Rules"
      actions={<Button size="sm" leadingIcon={<Plus size={14} />} disabled={busy} onClick={() => startEditing(newRule(), true)}>Add rule</Button>}>
      <Reveal open={rules.length === 0}><p className="uam-prov-sub">No rules. Every request follows the chat’s permission level.</p></Reveal>
      {rules.map((rule) => {
        const Icon = ACTION_ICONS[rule.action]
        const title = ruleTitle(rule)
        return <Reveal key={rule.id} open={!removingIds.has(rule.id)}><SettingRow icon={<Icon size={15} aria-hidden />}
          title={<span className={rule.tool === 'command' ? 'font-mono' : undefined}>{title}</span>} description={describe(rule)}
          control={<>
            <Switch hideLabel label={`Enable rule ${title}`} checked={rule.enabled} disabled={busy} onChange={(event) => void toggleRule(rule, event.target.checked)} />
            <IconButton size="sm" icon={<Pencil size={14} />} label={`Edit rule ${title}`} disabled={busy} onClick={() => startEditing(rule, false)} />
            <IconButton size="sm" icon={<X size={14} />} label={`Delete rule ${title}`} disabled={busy} onClick={() => setPendingDelete(rule)} />
          </>} /></Reveal>
      })}
      <Reveal open={Boolean(listError)}><p role="alert" className="text-xs" style={{ color: 'var(--error)' }}>{listError}</p></Reveal>
    </SettingsSection>

    <SettingsSection title="Test a command">
      <SettingsField label="Command" hint="Checked against the saved rules as the native matcher sees them.">
        <input className="uam-field w-full font-mono" aria-label="Command to test" placeholder="git push --force origin main" value={testCommand} spellCheck={false} onChange={(event) => setTestCommand(event.currentTarget.value)} />
      </SettingsField>
      <Reveal open={Boolean(testResult)}>
        <p className="uam-prov-sub" role="status" aria-live="polite" style={testError ? { color: 'var(--error)' } : undefined}>
          {testResult && ('error' in testResult ? testResult.error : testSummary(testResult, rules, aiReviewEnabled))}
        </p>
      </Reveal>
    </SettingsSection>

    <Presence open={editing !== null}>
      <Overlay open zIndex={60} onBackdropMouseDown={closeEditor}>
        {editing && draft && <div role="dialog" aria-modal="true" aria-label={editing.isNew ? 'New permission rule' : 'Edit permission rule'} tabIndex={-1} className="uam-mcp-dialog uam-central-dialog" ref={dialogRef}
          onKeyDown={(event) => { if ((event.target as HTMLElement).closest('[data-viewport-menu]')) return; keepFocusInDialog(event); if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); closeEditor() } }}>
          <header className="uam-central-dialog__head">
            <span className="text-sm font-semibold">{editing.isNew ? 'New permission rule' : 'Edit permission rule'}</span>
            <IconButton size="sm" icon={<X size={14} />} label="Close rule editor" disabled={busy} onClick={closeEditor} />
          </header>
          <div className="uam-central-dialog__body">
            <SettingsField label="Action">
              <MenuSelect label="Rule action" value={draft.action} disabled={busy} options={ACTION_OPTIONS.map((option) => ({ ...option, icon: (() => { const Icon = ACTION_ICONS[option.value]; return <Icon size={15} /> })() }))} onChange={(value) => setDraft({ action: value as PermissionRuleAction })} />
            </SettingsField>
            <SettingsField label="Applies to">
              <MenuSelect label="Rule applies to" value={draft.tool} disabled={busy} options={toolOptions.map(({ value, label, description }) => ({ value, label, description }))} onChange={(value) => setDraft({ tool: value as PermissionRuleTool })} />
            </SettingsField>
            <Reveal open={draft.tool === 'command'}>
              <SettingsField label="Command pattern" hint="Matches from the program name. * and ? match inside a word; ** spans words.">
                <input className="uam-field w-full font-mono" aria-label="Command pattern" value={draft.pattern} maxLength={512} spellCheck={false} placeholder="npm test" disabled={busy} onChange={(event) => setDraft({ pattern: event.currentTarget.value })} />
              </SettingsField>
            </Reveal>
            <SettingsField label="Provider">
              <MenuSelect label="Rule provider" value={draft.providerId} disabled={busy}
                options={[{ value: '', label: 'All providers' }, ...providers.map((provider) => ({ value: provider.id, label: providerName(provider.id), icon: <ProviderLogo providerId={provider.id} /> }))]}
                onChange={(value) => setDraft({ providerId: value })} />
            </SettingsField>
            <SettingsField label="Workspace" hint="Leave empty for every workspace; a folder also covers everything inside it.">
              <input className="uam-field w-full" aria-label="Rule workspace" value={draft.workspace} maxLength={1024} spellCheck={false} placeholder="All workspaces" disabled={busy} onChange={(event) => setDraft({ workspace: event.currentTarget.value })} />
            </SettingsField>
            <Reveal open={Boolean(draftError && dirty) || Boolean(saveError)}><p role="alert" className="text-xs" style={{ color: 'var(--error)' }}>{saveError || draftError}</p></Reveal>
          </div>
          <footer className="uam-central-dialog__foot">
            <Reveal open={exitPrompt} className="flex min-w-0 flex-1 items-center justify-end gap-2">
              <p className="mr-auto text-sm" role="status">Your changes have not been saved.</p>
              <Button size="sm" onClick={() => { setExitPrompt(false); dialogRef.current?.focus() }}>Keep editing</Button>
              <Button size="sm" variant="secondary" onClick={() => setEditing(null)}>Discard</Button>
            </Reveal>
            <Reveal open={!exitPrompt} className="flex items-center justify-end gap-2">
              <Button size="sm" disabled={busy} onClick={closeEditor}>Cancel</Button>
              <SaveButton saving={busy} loading={busy} disabled={busy || !dirty || Boolean(draftError)} onClick={() => void saveDraft()}>Save rule</SaveButton>
            </Reveal>
          </footer>
        </div>}
      </Overlay>
    </Presence>
  </div>
}
