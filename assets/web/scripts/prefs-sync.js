/**
 * Per-user preference sync (docs/auth-redesign.md §8, D7).
 *
 * The theme/accent stores keep writing localStorage as a FIRST-PAINT cache so
 * the UI renders instantly and works when auth is off (anonymous/guest).  On
 * top of that, when the identity system is on AND the visitor is logged in,
 * their units/theme/accent/chemistry bands live server-side (per user):
 *
 *   - on `auth:ready`, GET /api/preferences and hydrate the stores from the
 *     server (overriding the local first-paint values); and
 *   - store mutations call AqualinkPrefs.push({...}) to persist the changed
 *     per-user field back.
 *
 * Fetches go through the AqualinkAuth fetch wrapper, so the bearer token is
 * attached automatically.  Pushes are no-ops when not authenticated (the
 * localStorage cache already holds the value for the anonymous case).
 */
(function () {
    'use strict';

    function isAuthenticated() {
        return !!(window.AqualinkAuth && window.AqualinkAuth.token && window.AqualinkAuth.token());
    }

    function hydrateStores(prefs) {
        if (!prefs || typeof prefs !== 'object') return;

        const theme = Alpine.store('theme');
        if (theme && typeof theme.hydrate === 'function' && typeof prefs.theme === 'string') {
            theme.hydrate(prefs.theme);
        }

        const accent = Alpine.store('accent');
        if (accent && typeof accent.hydrate === 'function' && typeof prefs.accent === 'string') {
            accent.hydrate(prefs.accent);
        }

        // Units + chemistry bands are consumed by the settings/gauge components,
        // which read /api/preferences themselves; broadcast so they can refresh.
        window.dispatchEvent(new CustomEvent('prefs:hydrated', { detail: prefs }));
    }

    const AqualinkPrefs = {
        // Persist a partial per-user preferences document. No-op (cache only)
        // when not logged in — the store already wrote localStorage.
        push(partial) {
            if (!isAuthenticated() || !partial || typeof partial !== 'object') {
                return Promise.resolve(false);
            }

            return fetch('/api/preferences', {
                method: 'PUT',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(partial),
            })
                .then((res) => res.ok)
                .catch(() => false);
        },

        // Pull the server's per-user view and hydrate the stores.
        pull() {
            if (!isAuthenticated()) return Promise.resolve(null);

            return fetch('/api/preferences')
                .then((res) => (res.ok ? res.json() : null))
                .then((prefs) => {
                    if (prefs) hydrateStores(prefs);
                    return prefs;
                })
                .catch(() => null);
        },
    };

    window.AqualinkPrefs = AqualinkPrefs;

    // Hydrate once the app is authorised (or auth is disabled — then pull() is
    // a no-op and the localStorage first-paint values stand).
    window.addEventListener('auth:ready', () => AqualinkPrefs.pull());
})();
