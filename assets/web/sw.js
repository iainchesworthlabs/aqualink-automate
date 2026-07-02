/*
 * Aqualink Automate — service worker.
 *
 * Scope: progressive-web-app install + offline app shell ONLY.  Explicitly out
 * of scope (see WS7): offline command queueing and push notifications.
 *
 * Caching strategy:
 *   - Static shell (JS / CSS / icons / fonts-from-same-origin): NETWORK FIRST,
 *     falling back to the cached copy only when the network is unreachable. The
 *     server already marks these assets `no-cache` (revalidate via ETag); a
 *     cache-first service worker silently IGNORES that — the Cache Storage API
 *     does not honour HTTP cache semantics — and would pin returning clients to
 *     stale JS/CSS until CACHE_VERSION is bumped, even though the HTML (served
 *     network-first below) is fresh. That fresh-HTML + stale-JS mismatch breaks
 *     the UI until a hard reload. Network-first keeps online clients current and
 *     still boots offline from the last good copy.
 *   - Navigations: try the network, serve the cached app shell (index.html) when
 *     the network is unavailable so the SPA still boots offline.
 *   - /api/*  : NETWORK ONLY.  Never cached and never served from cache — API
 *     responses (equipment state, auth checks, etc.) must always be live, and a
 *     stale cached /api/auth/check could wrongly keep an unauthenticated client
 *     "logged in".
 *   - /ws/*   : never intercepted (WebSocket upgrades are not cacheable and must
 *     reach the backend untouched).
 *
 * The cache name is stamped with the build version at build/package time (see
 * cmake/stamp_sw_version.cmake), so each release gets its own cache and stale
 * offline shells purge on activate. The literal below is only the source/dev
 * fallback when the file is served unstamped (e.g. --doc-root <assets/web>);
 * with the network-first strategy above, online freshness never depends on it.
 */

const CACHE_VERSION = 'v3';
const CACHE_NAME = `aqualink-shell-${CACHE_VERSION}`;

// Core app-shell assets to precache so the UI boots offline.  Kept deliberately
// small and tolerant of individual failures (see install) — runtime caching
// below fills in everything else on first use.
const PRECACHE_URLS = [
  '/',
  '/index.html',
  '/manifest.json',
  '/favicon.svg',
  '/i18n/en.js',
  '/styles/app.css',
  '/styles/components.css',
  '/vendor/fonts/fonts.css',
  '/vendor/qrcode.min.js',
  '/vendor/alpine.min.js',
  '/vendor/chart.umd.min.js',
  '/icons/icon-192.png',
  '/icons/icon-512.png',
];

self.addEventListener('install', (event) => {
  event.waitUntil(
    (async () => {
      const cache = await caches.open(CACHE_NAME);
      // Cache entries individually so one missing optional asset does not abort
      // the whole install (addAll is all-or-nothing).
      await Promise.allSettled(PRECACHE_URLS.map((url) => cache.add(url)));
      await self.skipWaiting();
    })(),
  );
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    (async () => {
      const names = await caches.keys();
      await Promise.all(
        names
          .filter((name) => name.startsWith('aqualink-shell-') && name !== CACHE_NAME)
          .map((name) => caches.delete(name)),
      );
      await self.clients.claim();
    })(),
  );
});

self.addEventListener('fetch', (event) => {
  const { request } = event;

  // Only ever handle GET; let everything else go straight to the network.
  if (request.method !== 'GET') {
    return;
  }

  const url = new URL(request.url);

  // Same-origin only for our caching logic; cross-origin (e.g. Google Fonts)
  // is left to the browser's own handling.
  if (url.origin !== self.location.origin) {
    return;
  }

  // Never intercept WebSocket endpoints or the live API.
  if (url.pathname.startsWith('/ws')) {
    return;
  }
  if (url.pathname.startsWith('/api')) {
    // Network only — do not consult or populate the cache.
    event.respondWith(fetch(request));
    return;
  }

  // App-shell navigations: try the network, fall back to the cached shell so
  // the SPA still loads offline.
  if (request.mode === 'navigate') {
    event.respondWith(
      (async () => {
        try {
          return await fetch(request);
        } catch {
          const cache = await caches.open(CACHE_NAME);
          const cached = (await cache.match(request)) || (await cache.match('/index.html'));
          return cached || Response.error();
        }
      })(),
    );
    return;
  }

  // Static assets: network first, falling back to cache when offline. Always
  // prefer the live file so a redeployed JS/CSS bundle reaches the client on the
  // next normal load (no hard reload needed); refresh the cached copy on every
  // successful fetch so the offline fallback stays current.
  event.respondWith(
    (async () => {
      const cache = await caches.open(CACHE_NAME);
      try {
        const response = await fetch(request);
        // Only cache successful, basic (same-origin) responses.
        if (response && response.ok && response.type === 'basic') {
          cache.put(request, response.clone());
        }
        return response;
      } catch (err) {
        // Offline — serve the last good cached copy if we have one.
        const cached = await cache.match(request);
        if (cached) {
          return cached;
        }
        // Offline and not cached — surface the network failure.
        throw err;
      }
    })(),
  );
});
