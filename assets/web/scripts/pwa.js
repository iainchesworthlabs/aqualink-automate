// Progressive Web App service-worker registration.
//
// Service workers require a secure context.  Browsers already restrict
// registration to HTTPS and to localhost/127.0.0.1, so we simply attempt
// registration when the API is available and let the platform enforce the
// transport rule (an insecure non-localhost origin will reject it, which we
// log and otherwise ignore — the app continues to work without the SW).
(function registerServiceWorker() {
  if (!('serviceWorker' in navigator)) {
    return;
  }

  // Skip the service worker when running under Home Assistant ingress (a non-empty
  // base prefix). Its scope would be the per-session ingress path and its precache
  // list uses absolute "/…" URLs that resolve to the HA host root — both wrong
  // there. The UI works fine without the SW; it stays enabled for direct serving.
  if (window.AquaBase) {
    return;
  }

  // When an UPDATED worker takes control (e.g. a redeployed shell that bumped
  // CACHE_VERSION, which calls skipWaiting()+clients.claim()), reload once so
  // this tab runs the fresh JS/CSS it now serves instead of the assets it
  // loaded under the old worker. Guarded two ways: only armed when the page was
  // already controlled at load (a genuine update, not a first-ever install), and
  // the `refreshing` latch prevents a reload loop.
  if (navigator.serviceWorker.controller) {
    var refreshing = false;
    navigator.serviceWorker.addEventListener('controllerchange', function onControllerChange() {
      if (refreshing) {
        return;
      }
      refreshing = true;
      window.location.reload();
    });
  }

  window.addEventListener('load', function onLoad() {
    navigator.serviceWorker.register('/sw.js').catch(function onError(err) {
      console.warn('Service worker registration failed:', err);
    });
  });
})();
