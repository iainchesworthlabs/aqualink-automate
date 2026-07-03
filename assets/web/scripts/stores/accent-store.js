/**
 * Accent Store — accent-colour selection (teal / azure / aqua / violet).
 *
 * Mirrors theme-store: a client preference persisted to localStorage as the
 * first-paint cache and surfaced on the root element as `data-accent`, which
 * drives the `:root[data-accent="..."]` token overrides in app.css. The
 * default (teal) needs no attribute rule — it is the base `--accent`.
 *
 * For a logged-in user the choice is also stored per-user server-side via
 * AqualinkPrefs (see prefs-sync.js): hydrate() applies a server value without
 * writing it back, set() pushes the change.
 */
document.addEventListener('alpine:init', () => {
    Alpine.store('accent', {
        name: 'teal',
        options: ['teal', 'azure', 'aqua', 'violet'],

        init() {
            const saved = localStorage.getItem('accent');
            if (saved && this.options.includes(saved)) {
                this.name = saved;
            }
        },

        set(name) {
            if (!this.options.includes(name)) return;
            this.name = name;
            localStorage.setItem('accent', name);
            // Persist per-user when logged in (no-op otherwise).
            window.AqualinkPrefs?.push({ accent: name });
        },

        // Apply a server-provided value WITHOUT pushing it back.
        hydrate(name) {
            if (!this.options.includes(name)) return;
            this.name = name;
            localStorage.setItem('accent', name);
        }
    });
});
