// Adds the cross-origin isolation headers (COOP/COEP), which wasm threads need, to every response
// on hosts that can't send them themselves, such as GitHub Pages. shell.js registers it only when
// the page isn't isolated already, then reloads once so the page comes back through it. Hosts that
// can set headers should (see _headers); then this worker is never installed.
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));

self.addEventListener('fetch', (event) => {
  const request = event.request;
  // Fetching these through the worker would fail.
  if (request.cache === 'only-if-cached' && request.mode !== 'same-origin') return;
  event.respondWith((async () => {
    const response = await fetch(request);
    if (response.status === 0) return response; // opaque
    const headers = new Headers(response.headers);
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
  })());
});
