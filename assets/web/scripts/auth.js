/**
 * UI authentication (Wave A — session/identity model).
 *
 * Replaces the earlier shared-token paste UX with username/password sessions
 * against the identity system (docs/auth-redesign.md).  Two layers:
 *
 *   1. A module-scope IIFE (runs immediately) that owns the token pair, wraps
 *      window.fetch to attach `Authorization: Bearer <access>` to same-origin
 *      /api requests, transparently refreshes an expired access token once on a
 *      401, and emits `auth:unauthorized` when the session is truly dead.
 *   2. An Alpine `auth` store (registered on alpine:init) that mirrors
 *      GET /api/auth/me (posture / setup / identity / entitlements) and drives
 *      the setup / login / account affordances.
 *
 * Token storage (per the Wave A spec):
 *   - ACCESS token: kept in a module variable AND mirrored to sessionStorage so
 *     a same-tab reload survives without a network round-trip.  Short-lived
 *     (~15 min JWT); the fetch wrapper refreshes it silently when it expires.
 *   - REFRESH token: localStorage when "remember me" was ticked (survives a
 *     browser restart), otherwise sessionStorage (dies with the tab).  Refresh
 *     tokens are SINGLE-USE and rotate: every refresh stores the new pair.
 *
 * When posture is "disabled" the app never logs in, no token is attached, and
 * behaviour is identical to a build without the identity system.
 */
(function installAuthTransport() {
    'use strict';

    const ACCESS_KEY = 'aqualink_access';
    const REFRESH_KEY = 'aqualink_refresh';
    // Marks where the refresh token lives so a reload restores "remember me".
    const REMEMBER_KEY = 'aqualink_remember';

    // The access token is authoritative in memory; sessionStorage is a mirror so
    // a reload in the same tab can rehydrate it before Alpine boots.
    let accessToken = '';
    try { accessToken = sessionStorage.getItem(ACCESS_KEY) || ''; } catch (_) { /* storage unavailable */ }

    function remembered() {
        try { return localStorage.getItem(REMEMBER_KEY) === '1'; } catch (_) { return false; }
    }

    function readRefresh() {
        try {
            return (remembered() ? localStorage : sessionStorage).getItem(REFRESH_KEY) || '';
        } catch (_) {
            return '';
        }
    }

    function accessTokenValue() {
        return accessToken;
    }

    // Persist a freshly-minted access/refresh pair.  `remember` is only honoured
    // on the initial login; a silent refresh passes it through unchanged so the
    // rotated refresh token stays in the same store the user chose.
    function setTokens(access, refresh, remember) {
        accessToken = access || '';
        try {
            if (accessToken) { sessionStorage.setItem(ACCESS_KEY, accessToken); }
            else { sessionStorage.removeItem(ACCESS_KEY); }

            if (typeof remember === 'boolean') {
                localStorage.setItem(REMEMBER_KEY, remember ? '1' : '0');
            }

            // Write the refresh token to exactly one store; clear the other so a
            // stale copy can never be picked up after the remember choice changes.
            const useLocal = remembered();
            const primary = useLocal ? localStorage : sessionStorage;
            const secondary = useLocal ? sessionStorage : localStorage;
            secondary.removeItem(REFRESH_KEY);
            if (refresh) { primary.setItem(REFRESH_KEY, refresh); }
            else { primary.removeItem(REFRESH_KEY); }
        } catch (_) { /* storage unavailable */ }
    }

    function clearTokens() {
        accessToken = '';
        try {
            sessionStorage.removeItem(ACCESS_KEY);
            sessionStorage.removeItem(REFRESH_KEY);
            localStorage.removeItem(REFRESH_KEY);
            localStorage.removeItem(REMEMBER_KEY);
        } catch (_) { /* storage unavailable */ }
    }

    window.AqualinkAuth = {
        // The access token (prefs-sync.js treats a truthy value as "logged in").
        token: accessTokenValue,
        refreshToken: readRefresh,
        setTokens,
        clearTokens,
        // Browsers can't set an Authorization header on a WebSocket upgrade, so
        // the access token rides as a `bearer.<token>` subprotocol next to
        // `aqualink`.  Undefined when no token is stored (unauthenticated /
        // posture-disabled connect).
        wsSubprotocols() {
            const t = accessTokenValue();
            return t ? ['aqualink', 'bearer.' + t] : undefined;
        },
        // Exposed so the ws-store can force a refresh when a socket upgrade is
        // rejected (401/403).  Resolves true when a fresh access token is
        // available, false when the session is dead.
        tryRefresh() {
            return refreshAccessToken();
        },
    };

    const origFetch = window.fetch.bind(window);

    function isApiUrl(url) {
        if (!url) return false;
        return url.startsWith('/api') || url.startsWith(window.location.origin + '/api');
    }

    // Auth endpoints must NOT trigger the silent-refresh dance: a 401 from
    // /login is a bad password, and /refresh failing is exactly what tells us the
    // session is dead.  (/me is open and never 401s under the identity system.)
    function isAuthEndpoint(url) {
        return url.indexOf('/api/auth/') !== -1;
    }

    function withBearer(init, input) {
        const headers = new Headers(init.headers || (typeof input !== 'string' && input && input.headers) || {});
        if (accessToken) { headers.set('Authorization', 'Bearer ' + accessToken); }
        return Object.assign({}, init, { headers });
    }

    // A single in-flight refresh shared by every concurrent 401 so a burst of
    // requests rotates the single-use refresh token exactly once.
    let refreshInFlight = null;

    function refreshAccessToken() {
        if (refreshInFlight) { return refreshInFlight; }

        const refresh = readRefresh();
        if (!refresh) { return Promise.resolve(false); }

        refreshInFlight = origFetch('/api/auth/refresh', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ refresh_token: refresh }),
        })
            .then((resp) => {
                if (!resp.ok) { return false; }
                return resp.json().then((data) => {
                    if (!data || !data.access_token) { return false; }
                    // Rotate: store the new pair in the same store the user chose.
                    setTokens(data.access_token, data.refresh_token || refresh);
                    return true;
                });
            })
            .catch(() => false)
            .finally(() => { refreshInFlight = null; });

        return refreshInFlight;
    }

    window.fetch = function patchedFetch(input, init) {
        init = init || {};
        const url = (typeof input === 'string') ? input : (input && input.url) || '';
        const api = isApiUrl(url);

        if (!api) { return origFetch(input, init); }

        // Whether this request carried (or could refresh) a session. A guest
        // browsing anonymously has neither; a 401 for them is an ordinary
        // "not entitled" answer on a subject-scoped endpoint, NOT a dead
        // session — so it must not trigger the teardown/redirect below.
        const hadCred = !!accessToken || !!readRefresh();

        const firstInit = accessToken ? withBearer(init, input) : init;

        return origFetch(input, firstInit).then((resp) => {
            if (resp.status !== 401 || isAuthEndpoint(url)) {
                return resp;
            }

            // A 401 from a normal /api call means the access token expired (or is
            // missing).  Attempt exactly ONE silent refresh, then replay once.
            return refreshAccessToken().then((ok) => {
                if (!ok) {
                    // Only tear the session down if there was one. For an
                    // anonymous guest (no credential) a 401 is expected on
                    // endpoints outside the Guest scope; surface it quietly.
                    if (hadCred) {
                        clearTokens();
                        window.dispatchEvent(new CustomEvent('auth:unauthorized'));
                    }
                    return resp; // Surface the original 401 to the caller.
                }
                const replayInit = withBearer(init, input);
                return origFetch(input, replayInit);
            });
        });
    };
})();

