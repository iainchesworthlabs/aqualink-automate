/**
 * Entitlement editor (Slice 2 Wave B) — a shared Alpine component that lets an
 * admin compose the array of entitlement strings that the users / groups /
 * api-keys APIs accept.
 *
 * The assignable vocabulary comes from GET /api/entitlements ({ actions: [...] }).
 * Each entry is a bare action (e.g. `equipment.view`, `schedules.edit`,
 * `system.admin`). The selector grammar — "action[:selector]" — is a purely
 * client-side concern (docs/auth-redesign.md §4): only `equipment.control.aux`
 * is offered a per-resource selector because it is the one action the server
 * scopes to an individual device. Every other action is a plain checkbox.
 *
 * For the aux action we surface three modes:
 *   - off       -> no `equipment.control.aux` entitlement emitted;
 *   - all       -> emits `equipment.control.aux:*` (every aux device);
 *   - specific  -> emits one `equipment.control.aux:<device-id>` per ticked
 *                  device, sourced from GET /api/equipment/buttons.
 *
 * Usage (parent owns the value array):
 *   x-data="entitlementEditor()"
 *   x-init="init($store... , initialEntitlementsArray)"
 *   ...then read editor.compose() to get the string[] to POST/PUT.
 *
 * The component is intentionally self-contained: it fetches its own vocabulary
 * and device list once, and reconciles an incoming entitlement array into its
 * checkbox / aux-mode state so it round-trips cleanly for the edit flows.
 */

// The one action whose selector we expose in the UI. Kept as a constant so the
// aux-specific branches read clearly.
const ENTITLEMENT_AUX_ACTION = 'equipment.control.aux';

function entitlementEditor() {
    return {
        // Full action vocabulary from the server (bare action strings).
        actions: [],
        // Aux device choices: [{ id, label }] from GET /api/equipment/buttons.
        auxDevices: [],
        loading: false,
        loadError: '',

        // Ticked plain actions (a Set-like map: action -> true). system.admin and
        // equipment.control.aux are handled specially and NOT stored here.
        selected: {},

        // Aux selector mode: 'off' | 'all' | 'specific'.
        auxMode: 'off',
        // When auxMode === 'specific': device-id -> true.
        auxSelected: {},

        // system.admin is a superuser grant; give it its own prominent toggle and
        // reflect that it subsumes everything else.
        admin: false,

        // Load the vocabulary + aux devices, then hydrate from an initial array.
        // Safe to call repeatedly; it only re-hydrates state.
        async init(initial) {
            await this._loadVocabulary();
            await this._loadAuxDevices();
            this.hydrate(Array.isArray(initial) ? initial : []);
        },

        async _loadVocabulary() {
            this.loading = true;
            this.loadError = '';
            try {
                const resp = await fetch('/api/entitlements');
                if (!resp.ok) {
                    this.loadError = `Could not load the entitlement vocabulary (${resp.status}).`;
                    this.actions = [];
                    return;
                }
                const data = await resp.json();
                this.actions = Array.isArray(data.actions) ? data.actions : [];
            } catch (_) {
                this.loadError = 'Network error loading entitlements.';
                this.actions = [];
            } finally {
                this.loading = false;
            }
        },

        async _loadAuxDevices() {
            try {
                const resp = await fetch('/api/equipment/buttons');
                if (!resp.ok) { this.auxDevices = []; return; }
                const data = await resp.json();
                const list = Array.isArray(data) ? data : (Array.isArray(data.buttons) ? data.buttons : []);
                this.auxDevices = list.map((b) => ({ id: b.id, label: b.display_label || b.label || b.id }));
            } catch (_) {
                this.auxDevices = [];
            }
        },

        // The plain (checkbox) actions: everything except system.admin and the
        // aux action, which have dedicated controls.
        plainActions() {
            return this.actions.filter((a) => a !== 'system.admin' && a !== ENTITLEMENT_AUX_ACTION);
        },

        hasAuxAction() {
            return this.actions.includes(ENTITLEMENT_AUX_ACTION);
        },

        hasAdminAction() {
            return this.actions.includes('system.admin');
        },

        // Split a "action[:selector]" string.
        _parse(raw) {
            const colon = raw.indexOf(':');
            return colon === -1
                ? { action: raw, selector: null }
                : { action: raw.slice(0, colon), selector: raw.slice(colon + 1) };
        },

        // Reconcile an incoming entitlement array into checkbox / aux state.
        hydrate(entitlements) {
            this.selected = {};
            this.auxSelected = {};
            this.auxMode = 'off';
            this.admin = false;

            for (const raw of entitlements) {
                const { action, selector } = this._parse(raw);
                if (action === 'system.admin') { this.admin = true; continue; }
                if (action === ENTITLEMENT_AUX_ACTION) {
                    if (selector === '*' || selector === null) {
                        this.auxMode = 'all';
                    } else {
                        // A specific device id; only switch to 'all' if we saw '*'.
                        if (this.auxMode !== 'all') { this.auxMode = 'specific'; }
                        this.auxSelected[selector] = true;
                    }
                    continue;
                }
                this.selected[action] = true;
            }
        },

        toggleAction(action) {
            this.selected[action] = !this.selected[action];
        },

        setAuxMode(mode) {
            this.auxMode = mode;
            if (mode !== 'specific') { this.auxSelected = {}; }
        },

        toggleAuxDevice(id) {
            this.auxSelected[id] = !this.auxSelected[id];
        },

        // Number of specific aux devices ticked (for the summary line).
        auxSpecificCount() {
            return Object.values(this.auxSelected).filter(Boolean).length;
        },

        // Emit the composed entitlement string[] for the API. When system.admin is
        // granted it is a superuser grant, so we emit ONLY that (the server treats
        // it as permit-all and the other rows would be redundant / misleading).
        compose() {
            if (this.admin) { return ['system.admin']; }

            const out = [];
            for (const action of this.plainActions()) {
                if (this.selected[action]) { out.push(action); }
            }
            if (this.hasAuxAction()) {
                if (this.auxMode === 'all') {
                    out.push(`${ENTITLEMENT_AUX_ACTION}:*`);
                } else if (this.auxMode === 'specific') {
                    for (const dev of this.auxDevices) {
                        if (this.auxSelected[dev.id]) { out.push(`${ENTITLEMENT_AUX_ACTION}:${dev.id}`); }
                    }
                }
            }
            return out;
        },

        // A short human label for an entitlement string, used by the read-only
        // chips the list views render.
        describe(raw) {
            const { action, selector } = this._parse(raw);
            if (selector === null) { return action; }
            if (selector === '*') { return `${action} (all)`; }
            const dev = this.auxDevices.find((d) => d.id === selector);
            return dev ? `${action} (${dev.label})` : `${action} (${selector})`;
        },
    };
}

// A lightweight, dependency-free formatter for entitlement chips in list views
// that don't instantiate the full editor. Exposed on window so any view can use
// it without wiring a component.
window.AqualinkEntitlements = {
    // "equipment.control.aux:*" -> "equipment.control.aux (all)"; a bare id keeps
    // the id (the list views annotate aux ids further where they have the device
    // list). Never throws.
    describe(raw) {
        if (typeof raw !== 'string') { return String(raw); }
        const colon = raw.indexOf(':');
        if (colon === -1) { return raw; }
        const action = raw.slice(0, colon);
        const selector = raw.slice(colon + 1);
        if (selector === '*') { return `${action} (all)`; }
        return `${action} (${selector})`;
    },
};
