// Notifications only. Never cache authenticated companion data.
self.addEventListener('notificationclick', (event) => {
  event.notification.close()
  const chatId = event.notification.data?.chatId
  const url = new URL('/companion', self.location.origin)
  if (chatId) url.searchParams.set('chat', chatId)
  event.waitUntil(self.clients.matchAll({ type: 'window' }).then(async (windows) => {
    const existing = windows.find((client) => new URL(client.url).pathname === '/companion')
    if (existing) {
      existing.postMessage({ type: 'openChat', chatId })
      await existing.focus()
    } else await self.clients.openWindow(url.href)
  }))
})
