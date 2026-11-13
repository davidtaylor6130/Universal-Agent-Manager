import { useEffect, useRef, useState, type KeyboardEvent } from 'react'
import { Cpu, Pencil, Plus, X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import type { ComputePool, ComputePoolMember } from '../../store/cpp/types'
import { buildModelOptions } from '../chat/modelOptions'
import { DEFAULT_PROVIDER_ID, providerShortName } from '../../utils/providerMetadata'
import { Button, ConfirmDialog, EXIT_MS, IconButton, MenuSelect, Overlay, Presence, Reveal, motionAllowed, scaledMs } from '../ui'
import { SaveButton, SettingRow, SettingsField, SettingsSection } from './SettingsPrimitives'

const MIN_LIMIT = 1
const MAX_LIMIT = 64
const ALL_MODELS = ''
const MAX_NAME_BYTES = 80

function newPool(providerId: string): ComputePool {
  const token = globalThis.crypto?.randomUUID?.() ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`
  return { id: `pool-${token}`, name: '', limit: 2, members: [{ providerId, modelId: ALL_MODELS }] }
}

function validLimit(limit: number) {
  return Number.isInteger(limit) && limit >= MIN_LIMIT && limit <= MAX_LIMIT
}

export function ComputePoolsSettings() {
  const pools = useAppStore((s) => s.computePools)
  const usage = useAppStore((s) => s.computePoolUsage)
  const providers = useAppStore((s) => s.providers)
  const catalogs = useAppStore((s) => s.providerModelCatalogs)
  const defaultProviderId = useAppStore((s) => s.defaultNewChatProviderId) || DEFAULT_PROVIDER_ID
  const save = useAppStore((s) => s.setComputePools)
  const [editing, setEditing] = useState<{ draft: ComputePool; isNew: boolean; original: string } | null>(null)
  const [exitPrompt, setExitPrompt] = useState(false)
  const [pendingDelete, setPendingDelete] = useState<ComputePool | null>(null)
  const [busy, setBusy] = useState(false)
  const [removingIds, setRemovingIds] = useState<Set<string>>(() => new Set())
  const dialogRef = useRef<HTMLDivElement>(null)
  const editorOpen = editing !== null
  useEffect(() => {
    if (!editorOpen) return
    const opener = document.activeElement as HTMLElement | null
    dialogRef.current?.querySelector<HTMLElement>('input')?.focus()
    return () => opener?.focus()
  }, [editorOpen])
  const keepFocusInDialog = (event: KeyboardEvent<HTMLDivElement>) => {
    if (event.key !== 'Tab') return
    const focusable = Array.from(dialogRef.current?.querySelectorAll<HTMLElement>('button:not([disabled]), input:not([disabled])') ?? [])
    const edge = focusable[event.shiftKey ? 0 : focusable.length - 1]
    if (document.activeElement !== edge && !(event.shiftKey && document.activeElement === dialogRef.current)) return
    event.preventDefault()
    focusable[event.shiftKey ? focusable.length - 1 : 0]?.focus()
  }

  const providerName = (id: string) => providerShortName(providers.find((provider) => provider.id === id), id)
  const describe = (pool: ComputePool) => pool.members.map((member) => `${providerName(member.providerId)} · ${member.modelId || 'All models'}`).join(', ')
  const startEditing = (draft: ComputePool, isNew: boolean) => { setExitPrompt(false); setEditing({ draft, isNew, original: JSON.stringify(draft) }) }

  const draft = editing?.draft
  const dirty = editing ? JSON.stringify(editing.draft) !== editing.original : false
  const canSave = Boolean(draft && draft.name.trim() && new TextEncoder().encode(draft.name.trim()).length <= MAX_NAME_BYTES && validLimit(draft.limit) && draft.members.length > 0)
  const closeEditor = () => { if (busy) return; if (dirty) { setExitPrompt(true); dialogRef.current?.focus() } else setEditing(null) }
  const setDraft = (change: Partial<ComputePool>) => { setExitPrompt(false); setEditing((current) => current && { ...current, draft: { ...current.draft, ...change } }) }
  const setMember = (index: number, change: Partial<ComputePoolMember>) => draft && setDraft({ members: draft.members.map((member, at) => at === index ? { ...member, ...change } : member) })

  const persist = async (next: ComputePool[]) => {
    setBusy(true)
    try { return await save(next) } finally { setBusy(false) }
  }
  const saveDraft = async () => {
    if (!editing || !canSave) return
    const prepared = { ...editing.draft, name: editing.draft.name.trim() }
    const next = editing.isNew ? [...pools, prepared] : pools.map((pool) => pool.id === prepared.id ? prepared : pool)
    if (await persist(next)) setEditing(null)
  }
  const deletePool = async () => {
    if (!pendingDelete) return
    const { id } = pendingDelete
    setPendingDelete(null)
    // The row leaves through its exit before the store drops it; a failed save brings it back.
    setRemovingIds((current) => new Set(current).add(id))
    if (motionAllowed()) await new Promise((resolve) => window.setTimeout(resolve, scaledMs(EXIT_MS)))
    await persist(pools.filter((pool) => pool.id !== id))
    setRemovingIds((current) => { const next = new Set(current); next.delete(id); return next })
  }

  return <div className="grid gap-0">
    <ConfirmDialog open={pendingDelete !== null} title="Delete compute pool?" confirmLabel="Delete" busy={busy} onCancel={() => setPendingDelete(null)} onConfirm={() => void deletePool()}>Chats using this pool stop waiting for its slots.</ConfirmDialog>
    <SettingsSection title="Pools"
      actions={<Button size="sm" leadingIcon={<Plus size={14} />} disabled={busy} onClick={() => startEditing(newPool(defaultProviderId), true)}>Add pool</Button>}>
      <Reveal open={pools.length === 0}><p className="uam-prov-sub">No pools yet. Without one, chats run with no limit.</p></Reveal>
      {pools.map((pool) => {
        const inUse = usage[pool.id]?.inUse ?? 0
        const waiting = usage[pool.id]?.waiting ?? 0
        return <Reveal key={pool.id} open={!removingIds.has(pool.id)}><SettingRow icon={<Cpu size={15} aria-hidden />} title={pool.name} description={describe(pool)}
          status={`${inUse}/${pool.limit} in use${waiting > 0 ? ` · ${waiting} waiting` : ''}`}
          control={<>
            <IconButton size="sm" icon={<Pencil size={14} />} label={`Edit ${pool.name}`} disabled={busy} onClick={() => startEditing(pool, false)} />
            <IconButton size="sm" icon={<X size={14} />} label={`Delete ${pool.name}`} disabled={busy} onClick={() => setPendingDelete(pool)} />
          </>} /></Reveal>
      })}
    </SettingsSection>

    <Presence open={editing !== null}>
      <Overlay open zIndex={60} onBackdropMouseDown={closeEditor}>
        {editing && draft && <div role="dialog" aria-modal="true" aria-label={editing.isNew ? 'New compute pool' : `Edit ${draft.name}`} tabIndex={-1} className="uam-mcp-dialog uam-central-dialog" ref={dialogRef}
          onKeyDown={(event) => { if ((event.target as HTMLElement).closest('[data-viewport-menu]')) return; keepFocusInDialog(event); if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); closeEditor() } }}>
          <header className="uam-central-dialog__head">
            <span className="text-sm font-semibold">{editing.isNew ? 'New compute pool' : `Edit ${draft.name}`}</span>
            <IconButton size="sm" icon={<X size={14} />} label="Close compute pool editor" disabled={busy} onClick={closeEditor} />
          </header>
          <div className="uam-central-dialog__body">
            <SettingsField label="Name">
              <input className="uam-field w-full" aria-label="Pool name" value={draft.name} maxLength={80} placeholder="InferDeck" disabled={busy} onChange={(event) => setDraft({ name: event.currentTarget.value })} />
            </SettingsField>
            <SettingsField label="Chats running at once" hint={`From ${MIN_LIMIT} to ${MAX_LIMIT}. Later turns wait for a slot.`}>
              <input className="uam-field w-full" aria-label="Chats running at once" type="number" min={MIN_LIMIT} max={MAX_LIMIT} step={1} value={Number.isNaN(draft.limit) ? '' : draft.limit} disabled={busy} onChange={(event) => setDraft({ limit: event.currentTarget.valueAsNumber })} />
            </SettingsField>
            <SettingsField label="Providers and models" hint="Choose All models to include chats on the provider default.">
              <div className="grid gap-2">
                {draft.members.map((member, index) => {
                  const provider = providers.find((candidate) => candidate.id === member.providerId)
                  const availableModels = catalogs.filter((catalog) => catalog.providerId === member.providerId).flatMap((catalog) => catalog.availableModels ?? [])
                  const models = buildModelOptions({ availableModels }, member.modelId, provider, member.providerId).filter((option, at, all) => option.id && all.findIndex((other) => other.id === option.id) === at)
                  const modelOptions = [{ value: ALL_MODELS, label: 'All models' }, ...models.map((option) => ({ value: option.id, label: option.label }))]
                  if (member.modelId && !models.some((option) => option.id === member.modelId)) modelOptions.push({ value: member.modelId, label: member.modelId })
                  return <div key={index} className="grid grid-cols-[1fr_1fr_auto] items-center gap-2">
                    <MenuSelect label="Provider" value={member.providerId} disabled={busy} options={providers.map((option) => ({ value: option.id, label: providerName(option.id) }))} onChange={(providerId) => setMember(index, { providerId, modelId: ALL_MODELS })} />
                    <MenuSelect label="Model" value={member.modelId} disabled={busy} options={modelOptions} onChange={(modelId) => setMember(index, { modelId })} />
                    <IconButton size="sm" icon={<X size={14} />} label="Remove provider" disabled={busy || draft.members.length === 1} onClick={() => setDraft({ members: draft.members.filter((_, at) => at !== index) })} />
                  </div>
                })}
                <div><Button size="sm" leadingIcon={<Plus size={14} />} disabled={busy} onClick={() => setDraft({ members: [...draft.members, { providerId: defaultProviderId, modelId: ALL_MODELS }] })}>Add provider</Button></div>
              </div>
            </SettingsField>
          </div>
          <footer className="uam-central-dialog__foot">
            <Reveal open={exitPrompt} className="flex min-w-0 flex-1 items-center justify-end gap-2">
              <p className="mr-auto text-sm" role="status">Your changes have not been saved.</p>
              <Button size="sm" onClick={() => { setExitPrompt(false); dialogRef.current?.focus() }}>Keep editing</Button>
              <Button size="sm" variant="secondary" onClick={() => setEditing(null)}>Discard</Button>
            </Reveal>
            <Reveal open={!exitPrompt} className="flex items-center justify-end gap-2">
              <Button size="sm" disabled={busy} onClick={closeEditor}>Cancel</Button>
              <SaveButton saving={busy} loading={busy} disabled={busy || !dirty || !canSave} onClick={() => void saveDraft()}>Save pool</SaveButton>
            </Reveal>
          </footer>
        </div>}
      </Overlay>
    </Presence>
  </div>
}
