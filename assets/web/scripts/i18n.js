/**
 * I18n runtime — locale resolution, catalog registry, and translation lookup.
 *
 * Design (docs/i18n-scoping.md, Phase 0):
 *   - Catalogs are plain JS registration files under /i18n/<locale>.js that
 *     assign a flat key→string map into `window.AquaI18n.catalogs.<locale>`.
 *     English (/i18n/en.js) is included statically in index.html so the
 *     default/fallback catalog is always available synchronously at first
 *     render — no fetch race, no flash of raw keys, works offline.
 *   - Non-English catalogs load on demand via <script> injection; bindings
 *     re-render reactively when the catalog arrives or the locale changes.
 *   - Lookup order: active locale → English fallback → the key itself (with a
 *     one-time console warning, so missed extractions are visible in dev).
 *   - Locale choice: explicit user choice (localStorage, and mirrored to the
 *     server under preferences.ui.locale for cross-device sync) → server
 *     preference → browser language → 'en'.  <html lang>/<html dir> follow
 *     the active locale; RTL locales flip `dir` (none registered yet).
 *
 * Load order: this file MUST be referenced from index.html AFTER /i18n/en.js
 * and BEFORE every store/component/view script (they may call AquaI18n.t at
 * render time via Alpine bindings).
 */
(function (global) {
    'use strict';

    const STORAGE_KEY = 'locale';

    // Locale registry. Adding a language = one catalog file (/i18n/<code>.js)
    // + one entry here. `name` is the language's own name (endonym) — it is
    // deliberately NOT translated, so a user stuck in the wrong language can
    // always find their own. `dir` drives <html dir> ('ltr' | 'rtl').
    // `numberLocale` (optional) pins the Intl formatting locale when the bare
    // code's browser default is wrong or inconsistent: bare 'ar' resolves to
    // Latin digits in some engines, so the Eastern Arabic-Indic numbering
    // system is requested explicitly via the Unicode 'nu' extension.
    const SUPPORTED_LOCALES = [
        { code: 'en', name: 'English', dir: 'ltr' },
        { code: 'de', name: 'Deutsch', dir: 'ltr' },
        { code: 'ar', name: 'العربية', dir: 'rtl', numberLocale: 'ar-u-nu-arab' },
        { code: 'ja', name: '日本語', dir: 'ltr' },
    ];

    const api = (global.AquaI18n = global.AquaI18n || {});
    api.catalogs = api.catalogs || {};
    api.SUPPORTED_LOCALES = SUPPORTED_LOCALES;

    const warnedKeys = new Set();

    function localeInfo(code) {
        return SUPPORTED_LOCALES.find((l) => l.code === code) || null;
    }

    // localStorage holds only EXPLICIT user choices; a browser-derived default
    // is recomputed each boot so a user who never chose follows their browser.
    function savedLocale() {
        try {
            const saved = localStorage.getItem(STORAGE_KEY);
            return saved && localeInfo(saved) ? saved : null;
        } catch (_) {
            return null;
        }
    }

    function browserLocale() {
        const candidates = (navigator.languages && navigator.languages.length)
            ? navigator.languages
            : [navigator.language || 'en'];
        for (const cand of candidates) {
            const c = String(cand).toLowerCase();
            if (localeInfo(c)) return c;
            const base = c.split('-')[0];
            if (localeInfo(base)) return base;
        }
        return 'en';
    }

    function applyDocumentAttrs(code) {
        const info = localeInfo(code);
        document.documentElement.lang = code;
        document.documentElement.dir = (info && info.dir) === 'rtl' ? 'rtl' : 'ltr';
    }

    // Number-typed placeholder values render through Intl.NumberFormat so the
    // digits themselves localize (generic Arabic uses Eastern Arabic-Indic
    // numerals; German gets its grouping separators). Pre-formatted strings
    // (e.g. toFixed() output) pass through untouched.
    const numberFormats = {};
    function formatNumber(value, locale) {
        try {
            const info = localeInfo(locale);
            const nfLocale = (info && info.numberLocale) || locale;
            return (numberFormats[nfLocale] || (numberFormats[nfLocale] = new Intl.NumberFormat(nfLocale))).format(value);
        } catch (_) {
            return String(value);
        }
    }

    function formatMessage(msg, params, locale) {
        return msg.replace(/\{(\w+)\}/g, (whole, name) => {
            const v = params ? params[name] : undefined;
            if (v === undefined || v === null) return whole;
            return (typeof v === 'number') ? formatNumber(v, locale) : String(v);
        });
    }

    // Plain (non-reactive) lookup used before Alpine exists and as the shared
    // implementation behind the store's reactive t().
    function lookup(locale, key, params) {
        const active = api.catalogs[locale] || {};
        const fallback = api.catalogs.en || {};
        let msg = Object.prototype.hasOwnProperty.call(active, key) ? active[key]
            : Object.prototype.hasOwnProperty.call(fallback, key) ? fallback[key]
                : undefined;
        if (msg === undefined) {
            if (!warnedKeys.has(key)) {
                warnedKeys.add(key);
                console.warn(`[i18n] missing catalog key: ${key}`);
            }
            return key;
        }
        return params ? formatMessage(msg, params, locale) : msg;
    }

    // Inject a catalog <script>; resolves once it has registered (or failed —
    // the English fallback then covers every key).
    function ensureCatalog(code) {
        return new Promise((resolve) => {
            if (api.catalogs[code]) { resolve(true); return; }
            const el = document.createElement('script');
            el.src = `/i18n/${code}.js`;
            el.onload = () => resolve(!!api.catalogs[code]);
            el.onerror = () => {
                console.warn(`[i18n] failed to load catalog for '${code}'; falling back to English`);
                resolve(false);
            };
            document.head.appendChild(el);
        });
    }

    // Convenience bridge for plain-function call sites (helpers that have no
    // $store handle). Delegates to the Alpine store when available so the
    // caller's binding still picks up locale/catalog reactivity.
    api.t = function (key, params) {
        if (global.Alpine && Alpine.store('i18n')) return Alpine.store('i18n').t(key, params);
        return lookup(savedLocale() || browserLocale(), key, params);
    };

    // Apply lang/dir as early as possible (pre-Alpine) so fonts/UA behaviour
    // match the resolved locale from the first paint.
    const initialLocale = savedLocale() || browserLocale();
    applyDocumentAttrs(initialLocale);

    document.addEventListener('alpine:init', () => {
        Alpine.store('i18n', {
            locale: initialLocale,
            // Monotonic counter bumped when a catalog finishes loading; t()
            // reads it so bindings re-evaluate (catalogs live outside Alpine's
            // reactive graph).
            revision: 0,

            locales: SUPPORTED_LOCALES,

            init() {
                ensureCatalog(this.locale).then(() => { this.revision++; });
                // Adopt the server-side preference (cross-device sync). The
                // preferences API may 401 before login, so retry once the auth
                // gate opens.
                this._syncFromServer();
                window.addEventListener('auth:ready', () => this._syncFromServer(), { once: true });
            },

            t(key, params) {
                this.revision; // reactive dependency (see comment above)
                return lookup(this.locale, key, params);
            },

            // Plural-aware lookup: catalog holds `<key>.one`, `<key>.other`,
            // etc. (CLDR categories); {count} is always available as a param.
            tn(key, count, params) {
                let category = 'other';
                try { category = new Intl.PluralRules(this.locale).select(count); } catch (_) { /* keep 'other' */ }
                const merged = Object.assign({ count }, params);
                this.revision;
                const active = api.catalogs[this.locale] || {};
                const fallback = api.catalogs.en || {};
                const exact = `${key}.${category}`;
                const chosen = (exact in active || exact in fallback) ? exact : `${key}.other`;
                return lookup(this.locale, chosen, merged);
            },

            // Explicit user choice: persist locally (instant boot) and mirror
            // to the server (cross-device), then swap reactively.
            async setLocale(code) {
                if (!localeInfo(code) || code === this.locale) return;
                try { localStorage.setItem(STORAGE_KEY, code); } catch (_) { /* private mode */ }
                await this._apply(code);
                try {
                    await fetch('/api/preferences', {
                        method: 'PUT',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ ui: { locale: code } }),
                    });
                } catch (_) { /* offline: local choice still applies */ }
            },

            async _apply(code) {
                await ensureCatalog(code);
                this.locale = code;
                this.revision++;
                applyDocumentAttrs(code);
            },

            async _syncFromServer() {
                try {
                    const resp = await fetch('/api/preferences');
                    if (!resp.ok) return;
                    const p = await resp.json();
                    const server = p && p.ui && p.ui.locale;
                    if (server && localeInfo(server) && server !== this.locale) {
                        // Server is the cross-device source of truth; keep the
                        // local mirror in step so the next boot starts right.
                        try { localStorage.setItem(STORAGE_KEY, server); } catch (_) { /* ignore */ }
                        await this._apply(server);
                    }
                } catch (_) { /* offline / auth-gated: keep local resolution */ }
            },
        });

        Alpine.store('i18n').init();

        // `$t('key', {params})` sugar for templates.
        Alpine.magic('t', () => (key, params) => Alpine.store('i18n').t(key, params));
        Alpine.magic('tn', () => (key, count, params) => Alpine.store('i18n').tn(key, count, params));
    });
})(typeof window !== 'undefined' ? window : globalThis);
