/**
 * Admin — API keys tab (Slice 2 Wave B).
 *
 * Lists keys (label, entitlement chips, expiry, last-used, revoked state). The
 * list NEVER contains a secret/digest. Against /api/apikeys:
 *   - create (POST /api/apikeys {label, entitlements[], expiry_unix?}) -> 201
 *            { ..., secret, warning } — the secret is shown ONCE in a modal that
 *            must be dismissed, with a copy button.
 *   - revoke (DELETE /api/apikeys/{id}) — confirm dialog.
 *
 * Entitlements are composed with the shared entitlement-editor. Expiry is an
 * optional date -> unix seconds; blank means "never expires".
 *
 * NB: the server returns `expiry_unix` / `last_used_unix` (0 == unset).
 */
function adminApikeysView() {
    return {
        keys: [],
        loading: false,
        listError: '',

        // Inline create panel.
        panel: null,          // null | 'create'
        cLabel: '',
        cExpiry: '',          // <input type="date"> value ('' = never)
        cError: '',
        cBusy: false,

        // One-time secret modal shown after a successful create.
        secret: null,         // { label, secret, warning }
        copied: false,

        // Revoke confirm.
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
                const resp = await fetch('/api/apikeys');
                if (!resp.ok) {
                    this.listError = window.AquaI18n.t('admin.error_load_apikeys', { status: resp.status });
                    this.keys = [];
                    return;
                }
                const list = await resp.json();
                this.keys = Array.isArray(list) ? list : [];
            } catch (_) {
                this.listError = window.AquaI18n.t('admin.error_network_apikeys');
                this.keys = [];
            } finally {
                this.loading = false;
            }
        },

        describeEnt(raw) { return window.AqualinkEntitlements.describe(raw); },

        // ---- Display helpers ----
        expiryLabel(unix) {
            if (!unix) { return window.AquaI18n.t('admin.never'); }
            try { return window.AquaI18n.formatDateTime(unix * 1000); } catch (_) { return String(unix); }
        },

        lastUsedLabel(unix) {
            if (!unix) { return window.AquaI18n.t('admin.never_used'); }
            try { return window.AquaI18n.formatDateTime(unix * 1000); } catch (_) { return String(unix); }
        },

        // ---- Create ----
        openCreate() {
            this.cLabel = '';
            this.cExpiry = '';
            this.cError = '';
            this.panel = 'create';
        },

        async submitCreate(entitlements) {
            this.cError = '';
            const label = this.cLabel.trim();
            if (!label) { this.cError = window.AquaI18n.t('admin.error_label_required'); return; }

            const payload = { label, entitlements: entitlements || [] };
            if (this.cExpiry) {
                // <input type="date"> gives YYYY-MM-DD (local midnight). Convert to
                // unix seconds; guard against an unparseable value.
                const ms = Date.parse(this.cExpiry + 'T00:00:00');
                if (!Number.isNaN(ms)) { payload.expiry_unix = Math.floor(ms / 1000); }
            }

            this.cBusy = true;
            try {
                const resp = await fetch('/api/apikeys', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(payload),
                });
                if (resp.status === 201) {
                    const data = await resp.json();
                    this.panel = null;
                    // Show the one-time secret; it is never retrievable again.
                    this.secret = {
                        label: data.label || label,
                        secret: data.secret || '',
                        // Server-sent warning is shown verbatim; the fallback is ours.
                        warning: data.warning || window.AquaI18n.t('admin.secret_warning'),
                    };
                    this.copied = false;
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

        // ---- One-time secret modal ----
        async copySecret() {
            if (!this.secret || !this.secret.secret) { return; }
            try {
                await navigator.clipboard.writeText(this.secret.secret);
                this.copied = true;
                setTimeout(() => { this.copied = false; }, 1800);
            } catch (_) {
                // Clipboard blocked (insecure context / permissions): leave the
                // secret visible so the admin can select it manually.
                this.copied = false;
            }
        },

        dismissSecret() {
            this.secret = null;
            this.copied = false;
        },

        // ---- Revoke ----
        openRevoke(key) {
            this.dTarget = key;
            this.dError = '';
            this.panel = { del: key.id };
        },

        isRevoking(id) { return this.panel && this.panel.del === id; },

        async confirmRevoke() {
            if (!this.dTarget) { return; }
            this.dBusy = true;
            this.dError = '';
            try {
                const resp = await fetch(`/api/apikeys/${encodeURIComponent(this.dTarget.id)}`, { method: 'DELETE' });
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
