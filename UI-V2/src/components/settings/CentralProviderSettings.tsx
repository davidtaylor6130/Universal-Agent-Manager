import { useEffect, useRef, useState } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { Button } from '../ui'

/** Shared launch resources. Provider defaults and agent definitions retain their existing editors. */
export function CentralProviderSettings() {
  const saved = useAppStore(state => state.centralProviderConfiguration)
  const save = useAppStore(state => state.setCentralProviderConfiguration)
  const [draft, setDraft] = useState(saved)
  const [instructionPaths, setInstructionPaths] = useState(saved.instructionFiles.join('\n'))
  const [skillPaths, setSkillPaths] = useState(saved.skillDirectories.join('\n'))
  const [pending, setPending] = useState(false)
  const [message, setMessage] = useState('')
  const dirty = useRef(false)
  useEffect(() => {
    if (dirty.current) return
    setDraft(saved)
    setInstructionPaths(saved.instructionFiles.join('\n'))
    setSkillPaths(saved.skillDirectories.join('\n'))
  }, [saved])
  const fieldClass = 'w-full bg-black text-white border border-white/20 px-3 py-2 text-sm'
  const pathList = (text: string) => text.split('\n').map(path => path.trim()).filter(Boolean)
  return <div className="space-y-5 text-white">
    <label className="flex items-center gap-3 text-sm"><input type="checkbox" checked={draft.enabled} disabled={pending} onChange={event => { dirty.current = true; setDraft({ ...draft, enabled: event.target.checked }) }} />Apply central setup on provider launch</label>
    <label className="block text-sm">Instructions<textarea aria-label="Central instructions" className={`${fieldClass} mt-2`} rows={8} value={draft.instructions} disabled={pending} onChange={event => { dirty.current = true; setDraft({ ...draft, instructions: event.target.value }) }} /></label>
    <label className="block text-sm">Instruction files, one absolute local path per line<textarea aria-label="Central instruction files" className={`${fieldClass} mt-2`} rows={3} value={instructionPaths} disabled={pending} onChange={event => { dirty.current = true; setInstructionPaths(event.target.value) }} /></label>
    <label className="block text-sm">Extra skill folders, one absolute local path per line<textarea aria-label="Central skill folders" className={`${fieldClass} mt-2`} rows={3} value={skillPaths} disabled={pending} onChange={event => { dirty.current = true; setSkillPaths(event.target.value) }} /></label>
    <label className="block text-sm">Default agent for new chats<input aria-label="Central default agent" className={`${fieldClass} mt-2`} value={draft.defaultAgentId} disabled={pending} onChange={event => { dirty.current = true; setDraft({ ...draft, defaultAgentId: event.target.value }) }} /></label>
    <label className="flex items-center gap-3 text-sm"><input type="checkbox" checked={draft.uamControlEnabled} disabled={pending} onChange={event => { dirty.current = true; setDraft({ ...draft, uamControlEnabled: event.target.checked }) }} />Enable UAM tools for new chats</label>
    <p className="text-sm">The Skills library is included. Provider defaults, agents and MCP servers use their existing settings. SSH instruction files belong to each remote host. Changes apply on the next launch.</p>
    <Button size="sm" loading={pending} onClick={async () => {
      setPending(true); setMessage('')
      try { const result = await save({ ...draft, instructionFiles: pathList(instructionPaths), skillDirectories: pathList(skillPaths) }); if (result.ok) dirty.current = false; setMessage(result.ok ? 'Central setup saved.' : result.error || 'Could not save central setup.') }
      catch { setMessage('Could not save central setup.') }
      finally { setPending(false) }
    }}>Save</Button>
    {message && <p role="status" className="text-sm">{message}</p>}
  </div>
}
