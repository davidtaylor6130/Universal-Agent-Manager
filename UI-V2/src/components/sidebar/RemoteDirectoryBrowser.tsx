import { useCallback, useEffect, useRef, useState } from 'react'
import { ArrowUp, ChevronRight, Folder, Server, X } from 'lucide-react'
import type { ExecutionHost, RemoteDirectoryListing } from '../../types/session'
import { useAppStore } from '../../store/useAppStore'
import { defaultRemoteBrowsePath, isAbsoluteRemoteWorkspace } from '../../utils/remoteWorkspace'
import { Button, IconButton } from '../ui'
import { StatusIndicator } from '../shared/StatusIndicator'

interface RemoteDirectoryBrowserProps {
  host: ExecutionHost
  initialPath: string
  onCancel: () => void
  onSelect: (path: string) => void
}

export function RemoteDirectoryBrowser({ host, initialPath, onCancel, onSelect }: RemoteDirectoryBrowserProps) {
  const listRemoteDirectories = useAppStore((state) => state.listRemoteDirectories)
  const [path, setPath] = useState(initialPath.trim() || defaultRemoteBrowsePath(host.platform))
  const [listing, setListing] = useState<RemoteDirectoryListing | null>(null)
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState('')
  const [validatedDirectory, setValidatedDirectory] = useState('')
  const directoryKey = `${host.id}\n${host.platform}\n${path}`
  const canConfirm = !loading && Boolean(listing) && validatedDirectory === directoryKey &&
    path === listing?.directory && host.runnerStatus === 'ready'
  const requestRef = useRef(0)
  const pathRef = useRef<HTMLInputElement>(null)

  const load = useCallback(async (target: string) => {
    const normalized = target.trim()
    const request = ++requestRef.current
    setValidatedDirectory('')
    setPath(normalized)
    setLoading(false)
    setError('')
    if (!isAbsoluteRemoteWorkspace(host.platform, normalized)) {
      setError(`Enter an absolute ${host.platform.toLowerCase() === 'windows' ? 'Windows' : 'Unix'} path.`)
      return
    }
    setLoading(true)
    try {
      const result = await listRemoteDirectories(host.id, normalized)
      if (request !== requestRef.current) return
      if (!result.ok) {
        setError(result.error)
        return
      }
      if (!isAbsoluteRemoteWorkspace(host.platform, result.listing.directory)) {
        setError('The remote helper returned an invalid directory path.')
        return
      }
      setListing(result.listing)
      setPath(result.listing.directory)
      setValidatedDirectory(`${host.id}\n${host.platform}\n${result.listing.directory}`)
    } catch (error) {
      if (request === requestRef.current) setError(error instanceof Error ? error.message : 'The remote directory could not be listed. Try again.')
    } finally {
      if (request === requestRef.current) setLoading(false)
    }
  }, [host.id, host.platform, listRemoteDirectories])

  useEffect(() => {
    pathRef.current?.focus()
    void load(initialPath.trim() || defaultRemoteBrowsePath(host.platform))
    return () => { requestRef.current++ }
  }, [load, host.platform]) // Browse is an explicit read; reload only when the target computer changes.

  useEffect(() => {
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return
      event.preventDefault()
      event.stopImmediatePropagation()
      onCancel()
    }
    window.addEventListener('keydown', closeOnEscape)
    return () => window.removeEventListener('keydown', closeOnEscape)
  }, [onCancel])

  return (
    <div
      className="fixed inset-0 z-[70] flex items-center justify-center p-4 animate-fade-in"
      style={{ background: 'rgba(0,0,0,0.56)' }}
      onClick={(event) => { if (event.target === event.currentTarget) onCancel() }}
    >
      <div
        role="dialog"
        aria-modal="true"
        aria-label={`Choose directory on ${host.label}`}
        className="flex max-h-[min(680px,calc(100vh-32px))] w-full max-w-xl flex-col overflow-hidden rounded-xl"
        style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }}
      >
        <header className="flex items-center justify-between gap-3 px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}>
          <div className="min-w-0">
            <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>Choose remote directory</div>
            <div className="mt-1 flex items-center gap-1.5 text-xs" style={{ color: 'var(--text-2)' }}>
              <Server size={13} aria-hidden />
              <span>{host.label}</span>
              <span aria-hidden>·</span>
              <span>{host.platform}</span>
            </div>
          </div>
          <IconButton icon={<X size={16} />} variant="danger" label="Close remote directory browser" onClick={onCancel} />
        </header>

        <div className="flex min-h-0 flex-1 flex-col gap-3 p-5">
          <div>
            <label htmlFor="remote-directory-path" className="sr-only" style={{ color: 'var(--text-2)' }}>
              Directory on {host.label}
            </label>
            <div className="flex items-center gap-2">
              <div className="relative min-w-0 flex-1">
              <input
                id="remote-directory-path"
                ref={pathRef}
                value={path}
                aria-invalid={Boolean(error) || undefined}
                aria-describedby={error ? 'remote-directory-error' : undefined}
                onChange={(event) => {
                  requestRef.current++
                  setPath(event.target.value)
                  setValidatedDirectory('')
                  setLoading(false)
                  setError('')
                }}
                onKeyDown={(event) => { if (event.key === 'Enter') void load(path) }}
                className="w-full rounded-md pl-3 pr-10 py-2 text-sm outline-none"
                style={{ background: 'var(--surface-up)', border: '1px solid var(--border)', color: 'var(--text)', fontFamily: 'var(--font-mono)' }}
              />
              <span className="absolute right-3 top-1/2 -translate-y-1/2"><StatusIndicator issues={canConfirm ? [] : [error || (loading ? 'Loading directory.' : 'Load the directory before selecting it.')]} okLabel="Directory verified" /></span>
              </div>
              <Button variant="secondary" size="md" disabled={loading} onClick={() => void load(path)}>
                Go
              </Button>
            </div>
          </div>

          {error && (
            <div id="remote-directory-error" role="alert" className="flex items-center justify-between gap-2 rounded-lg px-3 py-2 text-xs" style={{ color: 'var(--red)', background: 'color-mix(in srgb, var(--red) 10%, var(--surface))', border: '1px solid color-mix(in srgb, var(--red) 35%, var(--border))' }}>
              <span className="min-w-0 break-words">{error}</span>
              <IconButton icon={<X size={14} />} label="Dismiss directory error" variant="danger" onClick={() => setError('')} />
            </div>
          )}

          <div className="min-h-[220px] flex-1 overflow-y-auto rounded-lg p-1" style={{ background: 'var(--surface-up)', border: '1px solid var(--border)' }}>
            {loading ? (
              <div role="status" className="flex h-full min-h-[220px] items-center justify-center text-sm" style={{ color: 'var(--text-2)' }}>Loading directories…</div>
            ) : listing ? (
              <>
                {validatedDirectory !== directoryKey && <p className="px-3 py-2 text-xs" style={{ color: 'var(--text-2)' }}>Last loaded: {listing.directory}</p>}
                {listing.parentDirectory && (
                  <button
                    type="button"
                    className="flex w-full cursor-pointer items-center gap-2 rounded-md px-3 py-2 text-left text-sm transition-colors duration-150 hover:bg-[var(--surface-hover)] focus-visible:outline focus-visible:outline-2 focus-visible:outline-[var(--accent)]"
                    style={{ color: 'var(--text)' }}
                    onClick={() => void load(listing.parentDirectory)}
                  >
                    <ArrowUp size={15} aria-hidden style={{ color: 'var(--text-3)' }} />
                    <span>Parent directory</span>
                  </button>
                )}
                {listing.directories.map((directory) => (
                  <button
                    key={directory.path}
                    type="button"
                    className="flex w-full cursor-pointer items-center gap-2 rounded-md px-3 py-2 text-left text-sm transition-colors duration-150 hover:bg-[var(--surface-hover)] focus-visible:outline focus-visible:outline-2 focus-visible:outline-[var(--accent)]"
                    style={{ color: 'var(--text)' }}
                    onClick={() => void load(directory.path)}
                  >
                    <Folder size={15} aria-hidden style={{ color: 'var(--accent)' }} />
                    <span className="min-w-0 flex-1 truncate">{directory.name}</span>
                    <ChevronRight size={14} aria-hidden style={{ color: 'var(--text-3)' }} />
                  </button>
                ))}
                {listing.directories.length === 0 && (
                  <div className="flex min-h-[180px] items-center justify-center px-4 text-center text-sm" style={{ color: 'var(--text-3)' }}>No child directories are visible here.</div>
                )}
              </>
            ) : null}
          </div>
          {listing?.truncated && <p className="text-xs" style={{ color: 'var(--yellow)' }}>Showing the first 200 directories. Enter a more specific path to continue.</p>}
          <p className="text-xs" style={{ color: 'var(--text-3)' }}>Read-only: UAM lists directories but does not create, rename, or modify anything on this computer.</p>
        </div>

        <footer className="flex items-center justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}>
          <Button variant="ghost" size="md" onClick={onCancel}>Cancel</Button>
          <Button
            variant="primary"
            size="md"
            disabled={!canConfirm}
            onClick={() => { if (listing && canConfirm) onSelect(listing.directory) }}
          >
            Use this directory
          </Button>
        </footer>
      </div>
    </div>
  )
}
