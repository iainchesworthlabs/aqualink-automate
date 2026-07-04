/**
 * Layout Store — responsive viewport + shell state (single source of truth).
 *
 * The CSS does the visual reflow via viewport media queries; this store gives
 * the JS/Alpine side the SAME breakpoint truth (kept in sync with matchMedia)
 * plus the small amount of imperative shell state that CSS can't own on its
 * own: whether the tablet nav drawer is open, and which full-screen sheet /
 * modal is active on small screens.
 *
 * Canonical breakpoints (must match styles/app.css):
 *   phone   < 640px
 *   tablet  640–1023px
 *   desktop >= 1024px
 *   + landscape orientation (used, with tablet, for the iPad dashboard layout)
 *
 * Later phases consume this: Phase 1 (bottom tab bar / hamburger drawer / More
 * sheet), Phase 4 (modal sheets), Phase 5 (tablet-landscape dashboard).
 */
document.addEventListener('alpine:init', () => {
    Alpine.store('layout', {
        // --- reactive viewport state (synced from matchMedia in init) ---
        isPhone: false,
        isTablet: false,
        isDesktop: true,
        isLandscape: false,

        // --- shell state ---
        mobileNavOpen: false,   // hamburger drawer (tablet only)
        // which sheet/modal is showing on small screens; null = none.
        // 'more' | 'login' | 'account' | 'admin' | 'alerts' | 'schedule' | 'device'
        activeSheet: null,

        init() {
            const mqs = {
                phone: window.matchMedia('(max-width: 639px)'),
                tablet: window.matchMedia('(min-width: 640px) and (max-width: 1023px)'),
                desktop: window.matchMedia('(min-width: 1024px)'),
                landscape: window.matchMedia('(orientation: landscape)')
            };

            const sync = () => {
                this.isPhone = mqs.phone.matches;
                this.isTablet = mqs.tablet.matches;
                this.isDesktop = mqs.desktop.matches;
                this.isLandscape = mqs.landscape.matches;
                // The hamburger drawer only exists on tablet — never let it stay
                // stranded open when the viewport grows to desktop or shrinks to phone.
                if (!this.isTablet && this.mobileNavOpen) this.mobileNavOpen = false;
                // The "More" sheet is a phone-only surface (reached from the bottom
                // tab bar); close it if the viewport leaves phone width.
                if (!this.isPhone && this.activeSheet === 'more') this.activeSheet = null;
            };

            Object.values(mqs).forEach(mq => mq.addEventListener('change', sync));
            sync();
        },

        // The iPad dashboard gets a dedicated orientation layout (Phase 5).
        get isTabletLandscape() { return this.isTablet && this.isLandscape; },

        // --- hamburger drawer ---
        toggleMobileNav() { this.mobileNavOpen = !this.mobileNavOpen; },
        closeMobileNav() { this.mobileNavOpen = false; },

        // --- sheets / modals ---
        get sheetOpen() { return this.activeSheet !== null; },
        openSheet(name) { this.activeSheet = name; this.mobileNavOpen = false; },
        closeSheet() { this.activeSheet = null; }
    });
});
