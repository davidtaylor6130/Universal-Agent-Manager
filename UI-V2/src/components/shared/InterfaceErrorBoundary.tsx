import { Component, createRef, type ErrorInfo, type ReactNode } from 'react'

interface Props {
  children: ReactNode
  panel?: boolean
  onDismiss?: () => void
}

/** Contains render and lazy-import failures without taking unrelated chats down. */
export class InterfaceErrorBoundary extends Component<Props, { failed: boolean }> {
  state = { failed: false }
  private recoveryRef = createRef<HTMLElement>()

  static getDerivedStateFromError() {
    return { failed: true }
  }

  componentDidCatch(error: Error, info: ErrorInfo) {
    console.error('[UI recovery]', error, info.componentStack)
    this.recoveryRef.current?.focus()
  }

  render() {
    if (!this.state.failed) return this.props.children
    return (
      <section ref={this.recoveryRef} tabIndex={-1} role="alert" aria-label="Interface recovery" style={{ background: '#000', color: '#fff', padding: 24, fontFamily: 'system-ui', position: this.props.panel ? 'fixed' : undefined, inset: this.props.panel ? 'auto 16px 16px' : undefined, zIndex: this.props.panel ? 1000 : undefined }}>
        <h1 style={{ fontSize: 16, margin: '0 0 12px' }}>{this.props.panel ? 'This panel could not load.' : 'The interface could not load.'}</h1>
        <p style={{ margin: '0 0 16px' }}>{this.props.panel ? 'Other chats remain available.' : 'Reload the interface to try again.'}</p>
        <p style={{ margin: '0 0 16px' }}>Active sessions keep running. Unsaved drafts may be lost when reloading.</p>
        {this.props.onDismiss && <button className="uam-recovery-button" onClick={this.props.onDismiss}>Dismiss</button>}
        <button className="uam-recovery-button" onClick={() => window.location.reload()}>Reload interface</button>
      </section>
    )
  }
}
