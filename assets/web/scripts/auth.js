/**
 * UI authentication (WS5).
 *
 * Two layers:
 *   1. A module-scope IIFE (runs immediately) that owns the bearer token, wraps
 *      window.fetch to attach `Authorization: Bearer <token>` to same-origin
 *      /api requests, and emits an `auth:unauthorized` event on a 401.
 *   2. An Alpine `auth` store (registered on alpine:init) that drives the login
 *      overlay: probing /api/auth/check, logging in, and logging out.
 *
 * When the server has no token configured, /api/auth/check returns 200, the
 * store marks itself ready, no login is ever shown, and no token is attached —
 * behaviour is identical to a build without auth.
 */
(function installAuthTransport() {
    const STORAGE_KEY = 'aqualink_token';

    function readToken() {
        try {
            return sessionStorage.getItem(STORAGE_KEY) || localStorage.getItem(STORAGE_KEY) || '';
        } catch (_) {
            return '';
        }
    }

    window.AqualinkAuth = {
        token: readToken,
        setToken(token, remember) {
            try {
                sessionStorage.removeItem(STORAGE_KEY);
                localStorage.removeItem(STORAGE_KEY);
                (remember ? localStorage : sessionStorage).setItem(STORAGE_KEY, token);
            } catch (_) { /* storage unavailable */ }
        },
        clearToken() {
            try {
                sessionStorage.removeItem(STORAGE_KEY);
                localStorage.removeItem(STORAGE_KEY);
            } catch (_) { /* storage unavailable */ }
        },
        // Browsers can't set an Authorization header on a WebSocket upgrade, so
        // the token rides as a `bearer.<token>` subprotocol next to `aqualink`.
        wsSubprotocols() {
            const t = readToken();
            return t ? ['aqualink', 'bearer.' + t] : undefined;
        },
    };

    const origFetch = window.fetch.bind(window);

    function isApiUrl(url) {
        if (!url) return false;
        return url.startsWith('/api') || url.startsWith(window.location.origin + '/api');
    }

    window.fetch = function patchedFetch(input, init) {
        init = init || {};
        const url = (typeof input === 'string') ? input : (input && input.url) || '';
        const api = isApiUrl(url);

        if (api) {
            const token = readToken();
            if (token) {
                const headers = new Headers(init.headers || (typeof input !== 'string' && input.headers) || {});
                headers.set('Authorization', 'Bearer ' + token);
                init = Object.assign({}, init, { headers });
            }
        }

        return origFetch(input, init).then(function (resp) {
            if (api && resp.status === 401) {
                window.dispatchEvent(new CustomEvent('auth:unauthorized'));
            }
            return resp;
        });
    };
})();

document.addEventListener('alpine:init', () => {
    Alpine.store('auth', {
        ready: false,       // authorized OR auth disabled -> the app may run
        required: false,    // the server requires a token (drives the logout control)
        showLogin: false,
        token: '',          // bound to the login input
        remember: false,
        error: '',
        busy: false,

        async check() {
            this.busy = true;
            try {
                const resp = await window.fetch('/api/auth/check');
                if (resp.ok) {
                    this.ready = true;
                    this.showLogin = false;
                    // A stored token that the server accepted means auth IS required
                    // and we are logged in -> expose the logout control. No token
                    // (auth disabled) -> no logout control.
                    this.required = !!window.AqualinkAuth.token();
                    return true;
                }
                if (resp.status === 401) {
                    this.required = true;
                    this.ready = false;
                    this.showLogin = true;
                    return false;
                }
            } catch (_) {
                // Network error — surface the login screen so the user can retry.
            } finally {
                this.busy = false;
            }
            this.ready = false;
            this.showLogin = true;
            return false;
        },

        async login() {
            this.error = '';
            const token = (this.token || '').trim();
            if (!token) { this.error = window.AquaI18n.t('login.error_empty'); return; }

            window.AqualinkAuth.setToken(token, this.remember);
            const ok = await this.check();
            if (ok) {
                this.token = '';
                window.dispatchEvent(new CustomEvent('auth:ready'));
            } else {
                window.AqualinkAuth.clearToken();
                this.error = window.AquaI18n.t('login.error_rejected');
                this.showLogin = true;
            }
        },

        logout() {
            window.AqualinkAuth.clearToken();
            this.ready = false;
            this.required = true;
            this.showLogin = true;
            try {
                Alpine.store('ws').disconnectEquipment();
                Alpine.store('ws').disconnectStats();
            } catch (_) { /* ws store may not be ready */ }
        },
    });

    // A 401 from any /api call means the stored token is missing/stale: drop it
    // and surface the login overlay.
    window.addEventListener('auth:unauthorized', () => {
        const store = Alpine.store('auth');
        if (!store) return;
        window.AqualinkAuth.clearToken();
        store.ready = false;
        store.required = true;
        store.showLogin = true;
    });
});
