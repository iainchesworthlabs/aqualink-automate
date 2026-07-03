/**
 * Admin — Users tab (Slice 2 Wave B).
 *
 * Lists users (username, group chips, disabled state, tokver) and offers the
 * full lifecycle against /api/users:
 *   - create  (POST /api/users {username, password, groups?, direct_entitlements?})
 *   - edit    (PUT  /api/users/{id} {groups?, direct_entitlements?, disabled?})
 *   - delete  (DELETE /api/users/{id})       — confirm dialog
 *   - reset password (PUT /api/users/{id}/password {password}, min 12)
 *
 * Group membership is a multi-select from GET /api/groups; direct entitlements
 * are composed with the shared entitlement-editor component nested inside the
 * create / edit panels.
 *
 * Server errors are surfaced VERBATIM (the API returns `{ "error": "..." }`):
 *   409 last-admin / duplicate username; 400 weak password (<12); etc.
 */
const ADMIN_PASSWORD_MIN = 12;

function adminUsersView() {
    return {
        // Exposed for the "(min {min})" catalog bindings in the panels.
        passwordMin: ADMIN_PASSWORD_MIN,

        users: [],
        groups: [],       // [{name, ...}] for the membership picker
        loading: false,
        listError: '',

        // Which inline panel is open: null | 'create' | { edit: id } | { pw: id } | { del: id }
        panel: null,

        // Create form.
        cForm: { username: '', password: '', confirm: '', groups: {} },
        cError: '',
        cBusy: false,

        // Edit form (bound to the row being edited).
        eForm: { id: '', username: '', groups: {}, disabled: false },
        eError: '',
        eBusy: false,

        // Reset-password form.
        pForm: { id: '', username: '', password: '', confirm: '' },
        pError: '',
        pSaved: false,
        pBusy: false,

        // Delete confirm.
        dTarget: null,
        dError: '',
        dBusy: false,

        // Lazily load once the tab is first shown (avoids firing /api calls before
        // the user has authenticated, which would 401). Idempotent.
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
                const [uResp, gResp] = await Promise.all([
                    fetch('/api/users'),
                    fetch('/api/groups'),
                ]);
                if (!uResp.ok) {
                    this.listError = window.AquaI18n.t('admin.error_load_users', { status: uResp.status });
                    this.users = [];
                } else {
                    const list = await uResp.json();
                    this.users = Array.isArray(list) ? list : [];
                }
                if (gResp.ok) {
                    const gl = await gResp.json();
                    this.groups = Array.isArray(gl) ? gl : [];
                }
            } catch (_) {
                this.listError = window.AquaI18n.t('admin.error_network_users');
                this.users = [];
            } finally {
                this.loading = false;
            }
        },

        // ---- Chip helper ----
        describeEnt(raw) { return window.AqualinkEntitlements.describe(raw); },

        // ---- Create ----
        openCreate() {
            this.cForm = { username: '', password: '', confirm: '', groups: {} };
            this.cError = '';
            this.panel = 'create';
            // The nested editor hydrates itself from [] on its own x-init.
        },

        _groupsArray(map) {
            return Object.keys(map).filter((k) => map[k]);
        },

        async submitCreate(entitlements) {
            this.cError = '';
            const username = this.cForm.username.trim();
            if (!username) { this.cError = window.AquaI18n.t('admin.error_username_required'); return; }
            if (this.cForm.password.length < ADMIN_PASSWORD_MIN) {
                this.cError = window.AquaI18n.t('admin.password_too_short', { min: ADMIN_PASSWORD_MIN });
                return;
            }
            if (this.cForm.password !== this.cForm.confirm) {
                this.cError = window.AquaI18n.t('admin.passwords_no_match');
                return;
            }
            this.cBusy = true;
            try {
                const resp = await fetch('/api/users', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({
                        username,
                        password: this.cForm.password,
                        groups: this._groupsArray(this.cForm.groups),
                        direct_entitlements: entitlements || [],
                    }),
                });
                if (resp.status === 201) {
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

        // ---- Edit ----
        openEdit(user) {
            const groups = {};
            (user.groups || []).forEach((g) => { groups[g] = true; });
            this.eForm = {
                id: user.id,
                username: user.username,
                groups,
                disabled: !!user.disabled,
            };
            this.eError = '';
            this.panel = { edit: user.id };
        },

        isEditing(id) { return this.panel && this.panel.edit === id; },

        async submitEdit(entitlements) {
            this.eError = '';
            this.eBusy = true;
            try {
                const resp = await fetch(`/api/users/${encodeURIComponent(this.eForm.id)}`, {
                    method: 'PUT',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({
                        groups: this._groupsArray(this.eForm.groups),
                        direct_entitlements: entitlements || [],
                        disabled: !!this.eForm.disabled,
                    }),
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

        // ---- Reset password ----
        openPassword(user) {
            this.pForm = { id: user.id, username: user.username, password: '', confirm: '' };
            this.pError = '';
            this.pSaved = false;
            this.panel = { pw: user.id };
        },

        isResetting(id) { return this.panel && this.panel.pw === id; },

        async submitPassword() {
            this.pError = '';
            this.pSaved = false;
            if (this.pForm.password.length < ADMIN_PASSWORD_MIN) {
                this.pError = window.AquaI18n.t('admin.password_too_short', { min: ADMIN_PASSWORD_MIN });
                return;
            }
            if (this.pForm.password !== this.pForm.confirm) {
                this.pError = window.AquaI18n.t('admin.passwords_no_match');
                return;
            }
            this.pBusy = true;
            try {
                const resp = await fetch(`/api/users/${encodeURIComponent(this.pForm.id)}/password`, {
                    method: 'PUT',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ password: this.pForm.password }),
                });
                if (resp.status === 204) {
                    this.pSaved = true;
                    this.pForm.password = '';
                    this.pForm.confirm = '';
                    setTimeout(() => { if (this.panel && this.panel.pw) { this.panel = null; } }, 1400);
                    return;
                }
                this.pError = await this._errorText(resp);
            } catch (_) {
                this.pError = window.AquaI18n.t('admin.error_network');
            } finally {
                this.pBusy = false;
            }
        },

        // ---- Delete ----
        openDelete(user) {
            this.dTarget = user;
            this.dError = '';
            this.panel = { del: user.id };
        },

        isDeleting(id) { return this.panel && this.panel.del === id; },

        async confirmDelete() {
            if (!this.dTarget) { return; }
            this.dBusy = true;
            this.dError = '';
            try {
                const resp = await fetch(`/api/users/${encodeURIComponent(this.dTarget.id)}`, { method: 'DELETE' });
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

        // Pull the server's verbatim `{ error }` message; fall back to the status.
        async _errorText(resp) {
            try {
                const data = await resp.json();
                if (data && data.error) { return data.error; }
            } catch (_) { /* not JSON */ }
            return window.AquaI18n.t('admin.error_request_failed', { status: resp.status });
        },
    };
}
