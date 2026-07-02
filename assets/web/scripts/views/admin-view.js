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
        tab: 'users',   // 'users' | 'groups' | 'entitlements' | 'apikeys'

        tabs: [
            { id: 'users', label: 'Users' },
            { id: 'groups', label: 'Groups' },
            { id: 'entitlements', label: 'Entitlements' },
            { id: 'apikeys', label: 'API keys' },
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
                    this.vocabError = `Could not load entitlements (${resp.status}).`;
                    return;
                }
                const data = await resp.json();
                this.vocabulary = Array.isArray(data.actions) ? data.actions : [];
                this.vocabLoaded = true;
            } catch (_) {
                this.vocabError = 'Network error loading entitlements.';
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

        // A one-line description for the reference tab. Purely presentational.
        actionHint(action) {
            const hints = {
                'system.admin': 'Full administrator — implies every other action.',
                'equipment.control.aux': 'Toggle auxiliary devices. Supports a per-device selector.',
            };
            if (hints[action]) { return hints[action]; }
            if (action.endsWith('.view')) { return 'Read-only visibility of this area.'; }
            if (action.startsWith('equipment.control.')) { return 'Command this equipment category.'; }
            if (action.endsWith('.edit')) { return 'Create / modify entries in this area.'; }
            return '';
        },
    };
}
