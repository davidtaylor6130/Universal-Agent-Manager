// Runs before React and has no bundle dependencies. It also detects a cleared root.
(() => {
  const root = document.getElementById('root')
  const fallback = document.getElementById('uam-startup-recovery')
  if (!root || !fallback) return
  const show = () => { fallback.hidden = false }
  let mounted = false
  const deadline = window.setTimeout(() => { if (!mounted) show() }, 15000)
  const observer = new MutationObserver(() => {
    if (root.childNodes.length) {
      mounted = true
      window.clearTimeout(deadline)
      fallback.hidden = true
    } else if (mounted) show()
  })
  observer.observe(root, { childList: true })
  window.addEventListener('error', (event) => {
    if (!mounted && (event.target instanceof HTMLScriptElement || event.error)) show()
  }, true)
  window.addEventListener('unhandledrejection', () => { if (!mounted || !root.childNodes.length) show() })
  document.getElementById('uam-reload-interface')?.addEventListener('click', () => window.location.reload())
})()
