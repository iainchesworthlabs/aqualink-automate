/**
 * Settings View — user/admin preferences.
 *
 * Two groups:
 *   - Server-backed preferences (units, alert thresholds, webhook URL, history
 *     retention) loaded from and saved to /api/preferences (persisted on the
 *     server, shared across devices, and read live by the backend services).
 *   - Chemistry target ranges: still mirrored to localStorage (so the unchanged
 *     chemistry-gauge component keeps working) AND synced to the server under
 *     preferences.ui.chemistryBands so they survive a cache clear / new device.
 */
function settingsView() {
    const ui = (typeof window !== 'undefined' && window.AquaUI) || {};
    const bandsKey = ui.CHEMISTRY_BANDS_KEY || 'chemistryBands';
    const bandDefaults = (ui.CHEMISTRY_BAND_DEFAULTS) || {
        ph:   { goodMin: 7.4,  goodMax: 7.6,  okayMin: 7.2,  okayMax: 7.8,  badMin: 7.0,  badMax: 8.0 },
        orp:  { goodMin: 700,  goodMax: 750,  okayMin: 650,  okayMax: 700,  badMin: 650,  badMax: 800 },
        salt: { goodMin: 3500, goodMax: 4000, okayMin: 2700, okayMax: 3500, badMin: 2700, badMax: 4500 }
    };

    const stored = JSON.parse(localStorage.getItem(bandsKey) || '{}');
    const values = {};
    for (const [key, def] of Object.entries(bandDefaults)) {
        values[key] = { ...def, ...(stored[key] || {}) };
    }

    function bandsForStorage(vals) {
        const out = {};
        for (const [key, val] of Object.entries(vals)) {
            out[key] = {
                goodMin: val.goodMin, goodMax: val.goodMax,
                okayMin: val.okayMin, okayMax: val.okayMax,
                badMin: val.badMin, badMax: val.badMax
            };
        }
        return out;
    }

    return {
        get gauges() {
            const t = window.AquaI18n.t;
            return [
                { key: 'ph', label: t('settings.gauge_ph'), step: 0.1 },
                { key: 'orp', label: t('settings.gauge_orp'), step: 10 },
                { key: 'salt', label: t('settings.gauge_salt'), step: 100 }
            ];
        },
        values,
        errors: {},

        // Server-backed preferences.
        prefs: {
            temperature_units: 'Celsius',
            salt_low_ppm: 2600,
            comms_timeout_seconds: 60,
            webhook_url: '',
            retention_days: 90,
        },
        prefsError: '',
        savedFlash: false,

        // Friendly display-name overrides keyed by canonical device label.
        labelOverrides: {},

        // When true, device display names are suffixed with the protocol aux id,
        // e.g. "Pool Light (Aux5)". Display-only; the canonical label is unchanged.
        showAuxIdInLabel: false,

        // Full addressable aux-slot space (GET /api/equipment/aux-slots), one row per aux id --
        // detected devices AND undetected slots, merged with the operator's presence overrides.
        // Backs the Device Names card's two tabs (see myAuxSlots()/otherAuxSlotGroupList()).
        auxSlots: [],
        auxTab: 'mine',
        showAllAuxSlots: false,
        expandedAuxGroups: { A: true },
        auxPresenceBusy: {},

        // Matter bridge status (sidecar status + commissioning QR) and Profiling
        // backend control. These are /api/diagnostics/* surfaces the design places
        // under Settings (not Diagnostics). Fetched once on init; profiling state
        // also refreshes on each control action.
        matter: { enabled: false },
        profiling: { enabled: false, running: false, backend: '', available: [] },
        profilingBusy: false,
        _matterQrPayload: null,

        // Alpine auto-calls init() on the component.
        init() {
            // These endpoints (preferences, matter, profiling) require auth
            // under the identity system. Defer the initial load until the
            // session is ready (authorised, or posture disabled) so a first
            // paint before login does not fire 401s. If already ready — e.g.
            // navigating here after login — load immediately.
            const auth = window.Alpine && Alpine.store('auth');
            if (auth && !auth.ready) {
                window.addEventListener('auth:ready', () => this._loadInitial(), { once: true });
            } else {
                this._loadInitial();
            }
        },

        async _loadInitial() {
            const auth = window.Alpine && Alpine.store('auth');
            // Matter + profiling are diagnostics.view surfaces; only fetch them
            // when the subject may see diagnostics (an anonymous guest usually
            // cannot — skip to avoid a pointless 401).
            if (!auth || auth.can('diagnostics.view')) {
                this.fetchMatter();
                this.fetchProfiling();
            }
            // Aux slots are EQUIPMENT_VIEW-gated (same tier as the dashboard buttons), not tied
            // to the per-user preferences session below, so a guest can still see them.
            this.fetchAuxSlots();
            // Per-user server preferences require a session (prefs.self). A guest
            // has none, so keep the localStorage first-paint values (D7) and skip
            // the server round-trip entirely.
            if (auth && !auth.authenticated) { return; }
            try {
                const resp = await fetch('/api/preferences');
                if (!resp.ok) { return; }
                const p = await resp.json();
                this.prefs.temperature_units = p.temperature_units || 'Celsius';
                this.prefs.salt_low_ppm = (p.alert && p.alert.salt_low_ppm) ?? 2600;
                this.prefs.comms_timeout_seconds = (p.alert && p.alert.comms_timeout_seconds) ?? 60;
                this.prefs.webhook_url = (p.alert && p.alert.webhook_url) || '';
                this.prefs.retention_days = (p.history && p.history.retention_days) ?? 90;
                this.labelOverrides = (p.label_overrides && typeof p.label_overrides === 'object') ? p.label_overrides : {};
                this.showAuxIdInLabel = (typeof p.show_aux_id_in_label === 'boolean') ? p.show_aux_id_in_label : false;

                // Server-stored chemistry bands take precedence (cross-device).
                if (p.ui && p.ui.chemistryBands) {
                    for (const [key, def] of Object.entries(bandDefaults)) {
                        this.values[key] = { ...def, ...(p.ui.chemistryBands[key] || {}) };
                    }
                    localStorage.setItem(bandsKey, JSON.stringify(bandsForStorage(this.values)));
                }
            } catch (_) { /* offline / disabled: keep defaults */ }
        },

        // ---- Chemistry bands (localStorage + server) ----
        updateValue(gaugeKey, field, rawValue) {
            const num = parseFloat(rawValue);
            if (isNaN(num)) return;
            this.values[gaugeKey][field] = num;
            if (this._validateGauge(gaugeKey)) {
                this._saveLocal();
                this._syncBandsToServer();
            }
        },

        // "min – max" label for the Good band (the design's Target chip).
        chemGoodLabel(gaugeKey) {
            const v = this.values[gaugeKey];
            return `${v.goodMin} – ${v.goodMax}`;
        },

        // Zone-bar segments (bad / okay / good / okay / bad) sized proportional to
        // the band config across the full [badMin, badMax] display range.
        chemZones(gaugeKey) {
            const v = this.values[gaugeKey];
            const span = (v.badMax - v.badMin) || 1;
            const pct = (a, b) => Math.max(0, ((b - a) / span) * 100);
            return [
                { color: 'var(--bad)',  pct: pct(v.badMin, v.okayMin) },
                { color: 'var(--warn)', pct: pct(v.okayMin, v.goodMin) },
                { color: 'var(--good)', pct: pct(v.goodMin, v.goodMax) },
                { color: 'var(--warn)', pct: pct(v.goodMax, v.okayMax) },
                { color: 'var(--bad)',  pct: pct(v.okayMax, v.badMax) },
            ];
        },

        resetGauge(gaugeKey) {
            this.values[gaugeKey] = { ...bandDefaults[gaugeKey] };
            this.errors[gaugeKey] = '';
            this._saveLocal();
            this._syncBandsToServer();
        },

        _validateGauge(gaugeKey) {
            const v = this.values[gaugeKey];
            let error = '';
            if (v.goodMin > v.goodMax || v.okayMin > v.okayMax || v.badMin > v.badMax) {
                error = window.AquaI18n.t('settings.tier_min_max_error');
            }
            this.errors[gaugeKey] = error;
            return error === '';
        },

        _saveLocal() {
            localStorage.setItem(bandsKey, JSON.stringify(bandsForStorage(this.values)));
        },

        _syncBandsToServer() {
            this._putPrefs({ ui: { chemistryBands: bandsForStorage(this.values) } });
        },

        // ---- Device display names ----
        // The canonical-labelled controllable devices come straight from the store.
        deviceButtons() {
            return (this.$store.pool && this.$store.pool.buttons) ? this.$store.pool.buttons : [];
        },

        // Non-aux devices (pump/heater/chlorinator) keep coming from the live buttons list
        // unchanged; aux rows are replaced by the richer auxSlots source below so an
        // undetected slot can have a row too.
        nonAuxDeviceButtons() {
            return this.deviceButtons().filter((b) => b.device_type !== 'Auxillary');
        },

        // Status badge text/class for a non-aux device row, mirroring the aux rows'
        // "Detected" badge so both halves of the Device Names list read consistently
        // instead of the aux rows having a status chip and these having none.
        deviceStatusLabel(button) {
            const ui = window.AquaUI || {};
            return ui.buttonStatusLabel ? ui.buttonStatusLabel(button.status) : String(button.status || '--');
        },
        deviceStatusBadgeClass(button) {
            const ui = window.AquaUI || {};
            const active = ui.isActiveStatus ? ui.isActiveStatus(button.status) : false;
            return active ? 'badge-good' : 'badge-muted';
        },

        // Method wrapper around the auxPresenceBusy lookup -- Alpine's reactivity for a raw
        // `auxPresenceBusy[slot.aux_id]` bracket-access expression bound directly in a nested
        // x-for template does not reliably re-sync (observed: buttons render permanently
        // disabled from first paint even though the underlying value is falsy). Every other
        // derived value in this file is read through a method for template binding; this is
        // the one place that used a raw expression, and it's the one that was broken.
        isAuxBusy(auxId) {
            return !!this.auxPresenceBusy[auxId];
        },

        async fetchAuxSlots() {
            try {
                const resp = await fetch('/api/equipment/aux-slots');
                if (!resp.ok) return;
                const data = await resp.json();
                this.auxSlots = Array.isArray(data.slots) ? data.slots : [];
            } catch (_) { /* offline / disabled: keep empty */ }
        },

        // "Your devices" tab: detected auxes, plus anything forced Present (even if the bus
        // still hasn't confirmed it).
        myAuxSlots() {
            return this.auxSlots.filter((s) => s.detected || s.presence_override === 'present');
        },

        // "Other aux slots" tab: everything not already on the first tab, bounded to the
        // detected panel model's slots unless the operator opted into seeing all 32.
        otherAuxSlots() {
            return this.auxSlots.filter((s) => !s.detected && s.presence_override !== 'present' && (this.showAllAuxSlots || s.in_model_span));
        },

        otherAuxSlotCount() {
            return this.otherAuxSlots().length;
        },

        // True once the model is known and at least one out-of-span slot exists, which is what
        // the "show all 32 slots" escape hatch is for; hidden otherwise (nothing to show for it).
        hasOutOfSpanAuxSlots() {
            return this.auxSlots.some((s) => !s.detected && s.presence_override !== 'present' && !s.in_model_span);
        },

        // Group the "other" tab by power centre, in a stable A/B/C/D/Extra order.
        otherAuxSlotGroupList() {
            const order = ['A', 'B', 'C', 'D', 'extra'];
            const labels = { A: 'A', B: 'B', C: 'C', D: 'D', extra: window.AquaI18n.t('settings.aux_extra') };
            const groups = {};
            for (const slot of this.otherAuxSlots()) {
                const key = slot.power_centre || 'extra';
                (groups[key] = groups[key] || []).push(slot);
            }
            return order.filter((key) => groups[key] && groups[key].length > 0).map((key) => ({ key, label: labels[key], slots: groups[key] }));
        },

        toggleAuxGroup(key) {
            this.expandedAuxGroups = { ...this.expandedAuxGroups, [key]: !this.expandedAuxGroups[key] };
        },

        // Set (or clear, via 'auto') one aux slot's presence override. Its own immediate-effect
        // action with a real side effect (creates/removes a device) -- deliberately NOT batched
        // into the name fields' Save button.
        async setAuxPresence(auxId, override) {
            if (this.auxPresenceBusy[auxId]) return;
            this.auxPresenceBusy = { ...this.auxPresenceBusy, [auxId]: true };
            try {
                const resp = await fetch(`/api/equipment/aux-slots/${encodeURIComponent(auxId)}`, {
                    method: 'PUT',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ presence_override: override }),
                });
                const data = await resp.json().catch(() => ({}));
                if (resp.ok) {
                    const idx = this.auxSlots.findIndex((s) => s.aux_id === auxId);
                    if (idx >= 0) { this.auxSlots.splice(idx, 1, data); } else { this.auxSlots.push(data); }
                    const key = override === 'present' ? 'toast.aux_forced_present' : (override === 'absent' ? 'toast.aux_forced_absent' : 'toast.aux_presence_reset');
                    Alpine.store('toast').show(window.AquaI18n.t(key, { aux_id: auxId }), 'info');
                    if (this.$store.pool && typeof this.$store.pool._fetchButtons === 'function') {
                        this.$store.pool._fetchButtons();
                    }
                } else {
                    Alpine.store('toast').show(window.AquaI18n.apiError(data, window.AquaI18n.t('toast.aux_presence_failed')), 'error');
                }
            } catch (e) {
                Alpine.store('toast').show(window.AquaI18n.t('toast.aux_presence_failed'), 'error');
            } finally {
                const rest = { ...this.auxPresenceBusy };
                delete rest[auxId];
                this.auxPresenceBusy = rest;
            }
        },

        async saveLabels() {
            // Drop blank entries so an empty field falls back to the canonical label.
            const cleaned = {};
            for (const [canonical, display] of Object.entries(this.labelOverrides)) {
                if (display && String(display).trim() !== '') { cleaned[canonical] = String(display).trim(); }
            }
            this.labelOverrides = cleaned;
            await this._putPrefs({ label_overrides: cleaned });
            // Refresh the dashboard so the new display names take effect immediately.
            if (this.$store.pool && typeof this.$store.pool._fetchButtons === 'function') {
                this.$store.pool._fetchButtons();
            }
        },

        // Toggle "show aux id alongside the friendly name" and refresh the dashboard.
        async saveShowAuxId() {
            await this._putPrefs({ show_aux_id_in_label: !!this.showAuxIdInLabel });
            if (this.$store.pool && typeof this.$store.pool._fetchButtons === 'function') {
                this.$store.pool._fetchButtons();
            }
        },

        // Temperature units apply instantly, like theme/accent/language — no Save
        // button, and no shared "Saved" flash (that badge belongs to the System
        // Preferences / Device Names cards' explicit batch-save actions).
        async setTemperatureUnits(value) {
            this.prefs.temperature_units = value;
            if (this.$store.pool) { this.$store.pool.displayUnits = value; }
            await this._putPrefs({ temperature_units: value }, { flash: false });
        },

        // ---- Server-backed preferences ----
        async saveServerPrefs() {
            await this._putPrefs({
                alert: {
                    salt_low_ppm: Number(this.prefs.salt_low_ppm),
                    comms_timeout_seconds: Number(this.prefs.comms_timeout_seconds),
                    webhook_url: this.prefs.webhook_url || '',
                },
                history: { retention_days: Number(this.prefs.retention_days) },
            });
        },

        async _putPrefs(payload, { flash = true } = {}) {
            this.prefsError = '';
            try {
                const resp = await fetch('/api/preferences', {
                    method: 'PUT',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(payload),
                });
                if (!resp.ok) {
                    // Structured error body ({error, code, params} — docs/i18n.md):
                    // show the translated message for the code when we have one.
                    let detail = '';
                    try {
                        const data = await resp.json();
                        detail = window.AquaI18n.apiError(data, '');
                    } catch (_) { /* non-JSON body */ }
                    this.prefsError = window.AquaI18n.t('settings.save_failed_status', { status: resp.status }) + (detail ? ': ' + detail : '');
                    return;
                }
                if (flash) {
                    this.savedFlash = true;
                    setTimeout(() => { this.savedFlash = false; }, 1500);
                }
            } catch (e) {
                this.prefsError = window.AquaI18n.t('settings.save_failed_network');
            }
        },

        // ---- Matter bridge (status + commissioning QR) ----
        async fetchMatter() {
            try {
                const resp = await fetch('/api/diagnostics/matter');
                if (!resp.ok) return;
                this.matter = await resp.json();
                this.$nextTick(() => this._renderMatterQr());
            } catch (_) { /* offline / disabled: keep defaults */ }
        },

        _renderMatterQr() {
            const payload = this.matter && this.matter.qr_payload;
            const el = this.$refs && this.$refs.matterQr;
            if (!el) return;
            if (!payload || typeof window.QRCode === 'undefined') { el.innerHTML = ''; return; }
            if (this._matterQrPayload === payload && el.childElementCount > 0) return;
            this._matterQrPayload = payload;
            el.innerHTML = '';
            try {
                new window.QRCode(el, { text: payload, width: 200, height: 200, correctLevel: window.QRCode.CorrectLevel.M });
            } catch (_) { /* ignore render failure */ }
        },

        // ---- Profiling backend control ----
        async fetchProfiling() {
            try {
                const resp = await fetch('/api/diagnostics/profiling');
                if (!resp.ok) return;
                this.profiling = await resp.json();
            } catch (_) { /* offline / disabled: keep defaults */ }
        },

        async _postProfiling(payload, okMessage, failMessage) {
            if (this.profilingBusy) return;
            this.profilingBusy = true;
            try {
                const resp = await fetch('/api/diagnostics/profiling', {
                    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload)
                });
                const data = await resp.json().catch(() => ({}));
                if (resp.ok) { this.profiling = data; Alpine.store('toast').show(okMessage, 'info'); }
                else { Alpine.store('toast').show(window.AquaI18n.apiError(data, failMessage), 'error'); }
            } catch (e) { Alpine.store('toast').show(failMessage, 'error'); }
            finally { this.profilingBusy = false; }
        },

        async startProfiling() { await this._postProfiling({ action: 'start' }, window.AquaI18n.t('toast.profiling_resumed'), window.AquaI18n.t('toast.profiling_resume_failed')); },
        async stopProfiling() { await this._postProfiling({ action: 'stop' }, window.AquaI18n.t('toast.profiling_paused'), window.AquaI18n.t('toast.profiling_pause_failed')); },
        async selectProfilingBackend(backend) { if (!backend) return; await this._postProfiling({ action: 'select', backend }, window.AquaI18n.t('toast.profiling_backend_set', { backend }), window.AquaI18n.t('toast.profiling_backend_failed')); },
    };
}
