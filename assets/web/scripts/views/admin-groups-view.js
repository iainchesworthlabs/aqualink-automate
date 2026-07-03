/**
 * Admin — Groups tab (Slice 2 Wave B).
 *
 * Lists groups (name, built-in badge, entitlement count) and edits a group's
 * entitlements with the shared entitlement-editor component. Against /api/groups:
 *   - create / edit  (POST /api/groups {name, entitlements[]}) — upsert
 *   - delete         (DELETE /api/groups/{name}) — built-in groups return 409
 *
 * Built-in groups (Everyone / Guest / Administrators) are editable but NOT
 * deletable — the delete control is hidden for them. Editing the **Guest** group
 * is the guest-scope editor: Guest is the deny-by-default identity applied to a
 * not-logged-in visitor, so we label its editor prominently so an admin
 * understands they are granting anonymous access.
 *
 * Server 400 responses (invalid entitlement) are surfaced verbatim.
 */
function adminGroupsView() {
    return {
        groups: [],
        loading: false,
        listError: '',

        // Inline panel: null | 'create' | { edit: name }
        panel: null,

        // Create form.
        cName: '',
        cError: '',
        cBusy: false,

        // Edit form.
        eName: '',
        eBuiltIn: false,
        eError: '',
        eBusy: false,

        // Delete confirm.
        dTarget: null,
        dError: '',
        dBusy: false,

        // Lazily load once the tab is first shown (avoids a pre-auth 401).
        _loaded: false,
        ensureLoaded() {
            if (this._loaded) { return; }
            this._loaded = true;
            this.refresh();
        },

        async refresh() {
            this.loading = true;
            this.listError = '';
            try {
                const resp = await fetch('/api/groups');
                if (!resp.ok) {
                    this.listError = window.AquaI18n.t('admin.error_load_groups', { status: resp.status });
                    this.groups = [];
                    return;
                }
                const list = await resp.json();
                this.groups = Array.isArray(list) ? list : [];
            } catch (_) {
                this.listError = window.AquaI18n.t('admin.error_network_groups');
                this.groups = [];
            } finally {
                this.loading = false;
            }
        },

        describeEnt(raw) { return window.AqualinkEntitlements.describe(raw); },

        // Guest is the anonymous-visitor identity; flag it so the edit panel can
        // explain the deny-by-default guest scope.
        isGuest(name) { return String(name).toLowerCase() === 'guest'; },

        // ---- Create ----
        openCreate() {
            this.cName = '';
            this.cError = '';
            this.panel = 'create';
        },

        async submitCreate(entitlements) {
            this.cError = '';
            const name = this.cName.trim();
            if (!name) { this.cError = window.AquaI18n.t('admin.error_group_name_required'); return; }
            this.cBusy = true;
            try {
                const resp = await fetch('/api/groups', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ name, entitlements: entitlements || [] }),
                });
                if (resp.ok) {
                    this.panel = null;
                    await this.refresh();
                    return;
                }
                this.cError = await this._errorText(resp);
            } catch (_) {
                this.cError = window.AquaI18n.t('admin.error_network');
            } finally {
                this.cBusy = false;
            }
        },

        // ---- Edit (entitlements) ----
        openEdit(group) {
            this.eName = group.name;
            this.eBuiltIn = !!group.built_in;
            this.eError = '';
            this.panel = { edit: group.name };
        },

        isEditing(name) { return this.panel && this.panel.edit === name; },

        // POST upserts by name, so editing a group's entitlements is the same call
        // as create with the existing name.
        async submitEdit(entitlements) {
            this.eError = '';
            this.eBusy = true;
            try {
                const resp = await fetch('/api/groups', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ name: this.eName, entitlements: entitlements || [] }),
                });
                if (resp.ok) {
                    this.panel = null;
                    await this.refresh();
                    return;
                }
                this.eError = await this._errorText(resp);
            } catch (_) {
                this.eError = window.AquaI18n.t('admin.error_network');
            } finally {
                this.eBusy = false;
            }
        },

        // ---- Delete ----
        openDelete(group) {
            this.dTarget = group;
            this.dError = '';
            this.panel = { del: group.name };
        },

        isDeleting(name) { return this.panel && this.panel.del === name; },

        async confirmDelete() {
            if (!this.dTarget) { return; }
            this.dBusy = true;
            this.dError = '';
            try {
                const resp = await fetch(`/api/groups/${encodeURIComponent(this.dTarget.name)}`, { method: 'DELETE' });
                if (resp.status === 204) {
                    this.panel = null;
                    this.dTarget = null;
                    await this.refresh();
                    return;
                }
                this.dError = await this._errorText(resp);
            } catch (_) {
                this.dError = window.AquaI18n.t('admin.error_network');
            } finally {
                this.dBusy = false;
            }
        },

        closePanel() { this.panel = null; },

        async _errorText(resp) {
            try {
                const data = await resp.json();
                if (data && data.error) { return data.error; }
            } catch (_) { /* not JSON */ }
            return window.AquaI18n.t('admin.error_request_failed', { status: resp.status });
        },
    };
}
