import { useEffect, useState } from 'react'
import type { VcsCommitStatus } from '../store/cpp/types'

export interface VcsContextTarget {
  id: string
  workspaceDirectory?: string
  executionHostId?: string
}

type LoadContext = (id: string, vcsType: 'git', options: { includeLineStats: false; contextOnly: true; requestId: string }) => Promise<VcsCommitStatus | null>

/** Owns a lightweight repository request by chat, workspace, and host. */
export function useVcsContext(target: VcsContextTarget, load: LoadContext, refreshToken?: unknown): VcsCommitStatus | null {
  const identity = JSON.stringify([target.id, target.workspaceDirectory ?? '', target.executionHostId ?? 'local'])
  const [result, setResult] = useState<{ identity: string; status: VcsCommitStatus | null } | null>(null)
  useEffect(() => {
    let disposed = false
    let request = 0
    let pending = false
    const refresh = async () => {
      if (disposed || pending || !target.workspaceDirectory?.trim()) return
      pending = true
      const currentRequest = ++request
      try {
        const status = await load(target.id, 'git', { includeLineStats: false, contextOnly: true, requestId: `vcs-context-${identity}-${currentRequest}` })
        if (!disposed && currentRequest === request) setResult({ identity, status })
      } catch {
        if (!disposed && currentRequest === request) setResult({ identity, status: null })
      } finally {
        pending = false
      }
    }
    const onFocus = () => { void refresh() }
    void refresh()
    window.addEventListener('focus', onFocus)
    const interval = window.setInterval(() => {
      if (document.visibilityState === 'visible') void refresh()
    }, 30000)
    return () => {
      disposed = true
      ++request
      window.removeEventListener('focus', onFocus)
      window.clearInterval(interval)
    }
  }, [identity, load, refreshToken])
  return result?.identity === identity ? result.status : null
}

/** Labels actual repository status; SVN uses the workspace name instead of inventing a branch. */
export function vcsContextLabel(status: VcsCommitStatus | null): string | null {
  if (!status?.available || status.error) return null
  if (status.activeVcsType === 'svn') {
    const name = status.workspaceDirectory.replace(/[\\/]+$/, '').split(/[\\/]/).pop()
    return name ? `SVN: ${name}` : 'SVN'
  }
  return status.branchOrRevision.trim() ? `Git: ${status.branchOrRevision.trim()}` : 'Git'
}