document.addEventListener('alpine:init', () => {
    Alpine.store('auth', {
        // Posture + lifecycle
        posture: 'disabled',   // 'enabled' | 'disabled'
        ready: false,          // authorised OR posture disabled -> the app may run
        authenticated: false,  // a valid credential was presented
        setupRequired: false,  // posture enabled AND the user store is empty
        kioskEnabled: false,   // kiosk PIN elevation is configured (offer PIN entry)

        // Resolved identity (from /api/auth/me)
        id: '',
        provider: '',
        groups: [],
        entitlements: [],

        // Login / setup form + UI state
        showLogin: false,
        busy: false,
        error: '',

        // Convenience for markup gating.
        get isEnabled() { return this.posture === 'enabled'; },

        // True when an anonymous visitor is browsing under the Guest scope:
        // identity system on, no session, past first-run setup, and the app has
        // resolved far enough to run (the Guest scope granted at least
        // equipment.view — otherwise `check()` shows the login wall instead).
        // Drives the "viewing as guest" affordances and lets the login overlay
        // be dismissed back to the guest dashboard.
        get isGuest() {
            return this.posture === 'enabled' && !this.authenticated
                && !this.setupRequired && this.ready;
        },

        // The login overlay may be closed only when doing so leaves the visitor
        // somewhere usable — i.e. the guest dashboard. A hard login wall
        // (nothing viewable anonymously) and first-run setup are not dismissible.
        get canDismissLogin() { return this.isGuest; },

        /**
         * Mirror the server's entitlement-selector semantics so the UI can gate
         * affordances from one source of truth (docs/auth-redesign.md §4,
         * entitlement.cpp / policy_engine.h):
         *   1. posture disabled            -> permit all (root-anonymous)
         *   2. holds `system.admin`        -> permit all (superuser)
         *   3. holds an entitlement whose action matches exactly AND whose
         *      selector matches: selector-less matches only a resource-less
         *      request; '*' matches any id; otherwise an exact id match.
         * The server remains the actual enforcement point; this only hides or
         * disables controls the caller could not use anyway.
         */
        can(action, resourceId) {
            if (this.posture !== 'enabled') { return true; }
            const rid = resourceId || '';
            for (const raw of this.entitlements) {
                const colon = raw.indexOf(':');
                const act = colon === -1 ? raw : raw.slice(0, colon);
                const sel = colon === -1 ? null : raw.slice(colon + 1);
                if (act === 'system.admin' && sel === null) { return true; }
                if (act !== action) { continue; }
                if (sel === null) { if (rid === '') { return true; } continue; }
                if (sel === '*' || sel === rid) { return true; }
            }
            return false;
        },

        _applyMe(me) {
            this.posture = me.posture === 'enabled' ? 'enabled' : 'disabled';
            this.authenticated = !!me.authenticated;
            this.setupRequired = !!me.setup_required;
            this.kioskEnabled = !!me.kiosk_enabled;
            this.id = me.id || '';
            this.provider = me.provider || '';
            this.groups = Array.isArray(me.groups) ? me.groups : [];
            this.entitlements = Array.isArray(me.entitlements) ? me.entitlements : [];
        },

        // Probe who-am-I and decide what to show.  Returns true when the app may
        // start running (posture disabled, or posture enabled + authenticated).
        async check() {
            this.busy = true;
            try {
                const resp = await window.fetch('/api/auth/me');
                if (!resp.ok) {
                    // /me is open under the identity system; a non-200 here means
                    // the legacy shared-token model is bearer-gating it, or the
                    // server is unreachable — surface the login screen.
                    this.showLogin = true;
                    this.ready = false;
                    return false;
                }
                const me = await resp.json();
                this._applyMe(me);

                if (this.posture !== 'enabled') {
                    // Identity system off: run exactly as an auth-free build.
                    this.showLogin = false;
                    this.ready = true;
                    window.dispatchEvent(new CustomEvent('auth:ready'));
                    return true;
                }

                if (this.authenticated) {
                    this.showLogin = false;
                    this.ready = true;
                    window.dispatchEvent(new CustomEvent('auth:ready'));
                    return true;
                }

                // Enabled but not authenticated. First-run setup is a hard gate:
                // no anonymous browsing until an administrator exists.
                if (this.setupRequired) {
                    this.showLogin = true;
                    this.ready = false;
                    return false;
                }

                // Guest mode (docs/auth-redesign.md D3): an anonymous visitor
                // resolves to the built-in Guest group. Guest browsing "turns on"
                // exactly when the admin has granted the Guest scope at least read
                // access — /api/auth/me already carries the Guest entitlements, so
                // can('equipment.view') is the switch. When granted, boot the app
                // as a guest (the header "Sign in" affordance elevates to a full
                // session); otherwise fall back to the login wall so an empty
                // Guest scope behaves exactly like "login required".
                if (this.can('equipment.view')) {
                    this.showLogin = false;
                    this.ready = true;
                    window.dispatchEvent(new CustomEvent('auth:ready'));
                    return true;
                }

                this.showLogin = true;
                this.ready = false;
                return false;
            } catch (_) {
                this.showLogin = true;
                this.ready = false;
                return false;
            } finally {
                this.busy = false;
            }
        },

        // First-run: create the initial administrator, then log straight in.
        async setup(username, password) {
            this.error = '';
            this.busy = true;
            try {
                const resp = await window.fetch('/api/auth/setup', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ username, password }),
                });
                if (resp.status === 201) {
                    // Setup succeeded -> authenticate with the same credentials.
                    return await this.login(username, password, true);
                }
                if (resp.status === 403) {
                    // Someone else completed setup first — fall back to login.
                    this.setupRequired = false;
                    this.error = window.AquaI18n.t('auth.error_setup_done');
                    return false;
                }
                this.error = window.AquaI18n.t('auth.error_setup_failed');
                return false;
            } catch (_) {
                this.error = window.AquaI18n.t('auth.error_network');
                return false;
            } finally {
                this.busy = false;
            }
        },

        async login(username, password, remember) {
            this.error = '';
            this.busy = true;
            try {
                const resp = await window.fetch('/api/auth/login', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ username, password }),
                });

                if (resp.status === 200) {
                    const data = await resp.json();
                    window.AqualinkAuth.setTokens(data.access_token, data.refresh_token, !!remember);
                    const ok = await this.check();
                    if (!ok) {
                        // Should not happen (we just authenticated) but stay safe.
                        window.AqualinkAuth.clearTokens();
                        this.error = window.AquaI18n.t('auth.error_signin_failed');
                    }
                    return ok;
                }

                if (resp.status === 429) {
                    const retry = resp.headers.get('Retry-After');
                    const secs = retry ? parseInt(retry, 10) : 0;
                    this.error = secs
                        ? Alpine.store('i18n').tn('auth.error_rate_limited', secs)
                        : window.AquaI18n.t('auth.error_rate_limited_later');
                    return false;
                }

                // 401 (and any other failure) is one indistinguishable message.
                this.error = window.AquaI18n.t('auth.error_bad_credentials');
                return false;
            } catch (_) {
                this.error = window.AquaI18n.t('auth.error_network');
                return false;
            } finally {
                this.busy = false;
            }
        },

        // Kiosk PIN elevation: exchange a PIN for a session in the admin-
        // configured target group. A kiosk is a shared terminal, so the session
        // is deliberately NOT "remembered" (refresh token dies with the tab).
        async loginWithPin(pin) {
            this.error = '';
            this.busy = true;
            try {
                const resp = await window.fetch('/api/auth/pin', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ pin }),
                });

                if (resp.status === 200) {
                    const data = await resp.json();
                    window.AqualinkAuth.setTokens(data.access_token, data.refresh_token, false);
                    const ok = await this.check();
                    if (!ok) {
                        window.AqualinkAuth.clearTokens();
                        this.error = window.AquaI18n.t('auth.error_signin_failed');
                    }
                    return ok;
                }

                if (resp.status === 429) {
                    const retry = resp.headers.get('Retry-After');
                    const secs = retry ? parseInt(retry, 10) : 0;
                    this.error = secs
                        ? Alpine.store('i18n').tn('auth.error_rate_limited', secs)
                        : window.AquaI18n.t('auth.error_rate_limited_later');
                    return false;
                }

                this.error = window.AquaI18n.t('auth.error_bad_pin');
                return false;
            } catch (_) {
                this.error = window.AquaI18n.t('auth.error_network');
                return false;
            } finally {
                this.busy = false;
            }
        },

        // Sign out THIS session: best-effort revoke server-side, then drop tokens
        // and return to the login card.
        async logout() {
            const refresh = window.AqualinkAuth.refreshToken();
            try {
                if (refresh) {
                    await window.fetch('/api/auth/logout', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ refresh_token: refresh }),
                    });
                }
            } catch (_) { /* revoke is best-effort; we drop tokens regardless */ }
            this._localSignOut();
        },

        // Sign out EVERYWHERE: revoke every session for this user (requires the
        // access token, which the fetch wrapper attaches).
        async logoutEverywhere() {
            const refresh = window.AqualinkAuth.refreshToken();
            try {
                await window.fetch('/api/auth/logout', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ refresh_token: refresh, everywhere: true }),
                });
            } catch (_) { /* best-effort */ }
            this._localSignOut();
        },

        // Local teardown shared by both sign-out paths and by auth:unauthorized.
        // After dropping the session we re-resolve identity: under guest mode a
        // signed-out user lands on the Guest dashboard (if the Guest scope grants
        // read access) rather than a dead end. check() re-fetches /api/auth/me
        // anonymously and sets ready / showLogin accordingly; if it resolves to
        // guest browsing we reconnect the equipment socket (now anonymous).
        _localSignOut() {
            window.AqualinkAuth.clearTokens();
            this.authenticated = false;
            this.ready = false;
            this.showLogin = true;
            this.id = '';
            this.groups = [];
            this.entitlements = [];
            try {
                Alpine.store('ws').disconnectEquipment();
                Alpine.store('ws').disconnectStats();
            } catch (_) { /* ws store may not be ready */ }

            this.check().then((ok) => {
                if (ok && !this.authenticated) {
                    // Dropped to guest browsing — bring the live socket back up.
                    try { Alpine.store('ws').connectEquipment(); } catch (_) { /* ws not ready */ }
                }
            });
        },
    });

    // The fetch wrapper fires this after a refresh attempt failed: the session is
    // dead. Re-resolve identity — under guest mode this drops to the Guest
    // dashboard when the Guest scope allows it, else surfaces the login wall.
    window.addEventListener('auth:unauthorized', () => {
        const store = Alpine.store('auth');
        if (!store) return;
        // Only meaningful under the identity system; a disabled posture never
        // 401s a normal request.
        store._localSignOut();
    });
});
