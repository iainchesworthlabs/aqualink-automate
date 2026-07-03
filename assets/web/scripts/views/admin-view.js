/**
 * Administration overlay shell (Slice 2 Wave B).
 *
 * A full-screen overlay (mirrors the .account-overlay pattern) reachable only
 * when $store.auth.can('system.admin'). It owns nothing but the open/close state
 * and the active tab; each tab hosts a self-contained sub-view component
 * (admin-users-view.js / admin-groups-view.js / admin-apikeys-view.js) plus a
 * read-only Entitlements reference tab built inline.
 *
 * Opened by dispatching `admin:open` on window (the nav button does this),
 * closed on Escape or the close button. Tabs are keyboard-navigable buttons with
 * an ARIA tablist so the whole surface is operable without a mouse.
 */
function adminView() {
    return {
        open: false,
        tab: 'users',   // 'users' | 'groups' | 'entitlements' | 'apikeys' | 'kiosk'

        // Tab captions resolve through the catalog at render time (x-text="$t(t.labelKey)").
        tabs: [
            { id: 'users', labelKey: 'admin.users' },
            { id: 'groups', labelKey: 'admin.groups' },
            { id: 'entitlements', labelKey: 'admin.entitlements' },
            { id: 'apikeys', labelKey: 'admin.apikeys' },
            { id: 'kiosk', labelKey: 'admin.kiosk' },
        ],

        // Entitlements reference tab state (read-only vocabulary listing).
        vocabulary: [],
        vocabError: '',
        vocabLoaded: false,

        show() {
            // Defensive: never open for a non-admin even if the event fires.
            if (!this.$store.auth.can('system.admin')) { return; }
            this.open = true;
            if (!this.vocabLoaded) { this.fetchVocabulary(); }
        },

        hide() {
            this.open = false;
        },

        selectTab(id) {
            this.tab = id;
        },

        async fetchVocabulary() {
            this.vocabError = '';
            try {
                const resp = await fetch('/api/entitlements');
                if (!resp.ok) {
                    this.vocabError = window.AquaI18n.t('admin.error_load_entitlements', { status: resp.status });
                    return;
                }
                const data = await resp.json();
                this.vocabulary = Array.isArray(data.actions) ? data.actions : [];
                this.vocabLoaded = true;
            } catch (_) {
                this.vocabError = window.AquaI18n.t('admin.error_network_entitlements');
            }
        },

        // Group the vocabulary by its leading namespace (before the first dot) so
        // the reference tab reads as sections: system / equipment / schedules ...
        vocabularyGroups() {
            const groups = {};
            for (const action of this.vocabulary) {
                const ns = action.indexOf('.') === -1 ? action : action.slice(0, action.indexOf('.'));
                (groups[ns] = groups[ns] || []).push(action);
            }
            return Object.entries(groups).map(([name, actions]) => ({ name, actions }));
        },

        // A one-line description for the reference tab. Purely presentational —
        // resolved through the catalog at render time (locale can change).
        actionHint(action) {
            if (action === 'system.admin') { return window.AquaI18n.t('admin.hint_system_admin'); }
            if (action === 'equipment.control.aux') { return window.AquaI18n.t('admin.hint_aux'); }
            if (action.endsWith('.view')) { return window.AquaI18n.t('admin.hint_view'); }
            if (action.startsWith('equipment.control.')) { return window.AquaI18n.t('admin.hint_control'); }
            if (action.endsWith('.edit')) { return window.AquaI18n.t('admin.hint_edit'); }
            return '';
        },
    };
}
