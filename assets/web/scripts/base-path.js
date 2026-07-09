// Ingress base-path shim — MUST be the first script the page loads (synchronous,
// in <head>, before any deferred bundle or dynamic asset injection).
//
// Home Assistant "ingress" serves this UI under a per-session path prefix
// (/api/hassio_ingress/<token>/) and proxies requests to the add-on with that
// prefix stripped, so the backend still sees "/", "/api/…", "/ws/…" unchanged.
// The browser, however, resolves absolute ("/…") URLs against the Home Assistant
// host root, which bypasses the prefix. Because the app uses hash routing,
// location.pathname is always pinned to the app root — the ingress prefix under
// Home Assistant, or "/" when the UI is served directly — so we can derive the
// prefix here and rebase same-origin absolute URLs onto it.
//
// Coverage:
//   - Static assets in index.html are authored RELATIVE, so they resolve against
//     this base for free (no work here).
//   - fetch("/api/…") is rebased by the wrapper below (covers every API call).
//   - WebSocket URLs are rebased in ws-store's wsUrl() via window.AquaBase.
//   - Dynamically injected catalog <script>s (i18n.js) use window.AquaBase.
//
// When served at the site root the base is "" and everything below is a no-op.
(function () {
    'use strict';

    // Hash routing keeps pathname at the app root; strip any trailing "/index.html"
    // or trailing slashes to get the bare prefix ("" at root).
    var path = window.location.pathname.replace(/\/index\.html$/, '/');
    var base = path.replace(/\/+$/, '');

    // Public helpers used by scripts that build URLs by hand (ws-store, i18n).
    window.AquaBase = base;
    window.aquaUrl = function (p) {
        return (typeof p === 'string' && p.charAt(0) === '/' && p.charAt(1) !== '/') ? base + p : p;
    };

    if (base === '') {
        return; // served at the site root: nothing to rebase.
    }

    // Rebase same-origin absolute-path fetches onto the ingress prefix. Full URLs
    // (scheme-relative "//host" or absolute "http(s)://…") and already-relative
    // paths pass through untouched.
    var nativeFetch = window.fetch.bind(window);
    window.fetch = function (input, init) {
        if (typeof input === 'string' && input.charAt(0) === '/' && input.charAt(1) !== '/') {
            input = base + input;
        }
        return nativeFetch(input, init);
    };
})();
