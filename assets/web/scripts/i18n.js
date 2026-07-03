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
        { code: 'es', name: 'Español', dir: 'ltr' },
        { code: 'fr', name: 'Français', dir: 'ltr' },
        { code: 'ar', name: 'العربية', dir: 'rtl', numberLocale: 'ar-u-nu-arab' },
        { code: 'he', name: 'עברית', dir: 'rtl' },
        { code: 'ja', name: '日本語', dir: 'ltr' },
        { code: 'zh', name: '简体中文', dir: 'ltr' },
        { code: 'yi', name: 'ייִדיש', dir: 'rtl' },
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

    // True when the key exists in the active or English catalog — WITHOUT the
    // missing-key console warning, for optional keys resolved from server data
    // (error codes, alert conditions).
    api.has = function (key) {
        const st = global.Alpine && Alpine.store('i18n');
        const code = st ? st.locale : (savedLocale() || browserLocale());
        return !!((api.catalogs[code] && Object.prototype.hasOwnProperty.call(api.catalogs[code], key))
            || (api.catalogs.en && Object.prototype.hasOwnProperty.call(api.catalogs.en, key)));
    };

    // Translated message for a structured API error body ({error, code,
    // params} — docs/i18n.md): the catalog entry for the code when one exists,
    // else the server's English `error` string, else the fallback.
    api.apiError = function (data, fallback) {
        if (data && data.code && api.has('error.' + data.code)) {
            return api.t('error.' + data.code, data.params);
        }
        return (data && (data.error || data.message)) || fallback;
    };

    // ---- Locale-aware value formatting (docs/i18n.md) ----------------------
    // All helpers resolve the ACTIVE locale through the Alpine store when it
    // exists — reading store.locale registers a reactive dependency, so
    // bindings re-render on a language switch — and honour the registry's
    // numberLocale pin (e.g. Arabic-Indic digits).

    function activeFormatLocale() {
        const code = (global.Alpine && Alpine.store('i18n')) ? Alpine.store('i18n').locale : (savedLocale() || browserLocale());
        const info = localeInfo(code);
        return (info && info.numberLocale) || code;
    }

    const fmtCache = {};
    function cachedFormat(kind, locale, options, factory) {
        const key = kind + '|' + locale + '|' + JSON.stringify(options || {});
        return fmtCache[key] || (fmtCache[key] = factory());
    }

    // Locale-digit number formatting. `options` passes through to
    // Intl.NumberFormat (e.g. {maximumFractionDigits: 1}).
    api.formatNumber = function (value, options) {
        if (value === undefined || value === null || (typeof value === 'number' && !isFinite(value))) return String(value);
        const locale = activeFormatLocale();
        try {
            return cachedFormat('n', locale, options, () => new Intl.NumberFormat(locale, options)).format(value);
        } catch (_) {
            return String(value);
        }
    };

    // Temperature display honouring the server display-units preference
    // (units: 'Celsius' | 'Fahrenheit'). Accepts the wire dual-unit object
    // ({celsius, fahrenheit}), a bare celsius number, or a legacy/unknown
    // value (returned unchanged, e.g. the '--' placeholder).
    api.formatTemperature = function (value, units) {
        let celsius = null, fahrenheit = null;
        if (value && typeof value === 'object') {
            if (value.celsius == null && value.fahrenheit == null) return '--';
            celsius = value.celsius; fahrenheit = value.fahrenheit;
        } else if (typeof value === 'number' && isFinite(value)) {
            celsius = value;
        } else {
            return value == null ? '--' : String(value);
        }
        const useF = (units === 'Fahrenheit');
        let n = useF ? fahrenheit : celsius;
        if (n == null) {
            // Only the other unit arrived: convert rather than showing nothing.
            n = useF ? (celsius * 9 / 5 + 32) : ((fahrenheit - 32) * 5 / 9);
        }
        n = Math.round(n * 10) / 10;
        const locale = activeFormatLocale();
        try {
            // unitDisplay 'short' (not 'narrow'): CLDR's narrow fahrenheit
            // symbol is a bare '°' with no F, which is ambiguous.
            return cachedFormat('t', locale, { u: useF }, () => new Intl.NumberFormat(locale, {
                style: 'unit', unit: useF ? 'fahrenheit' : 'celsius', unitDisplay: 'short', maximumFractionDigits: 1,
            })).format(n);
        } catch (_) {
            return api.formatNumber(n, { maximumFractionDigits: 1 }) + (useF ? '°F' : '°C');
        }
    };

    // Time / date-time display in the active locale (not the browser locale).
    api.formatTime = function (value) {
        const d = (value instanceof Date) ? value : new Date(value);
        if (isNaN(d.getTime())) return String(value);
        const locale = activeFormatLocale();
        try {
            return cachedFormat('tt', locale, null, () => new Intl.DateTimeFormat(locale, { timeStyle: 'medium' })).format(d);
        } catch (_) {
            return d.toLocaleTimeString();
        }
    };

    api.formatDateTime = function (value) {
        const d = (value instanceof Date) ? value : new Date(value);
        if (isNaN(d.getTime())) return String(value);
        const locale = activeFormatLocale();
        try {
            return cachedFormat('dt', locale, null, () => new Intl.DateTimeFormat(locale, { dateStyle: 'short', timeStyle: 'medium' })).format(d);
        } catch (_) {
            return d.toLocaleString();
        }
    };

    // Apply lang/dir as early as possible (pre-Alpine) so fonts/UA behaviour
    // match the resolved locale from the first paint.
    const initialLocale = savedLocale() || browserLocale();
    applyDocumentAttrs(initialLocale);

    // Load the saved non-English catalog SYNCHRONOUSLY during parse so the
    // first paint is already in the user's language. The async injection path
    // would render English until the catalog arrived — and freeze anything
    // fired in that window (e.g. the connection-lost toast) in English.
    // document.write is intentional and safe here: this is a classic script
    // executing DURING parsing, where document.write appends a parser-blocking
    // <script> that runs before the deferred Alpine bundle. If it is
    // unavailable (or the file 404s) the async path in init() still recovers,
    // with English as the interim fallback.
    if (initialLocale !== 'en' && !api.catalogs[initialLocale]) {
        try {
            document.write('<script src="/i18n/' + initialLocale + '.js"><\/script>');
        } catch (_) { /* async fallback in init() */ }
    }

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
                // An explicit choice this session outranks the async server pull
                // (_syncFromServer): without this, the auth:ready-time adoption
                // can race a just-made switch (its PUT still in flight) and
                // snap the UI back to the previous locale.
                this._explicitChoice = true;
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
                // Per-user prefs (the server locale) are prefs.self-gated, but a
                // disabled posture permits everyone — so gate on the auth store's
                // can() (one source of truth): auth-off installs keep cross-device
                // locale sync, while an anonymous/guest visitor under an enabled
                // posture keeps the localStorage locale (D7) and skips the request
                // to avoid first-paint 401 noise. Falls back to the raw token while
                // /api/auth/me is in flight; the auth:ready listener re-runs this.
                const auth = global.Alpine && global.Alpine.store('auth');
                const reachable = (auth && auth.ready)
                    ? auth.can('prefs.self')
                    : !!(global.AqualinkAuth && global.AqualinkAuth.token && global.AqualinkAuth.token());
                if (!reachable) { return; }
                if (this._explicitChoice) { return; } // user already chose this session
                try {
                    const resp = await fetch('/api/preferences');
                    if (!resp.ok) return;
                    const p = await resp.json();
                    // Re-check after the await: a switch made while this request
                    // was in flight must not be stomped by a stale server value.
                    if (this._explicitChoice) { return; }
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
        // Locale-digit numbers: `$n(count)` and fixed-decimals `$nf(value, digits)`.
        Alpine.magic('n', () => (value, options) => api.formatNumber(value, options));
        Alpine.magic('nf', () => (value, digits) => api.formatNumber(value, { minimumFractionDigits: digits, maximumFractionDigits: digits }));
    });
})(typeof window !== 'undefined' ? window : globalThis);
