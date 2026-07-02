/**
 * Theme Store — Dark/light mode management.
 *
 * localStorage is the FIRST-PAINT cache (instant render, and the whole story
 * when auth is off).  For a logged-in user the choice is also persisted
 * per-user server-side via AqualinkPrefs (see prefs-sync.js): hydrate() applies
 * the server value without writing it back, toggle() pushes the change.
 */
document.addEventListener('alpine:init', () => {
    Alpine.store('theme', {
        isDark: false,

        init() {
            const saved = localStorage.getItem('theme');
            if (saved) {
                this.isDark = saved === 'dark';
            } else {
                this.isDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
            }

            window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', (e) => {
                if (!localStorage.getItem('theme')) {
                    this.isDark = e.matches;
                }
            });
        },

        toggle() {
            this.isDark = !this.isDark;
            const mode = this.isDark ? 'dark' : 'light';
            localStorage.setItem('theme', mode);
            // Persist per-user when logged in (no-op otherwise).
            window.AqualinkPrefs?.push({ theme: mode });
        },

        // Apply a server-provided value ('light' | 'dark' | 'system') WITHOUT
        // pushing it back. 'system' clears the explicit choice and follows the
        // OS setting, matching the no-localStorage path in init().
        hydrate(mode) {
            if (mode === 'system') {
                localStorage.removeItem('theme');
                this.isDark = window.matchMedia('(prefers-color-scheme: dark)').matches;
                return;
            }

            if (mode === 'dark' || mode === 'light') {
                this.isDark = mode === 'dark';
                localStorage.setItem('theme', mode);
            }
        }
    });
});
