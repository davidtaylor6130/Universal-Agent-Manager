import { useEffect, useState } from 'react'
import { isCompanionContext, sendToCEF } from '../../ipc/cefBridge'
import { ConfirmDialog } from '../ui'

interface LegacyChatSource { path: string; chatCount: number }
interface LegacyChatCopyResult { status?: 'complete' | 'degraded' | 'failed'; importedCount?: number; failedCount?: number; errors?: string[] }

/** Asks about each earlier-release chat history found at startup: copy it, start brand new, or stop UAM. */
export function LegacyChatImportPrompt() {
  const [sources, setSources] = useState<LegacyChatSource[]>([])
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  useEffect(() => {
    if (isCompanionContext()) return
    let active = true
    void sendToCEF<{ sources?: LegacyChatSource[] }>({ action: 'getLegacyChatImports' }).then((response) => {
      if (active && response.ok) setSources(response.data?.sources ?? [])
    })
    return () => { active = false }
  }, [])
  const source = sources[0]
  if (!source) return null

  const resolve = async (choice: 'copy' | 'brandNew') => {
    setBusy(true)
    setError('')
    const response = await sendToCEF<LegacyChatCopyResult>({ action: 'resolveLegacyChatImport', payload: { path: source.path, choice } })
    setBusy(false)
    if (!response.ok) {
      setError(response.error || 'Nothing was changed. Try again.')
      return
    }
    const result = response.data
    if (result?.status && result.status !== 'complete') {
      setError(result.errors?.[0] || `${result.failedCount ?? 0} chats could not be copied. Copy again to retry.`)
      return
    }
    setSources((current) => current.slice(1))
  }
  const stop = async () => {
    setBusy(true)
    setError('')
    const response = await sendToCEF({ action: 'quitApplication' }, false)
    if (!response.ok) {
      setBusy(false)
      setError(response.error || 'Universal Agent Manager could not close. Close it from the window instead.')
    }
  }

  return (
    <ConfirmDialog
      open
      tone="primary"
      title="Chat history from a previous version"
      confirmLabel="Copy chats"
      cancelLabel="Stop"
      secondaryAction={{ label: 'Brand new', onClick: () => void resolve('brandNew') }}
      dismissible={false}
      busy={busy}
      error={error}
      onConfirm={() => void resolve('copy')}
      onCancel={() => void stop()}
    >
      <p>Found {source.chatCount} {source.chatCount === 1 ? 'chat' : 'chats'} from an earlier version in {source.path}.</p>
      <p className="mt-2">Copy chats adds them to your current history and leaves the originals in place. Brand new keeps your current history and stops asking about this folder. Stop closes Universal Agent Manager without changing anything.</p>
    </ConfirmDialog>
  )
}
