/**
 * Administration — Kiosk PIN tab (Slice 5, guest mode / D16).
 *
 * Kiosk PIN elevation lets a shared wall tablet quick-elevate the anonymous
 * Guest into an admin-chosen group by typing a short PIN. This tab is the
 * system.admin surface over /api/kiosk:
 *
 *   GET    -> { enabled, target_group }   (the PIN itself is write-only)
 *   PUT    { pin, target_group }          (set / replace the PIN)
 *   DELETE                                (disable + clear)
 *
 * The target group is chosen from the existing groups (GET /api/groups); any
 * group works, but the Guest group is the natural pairing — the same scope an
 * un-elevated visitor already has, extended with whatever the kiosk group adds.
 */
function adminKioskView() {
    return {
        loaded: false,
        loading: false,
        saving: false,
        error: '',
        okFlash: false,

        // Current server state.
        enabled: false,
        targetGroup: '',

        // Form state.
        pin: '',
        confirmPin: '',
        formGroup: '',
        groups: [],

        minPin: 4,

        // Load once when the tab is first shown.
        ensureLoaded() {
            if (this.loaded) { return; }
            this.loaded = true;
            this.refresh();
            this.loadGroups();
        },

        async refresh() {
            this.loading = true;
            this.error = '';
            try {
                const resp = await fetch('/api/kiosk');
                if (!resp.ok) { this.error = `Could not load kiosk status (${resp.status}).`; return; }
                const data = await resp.json();
                this.enabled = !!data.enabled;
                this.targetGroup = data.target_group || '';
                if (!this.formGroup) { this.formGroup = this.targetGroup; }
            } catch (_) {
                this.error = 'Network error loading kiosk status.';
            } finally {
                this.loading = false;
            }
        },

        async loadGroups() {
            try {
                const resp = await fetch('/api/groups');
                if (!resp.ok) { return; }
                const data = await resp.json();
                const list = Array.isArray(data) ? data : (Array.isArray(data.groups) ? data.groups : []);
                this.groups = list.map((g) => g.name).filter(Boolean);
                // Default the picker to Guest when nothing is set yet.
                if (!this.formGroup) {
                    this.formGroup = this.groups.includes('Guest') ? 'Guest' : (this.groups[0] || '');
                }
            } catch (_) { /* leave the picker empty; save will validate */ }
        },

        get pinValid() {
            return this.pin.length >= this.minPin && this.pin === this.confirmPin && this.formGroup.length > 0;
        },

        async save() {
            if (!this.pinValid || this.saving) { return; }
            this.saving = true;
            this.error = '';
            try {
                const resp = await fetch('/api/kiosk', {
                    method: 'PUT',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ pin: this.pin, target_group: this.formGroup }),
                });
                if (!resp.ok) {
                    let detail = '';
                    try { detail = (await resp.json()).error || ''; } catch (_) { /* ignore */ }
                    this.error = detail || `Could not save the PIN (${resp.status}).`;
                    return;
                }
                this.pin = '';
                this.confirmPin = '';
                this.okFlash = true;
                setTimeout(() => { this.okFlash = false; }, 1500);
                await this.refresh();
            } catch (_) {
                this.error = 'Network error saving the PIN.';
            } finally {
                this.saving = false;
            }
        },

        async disable() {
            if (this.saving) { return; }
            this.saving = true;
            this.error = '';
            try {
                const resp = await fetch('/api/kiosk', { method: 'DELETE' });
                if (!resp.ok) { this.error = `Could not disable the kiosk PIN (${resp.status}).`; return; }
                this.pin = '';
                this.confirmPin = '';
                await this.refresh();
            } catch (_) {
                this.error = 'Network error disabling the kiosk PIN.';
            } finally {
                this.saving = false;
            }
        },
    };
}
