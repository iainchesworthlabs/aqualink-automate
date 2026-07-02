/**
 * Pool Store — Equipment state (temperatures, chemistry, buttons, system status)
 *
 * REST API field mapping:
 *   /api/equipment          → temperatures, chemistry, buttons, devices, stats, version
 *                             chemistry = { salt_ppm, orp_mv, ph,
 *                                           chlorinator: { generating_percent, duty_cycle,
 *                                                          pool_setpoint_percent, spa_setpoint_percent,
 *                                                          setpoint_percent, status, health } }
 *   /api/equipment/buttons  → { buttons: [{ id, label, status }] }
 *   /api/equipment/version  → { fields: [{ label, value }], model_number, fw_revision }
 *   /api/version            → { software_version: { name, version, description, homepage }, git_info: { ... } }
 *
 * WebSocket payload mapping:
 *   TemperatureUpdate → { pool_temp, spa_temp, air_temp }  ({celsius, fahrenheit} objects, same shape as REST)
 *   ChemistryUpdate   → { ph, orp, salt_level }            (numeric values)
 *   SystemStatusChange → serialised IStatus
 *   ButtonStateChange  → { button_id, status, label? }
 */

// How long a terminal command state ('applied'/'rejected'/'blocked'/'timedout')
// stays visible before it auto-clears, and how long an in-flight 'sending'
// command waits before it is considered timed out.
const COMMAND_STATE_CLEAR_MS = 3000;
const COMMAND_SENDING_TIMEOUT_MS = 5000;
// After the server accepts a button command (HTTP 200), the equipment has NOT
// changed yet — the command still has to traverse the RS-485 bus, the controller
// has to actuate the relay, and the new state has to be decoded back and pushed as
// a 'ButtonStateChange'. We hold the command 'sending' until that authoritative
// confirmation arrives, or until this (longer) window elapses and we surface an
// honest 'timedout'. Sized for the controller round-trip (the RSSA status-poll
// rotation / a OneTouch menu re-scrape); tune if confirmations routinely arrive
// later than this on real hardware.
const COMMAND_CONFIRM_TIMEOUT_MS = 20000;

// Re-fetch interval when the buttons endpoint reports it is not yet ready.
const BUTTONS_RETRY_MS = 5000;

// Circulation (spa-mode) confirmation. Unlike buttons/heaters there is no
// WebSocket event for a body becoming active, so the only authoritative signal
// is configuration.bodies[].is_active from /api/equipment. After a circulation
// command we re-poll that endpoint until the requested mode is reflected (or the
// command's COMMAND_CONFIRM_TIMEOUT_MS elapses and it honestly resolves to
// 'timedout'). The poll cadence × attempts is sized to span the confirm window.
const CIRCULATION_CONFIRM_POLL_MS = 2500;
const CIRCULATION_CONFIRM_MAX_ATTEMPTS = 7;

// Normalise the several system-status vocabularies (REST 'operational',
// WS SystemStatusChange status_type like 'Normal'/'Initializing', and the
// initial 'unknown') down to one canonical token. The token is compared
// against in templates (e.g. `systemStatus === 'Operational'`); the
// translated display text comes from the systemStatusLabel getter.
function _normaliseSystemStatus(raw) {
    switch (String(raw || '').toLowerCase()) {
        case 'operational':
        case 'normal':
            return 'Operational';
        case 'initializing':
        case 'starting':
            return 'Starting';
        case 'service':
        case 'service_mode':
            return 'Service Mode';
        case '':
        case 'unknown':
            return 'Unknown';
        default:
            return String(raw);
    }
}

document.addEventListener('alpine:init', () => {
    Alpine.store('pool', {
        // Temperatures hold the RAW wire value ({celsius, fahrenheit} object,
        // or legacy string); the poolTemp/spaTemp/airTemp getters below format
        // for display at read time, so a locale or display-units change
        // re-renders every consumer reactively.
        _poolTempRaw: null,
        _spaTempRaw: null,
        _airTempRaw: null,

        // Server display-units preference ('Celsius' | 'Fahrenheit'), loaded
        // with the initial data and updated when Settings saves it.
        displayUnits: 'Celsius',

        get poolTemp() { return this._displayTemp(this._poolTempRaw); },
        get spaTemp() { return this._displayTemp(this._spaTempRaw); },
        get airTemp() { return this._displayTemp(this._airTempRaw); },

        _displayTemp(raw) {
            if (raw == null) return '--';
            return window.AquaI18n.formatTemperature(raw, this.displayUnits);
        },

        // Setpoints hold the raw wire value too; these getters are the single
        // display path for the ~8 call sites that previously rebuilt
        // "x.x °C" strings inline.
        get poolSetpointDisplay() { return typeof this.poolSetpoint === 'object' ? this._displayTemp(this.poolSetpoint) : this.poolSetpoint; },
        get spaSetpointDisplay() { return typeof this.spaSetpoint === 'object' ? this._displayTemp(this.spaSetpoint) : this.spaSetpoint; },

        poolSetpoint: '--',
        poolSetpoint2: '--',   // POOLSP2 / panel "TEMP2" maintenance setpoint (single-body systems)
        poolHeater2Enabled: null,   // POOLHT2 / panel "TEMP2" maintenance heating enabled (read-only)
        spaSetpoint: '--',
        ph: '--',
        orp: '--',
        saltPpm: '--',

        // Chlorinator (SWG) state — sourced from the nested chemistry.chlorinator
        // block of /api/equipment. Null/unknown until a chlorinator is discovered.
        chlorinatorPresent: false,
        swgGeneratingPercent: '--',
        swgDutyCycle: '--',
        // Configured output setpoint(s) — distinct from the instantaneous generating %.
        // swgSetpointPercent is the headline target for the active body; pool/spa are the
        // per-body configured values. A setpoint of 0 is valid (treated as known, not '--').
        swgSetpointPercent: '--',
        swgPoolSetpoint: '--',
        swgSpaSetpoint: '--',
        chlorinatorStatus: '--',
        chlorinatorHealth: '--',

        // Translated display text for the chlorinator health enum name; an
        // unknown value falls back to the raw name with underscores spaced.
        get chlorinatorHealthLabel() {
            const h = this.chlorinatorHealth;
            if (!h || h === '--') return '';
            const key = window.AquaUI.swgHealthKey(h);
            return key ? window.AquaI18n.t(key) : String(h).replace(/_/g, ' ');
        },

        // Per-value timestamps for freshness tracking
        _timestamps: {},
        _stalenessThreshold: 60,  // seconds

        buttons: [],
        // Bodies of water from /api/equipment configuration.bodies[]
        // ({ id: 'Pool'|'Spa'|..., label, is_active, ... }). The authoritative
        // source for circulation/spa-mode state — Spa body active == spa mode.
        bodies: [],
        commandStates: {},   // { [buttonId|'setpoint:pool'|'setpoint:spa'|'circulation']: { state, timer } }
        systemStatus: 'Unknown',

        // Translated display text for the canonical systemStatus token; falls
        // back to the raw token for values with no catalog entry.
        get systemStatusLabel() {
            const key = {
                'Operational': 'status.operational',
                'Starting': 'status.starting',
                'Service Mode': 'status.service_mode',
                'Unknown': 'common.unknown',
            }[this.systemStatus];
            return key ? window.AquaI18n.t(key) : this.systemStatus;
        },
        equipmentVersion: [],
        softwareVersion: '--',
        gitCommit: '',
        gitDate: '',
        gitUncommitted: false,
        projectName: '',
        projectDescription: '',
        projectHomepage: '',
        lastUpdate: null,
        buttonsLoading: true,

        // ---- Body-of-water / circulation state -------------------------
        _bodyIds() {
            return this.bodies.map(b => String(b.id || '').toLowerCase());
        },

        // A dual-body system has both a Pool and a Spa body. Only then is a
        // circulation (pool<->spa) mode toggle meaningful.
        get hasDualBody() {
            const ids = this._bodyIds();
            return ids.includes('pool') && ids.includes('spa');
        },

        // Spa mode is active when the Spa body is the active body of water.
        get spaModeActive() {
            const spa = this.bodies.find(b => String(b.id || '').toLowerCase() === 'spa');
            return !!(spa && spa.is_active);
        },

        async fetchInitial() {
            await Promise.all([
                this._fetchEquipment(),
                this._fetchButtons(),
                this._fetchVersion(),
                this._fetchDisplayUnits()
            ]);
        },

        // Fetch the display-units preference (best-effort: 401/offline keeps
        // the Celsius default; Settings updates it live on save).
        async _fetchDisplayUnits() {
            try {
                const resp = await fetch('/api/preferences');
                if (!resp.ok) return;
                const p = await resp.json();
                if (p.temperature_units === 'Celsius' || p.temperature_units === 'Fahrenheit') {
                    this.displayUnits = p.temperature_units;
                }
            } catch (_) { /* keep default */ }
        },

        _touch(field) {
            this._timestamps[field] = Date.now();
        },

        age(field) {
            const ts = this._timestamps[field];
            if (!ts) return null;
            return Math.floor((Date.now() - ts) / 1000);
        },

        isStale(field) {
            const a = this.age(field);
            return a !== null && a > this._stalenessThreshold;
        },

        async _fetchEquipment() {
            try {
                const resp = await fetch('/api/equipment');
                const data = await resp.json();
                if (!data || Object.keys(data).length === 0) return;

                // Temperatures — store the raw wire values; display formatting
                // happens in the poolTemp/spaTemp/airTemp getters.
                if (data.temperatures) {
                    this._poolTempRaw = data.temperatures.pool ?? this._poolTempRaw;
                    this._spaTempRaw = data.temperatures.spa ?? this._spaTempRaw;
                    this._airTempRaw = data.temperatures.air ?? this._airTempRaw;
                    if (data.temperatures.pool_setpoint) this.poolSetpoint = data.temperatures.pool_setpoint;
                    if (data.temperatures.pool_setpoint_2) this.poolSetpoint2 = data.temperatures.pool_setpoint_2;
                    if (data.temperatures.pool_heater_2_enabled != null) this.poolHeater2Enabled = data.temperatures.pool_heater_2_enabled;
                    if (data.temperatures.spa_setpoint) this.spaSetpoint = data.temperatures.spa_setpoint;
                }

                // Chemistry — nested structure: { salt_ppm, orp_mv, ph,
                // chlorinator: { generating_percent, duty_cycle, status, health } }.
                // ORP/pH are null when no sensor is present (placeholders today);
                // a value of 0 is also treated as unknown. The chlorinator block
                // is null until a SWG is discovered on the wire.
                if (data.chemistry) {
                    if (data.chemistry.ph != null && data.chemistry.ph !== '' && data.chemistry.ph !== 0) this.ph = data.chemistry.ph;
                    if (data.chemistry.orp_mv != null && data.chemistry.orp_mv !== '' && data.chemistry.orp_mv !== 0) this.orp = data.chemistry.orp_mv;
                    if (data.chemistry.salt_ppm != null && data.chemistry.salt_ppm !== '' && data.chemistry.salt_ppm !== 0) this.saltPpm = data.chemistry.salt_ppm;

                    const swg = data.chemistry.chlorinator;
                    if (swg != null) {
                        this.chlorinatorPresent = true;
                        this.swgGeneratingPercent = (swg.generating_percent != null) ? swg.generating_percent : '--';
                        this.swgDutyCycle = (swg.duty_cycle != null) ? swg.duty_cycle : '--';
                        // Setpoints may legitimately be 0, so test against null only.
                        this.swgSetpointPercent = (swg.setpoint_percent != null) ? swg.setpoint_percent : '--';
                        this.swgPoolSetpoint = (swg.pool_setpoint_percent != null) ? swg.pool_setpoint_percent : '--';
                        this.swgSpaSetpoint = (swg.spa_setpoint_percent != null) ? swg.spa_setpoint_percent : '--';
                        this.chlorinatorStatus = swg.status || '--';
                        this.chlorinatorHealth = swg.health || '--';
                    } else {
                        this.chlorinatorPresent = false;
                    }
                }

                // Equipment version — use generic fields array if available
                if (data.version) {
                    if (Array.isArray(data.version.fields) && data.version.fields.length > 0) {
                        this.equipmentVersion = data.version.fields.filter(f => f.value);
                    } else {
                        // Back-compat: build fields from flat keys
                        const fields = [];
                        if (data.version.model_number) fields.push({ label: window.AquaI18n.t('about.model'), value: data.version.model_number });
                        if (data.version.fw_revision) fields.push({ label: window.AquaI18n.t('about.revision'), value: data.version.fw_revision });
                        this.equipmentVersion = fields;
                    }
                }

                // Seed diagnostics stats from initial load
                if (data.stats) {
                    Alpine.store('stats').handleEvent({
                        type: 'StatisticsUpdate',
                        payload: data.stats
                    });
                }

                this.systemStatus = _normaliseSystemStatus('operational');

                // Bodies of water — authoritative circulation/spa-mode state.
                if (data.configuration && Array.isArray(data.configuration.bodies)) {
                    this.bodies = data.configuration.bodies;
                }

                // Update system store if pool configuration is known
                if (data.configuration && data.configuration.pool_configuration &&
                    data.configuration.pool_configuration !== 'Unknown') {
                    const sys = Alpine.store('system');
                    sys.markReadyIfStarting();
                    sys.poolConfiguration = data.configuration.pool_configuration;
                }
            } catch (e) {
                console.error('Failed to fetch equipment:', e);
                Alpine.store('toast').show(window.AquaI18n.t('toast.load_equipment_failed'), 'error');
            }
        },

        async _fetchButtons() {
            this.buttonsLoading = true;
            try {
                const resp = await fetch('/api/equipment/buttons');
                const data = await resp.json();
                if (data && Array.isArray(data.buttons)) {
                    this.buttons = data.buttons;
                } else if (data && data.buttons === null) {
                    setTimeout(() => this._fetchButtons(), BUTTONS_RETRY_MS);
                    return;
                }
            } catch (e) {
                console.error('Failed to fetch buttons:', e);
                Alpine.store('toast').show(window.AquaI18n.t('toast.load_controls_failed'), 'error');
            }
            this.buttonsLoading = false;
        },

        async _fetchVersion() {
            try {
                const resp = await fetch('/api/version');
                const data = await resp.json();
                if (data.software_version) {
                    this.softwareVersion = data.software_version.version || '--';
                    this.projectName = data.software_version.name || '';
                    this.projectDescription = data.software_version.description || '';
                    this.projectHomepage = data.software_version.homepage || '';
                }
                if (data.git_info) {
                    this.gitCommit = data.git_info.commit_sha1 || '';
                    this.gitDate = data.git_info.commit_date || '';
                    this.gitUncommitted = data.git_info.uncommitted_changes || false;
                }
                // Forward server timing to system store
                const sys = Alpine.store('system');
                if (data.server_start_time) sys.serverStartTime = data.server_start_time;
                if (data.uptime_seconds != null) sys.uptimeSeconds = data.uptime_seconds;
            } catch (e) {
                console.error('Failed to fetch version:', e);
                Alpine.store('toast').show(window.AquaI18n.t('toast.load_version_failed'), 'warn');
            }
        },

        handleEvent(msg) {
            if (!msg || !msg.type) return;
            this.lastUpdate = new Date();

            switch (msg.type) {
                case 'TemperatureUpdate':
                    if (msg.payload) {
                        if (msg.payload.pool_temp != null) { this._poolTempRaw = msg.payload.pool_temp; this._touch('poolTemp'); }
                        if (msg.payload.spa_temp != null) { this._spaTempRaw = msg.payload.spa_temp; this._touch('spaTemp'); }
                        if (msg.payload.air_temp != null) { this._airTempRaw = msg.payload.air_temp; this._touch('airTemp'); }
                        if (msg.payload.pool_setpoint != null) this.poolSetpoint = msg.payload.pool_setpoint;
                        if (msg.payload.pool_setpoint_2 != null) this.poolSetpoint2 = msg.payload.pool_setpoint_2;
                        if (msg.payload.pool_heater_2_enabled != null) this.poolHeater2Enabled = msg.payload.pool_heater_2_enabled;
                        if (msg.payload.spa_setpoint != null) this.spaSetpoint = msg.payload.spa_setpoint;
                    }
                    break;

                case 'ChemistryUpdate':
                    if (msg.payload) {
                        if (msg.payload.ph != null && msg.payload.ph !== 0) { this.ph = msg.payload.ph; this._touch('ph'); }
                        if (msg.payload.orp != null && msg.payload.orp !== 0) { this.orp = msg.payload.orp; this._touch('orp'); }
                        if (msg.payload.salt_level != null && msg.payload.salt_level !== 0) { this.saltPpm = msg.payload.salt_level; this._touch('saltPpm'); }
                    }
                    break;

                case 'SystemStatusChange': {
                    const statusType = msg.payload?.status_type;
                    this.systemStatus = _normaliseSystemStatus(statusType);
                    if (statusType === 'Normal' && this.buttons.length === 0) {
                        this._fetchButtons();
                    }
                    break;
                }

                case 'CirculationUpdate':
                    // Live circulation/active-body change. Merge the per-body active
                    // state into bodies[] (the source of truth for spaModeActive) by id;
                    // append any body we have not seen yet. Reassign the array so Alpine
                    // re-evaluates hasDualBody / spaModeActive.
                    if (Array.isArray(msg.payload?.bodies)) {
                        const next = this.bodies.map(b => ({ ...b }));
                        for (const upd of msg.payload.bodies) {
                            const idLc = String(upd.id || '').toLowerCase();
                            const existing = next.find(b => String(b.id || '').toLowerCase() === idLc);
                            if (existing) {
                                existing.is_active = !!upd.is_active;
                            } else {
                                next.push({ id: upd.id, is_active: !!upd.is_active });
                            }
                        }
                        this.bodies = next;
                    }
                    break;

                case 'ButtonStateChange':
                    if (msg.payload?.button_id != null) {
                        const idx = this.buttons.findIndex(b => b.id === msg.payload.button_id);
                        if (idx !== -1) {
                            const updates = { status: msg.payload.status };
                            if (msg.payload.label) updates.label = msg.payload.label;
                            this.buttons[idx] = { ...this.buttons[idx], ...updates };
                            // If a command for this button was in flight, the controller has
                            // now reported the resulting state — confirm it as 'applied'
                            // (this is the authoritative success signal, not the HTTP 200).
                            if (this.commandStates[msg.payload.button_id]?.state === 'sending') {
                                this._setCommandState(msg.payload.button_id, 'applied');
                            }
                        } else {
                            // New button discovered — re-fetch to get full data (device_type, etc.)
                            this._fetchButtons();
                        }
                    }
                    break;
            }
        },

        // Clear a command state after COMMAND_STATE_CLEAR_MS and notify Alpine.
        _scheduleClear(key) {
            return setTimeout(() => this._clearCommandState(key), COMMAND_STATE_CLEAR_MS);
        },

        _clearCommandState(key) {
            const { [key]: _removed, ...rest } = this.commandStates;
            this.commandStates = rest;
        },

        _setCommandState(key, state, sendingTimeoutMs = COMMAND_SENDING_TIMEOUT_MS) {
            const prev = this.commandStates[key];
            if (prev?.timer) clearTimeout(prev.timer);

            let timer;
            if (state === 'sending') {
                // In-flight command: if it has not resolved by the timeout (either no
                // HTTP response, or — after a 200 — no authoritative ButtonStateChange
                // confirming the controller actually changed the equipment), surface a
                // distinct 'timedout' state (then auto-clear it).
                timer = setTimeout(() => {
                    if (this.commandStates[key]?.state === 'sending') {
                        console.warn(`Command '${key}' timed out after ${sendingTimeoutMs}ms`);
                        Alpine.store('toast').show(window.AquaI18n.t('toast.command_timeout'), 'warn');
                        this.commandStates = { ...this.commandStates, [key]: { state: 'timedout', timer: this._scheduleClear(key) } };
                    }
                }, sendingTimeoutMs);
            } else {
                // Terminal state ('applied'/'rejected'/'blocked'/'timedout'): auto-clear.
                timer = this._scheduleClear(key);
            }

            this.commandStates = { ...this.commandStates, [key]: { state, timer } };
        },

        getCommandState(key) {
            return this.commandStates[key]?.state || null;
        },

        async adjustSetpoint(target, delta) {
            const key = `setpoint:${target}`;
            if (!Alpine.store('system').commandsEnabled) {
                this._setCommandState(key, 'blocked');
                return;
            }

            try {
                const current = target === 'pool' ? this.poolSetpoint : this.spaSetpoint;
                const currentCelsius = typeof current === 'object' ? current.celsius : parseFloat(current);
                if (isNaN(currentCelsius)) return;
                // The backend quantizes setpoints to whole degrees (std::round in
                // webroute_equipment_setpoints.cpp); round the optimistic value the
                // same way so the UI never shows a fractional value the controller
                // will not honour.
                const newTemp = Math.round(currentCelsius + delta);
                const body = {};
                body[target] = newTemp;

                this._setCommandState(key, 'sending');

                const resp = await fetch('/api/equipment/setpoints', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(body)
                });

                if (!resp.ok) {
                    const reason = await this._readErrorReason(resp);
                    // A rejected command is an expected operational outcome (e.g.
                    // controller not ready), not a UI fault — warn, don't error.
                    console.warn(`Setpoint change rejected (HTTP ${resp.status}):`, reason);
                    this._setCommandState(key, 'rejected');
                    Alpine.store('toast').show(window.AquaI18n.t('toast.setpoint_failed_reason', { reason }), 'error');
                    return;
                }

                // The POST always returns 200; the per-target result carries the
                // real outcome. Treat the command as applied only when the server
                // confirms status === 'success' for this target.
                let data = null;
                try { data = await resp.json(); } catch (_) { /* tolerate empty/non-JSON body */ }
                const targetResult = data?.[target];
                if (targetResult?.status === 'success') {
                    // Prefer the server's authoritative (quantized) celsius value.
                    const applied = targetResult.celsius != null ? targetResult.celsius : newTemp;
                    if (target === 'pool' && typeof this.poolSetpoint === 'object') {
                        this.poolSetpoint = { ...this.poolSetpoint, celsius: applied };
                    } else if (target === 'spa' && typeof this.spaSetpoint === 'object') {
                        this.spaSetpoint = { ...this.spaSetpoint, celsius: applied };
                    }
                    this._setCommandState(key, 'applied');
                } else {
                    console.warn(`Setpoint change for '${target}' was not applied:`, targetResult);
                    this._setCommandState(key, 'rejected');
                    Alpine.store('toast').show(window.AquaI18n.t('toast.setpoint_not_applied'), 'error');
                }
            } catch (e) {
                console.error('Failed to adjust setpoint:', e);
                this._setCommandState(key, 'rejected');
                Alpine.store('toast').show(window.AquaI18n.t('toast.setpoint_failed_conn'), 'error');
            }
        },

        // Best-effort extraction of a human-readable failure reason from a
        // non-ok response. Structured errors ({error, code, params} —
        // docs/i18n.md) translate via the error.<code> catalog entry; legacy
        // JSON {error|message} or plain text pass through, falling back to
        // the HTTP status text.
        async _readErrorReason(resp) {
            try {
                const text = await resp.text();
                if (text) {
                    try {
                        const j = JSON.parse(text);
                        const translated = window.AquaI18n.apiError(j, null);
                        if (translated) return String(translated);
                    } catch (_) { /* not JSON — use raw text */ }
                    return text;
                }
            } catch (_) { /* body unreadable */ }
            return resp.statusText || `HTTP ${resp.status}`;
        },

        async toggleButton(id) {
            if (!Alpine.store('system').commandsEnabled) {
                this._setCommandState(id, 'blocked');
                return;
            }

            try {
                this._setCommandState(id, 'sending');

                const resp = await fetch(`/api/equipment/buttons/${id}`, { method: 'POST' });

                if (!resp.ok) {
                    const reason = await this._readErrorReason(resp);
                    // A rejected command is an expected operational outcome (e.g.
                    // controller not ready), not a UI fault — warn, don't error.
                    console.warn(`Command for button '${id}' rejected (HTTP ${resp.status}):`, reason);
                    this._setCommandState(id, 'rejected');
                    Alpine.store('toast').show(window.AquaI18n.t('toast.command_failed_reason', { reason }), 'error');
                    return;
                }

                // HTTP 200 means the controller ACCEPTED the command for transmission, NOT
                // that the equipment has changed yet. The response body's status is the
                // PRE-toggle state, so we deliberately do NOT paint it as the new status and
                // do NOT mark the command 'applied' here — doing so was the optimistic bug
                // that showed false success when a command silently did nothing. Instead we
                // keep the command in-flight ('sending') and wait for the authoritative
                // 'ButtonStateChange' WS event to confirm the real new state (-> 'applied').
                // If no confirmation arrives within COMMAND_CONFIRM_TIMEOUT_MS the command
                // honestly resolves to 'timedout'.
                try { await resp.json(); } catch (_) { /* body intentionally ignored */ }
                this._setCommandState(id, 'sending', COMMAND_CONFIRM_TIMEOUT_MS);
            } catch (e) {
                console.error('Failed to toggle button:', e);
                this._setCommandState(id, 'rejected');
                Alpine.store('toast').show(window.AquaI18n.t('toast.command_failed_conn'), 'error');
            }
        },

        // Toggle the circulation mode between pool (enable=false) and spa
        // (enable=true) via SetCirculationMode. Mirrors toggleButton: the POST
        // 200 only means the command was accepted for transmission, so we hold
        // the command 'sending' and wait for the authoritative bodies[].is_active
        // state (re-polled from /api/equipment, since there is no WS event for it)
        // to confirm the change — no optimistic apply.
        async toggleSpaMode(enable) {
            const key = 'circulation';
            if (!Alpine.store('system').commandsEnabled) {
                this._setCommandState(key, 'blocked');
                return;
            }

            try {
                this._setCommandState(key, 'sending', COMMAND_CONFIRM_TIMEOUT_MS);

                const resp = await fetch('/api/equipment/circulation', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ mode: enable ? 'spa' : 'pool' })
                });

                if (!resp.ok) {
                    const reason = await this._readErrorReason(resp);
                    console.warn(`Spa mode change rejected (HTTP ${resp.status}):`, reason);
                    this._setCommandState(key, 'rejected');
                    Alpine.store('toast').show(window.AquaI18n.t('toast.spa_mode_failed_reason', { reason }), 'error');
                    return;
                }

                try { await resp.json(); } catch (_) { /* body intentionally ignored */ }
                // Authoritative confirmation: poll /api/equipment until the Spa
                // body's active state matches the request (or the command times out).
                this._confirmCirculation(enable);
            } catch (e) {
                console.error('Failed to toggle spa mode:', e);
                this._setCommandState(key, 'rejected');
                Alpine.store('toast').show(window.AquaI18n.t('toast.spa_mode_failed_conn'), 'error');
            }
        },

        // Re-poll /api/equipment until bodies[].is_active reflects the requested
        // circulation mode, then confirm the command 'applied'. Stops early if the
        // command is no longer in flight (timed out / superseded). If the mode is
        // never observed, the command is left 'sending' to resolve via its
        // COMMAND_CONFIRM_TIMEOUT_MS -> 'timedout' path.
        async _confirmCirculation(enable, attempt = 0) {
            const key = 'circulation';
            if (this.commandStates[key]?.state !== 'sending') return;

            await this._fetchEquipment();
            if (this.commandStates[key]?.state !== 'sending') return;

            if (this.spaModeActive === enable) {
                this._setCommandState(key, 'applied');
                return;
            }

            if (attempt < CIRCULATION_CONFIRM_MAX_ATTEMPTS) {
                setTimeout(() => this._confirmCirculation(enable, attempt + 1), CIRCULATION_CONFIRM_POLL_MS);
            }
        },

        // Map a heater button to its body-of-water keyword for /api/equipment/heater.
        // Checked solar-first so "Solar Heat" is not mis-classified by a 'pool'/'spa'
        // substring. Returns null for an unrecognised heater label.
        _heaterBodyFor(button) {
            const label = String(button.label || '').toLowerCase();
            if (label.includes('solar')) return 'solar';
            if (label.includes('spa')) return 'spa';
            if (label.includes('pool')) return 'pool';
            return null;
        },

        // Enable/disable a heater via SetHeaterMode (the validated heater path),
        // instead of the generic DeviceActuator button toggle. Keyed on the heater
        // button's id so the existing ButtonStateChange -> 'applied' confirmation
        // applies — mirrors toggleButton (no optimistic apply). Falls back to the
        // generic button toggle for an unrecognised heater body.
        async setHeaterMode(button, enable) {
            const id = button.id;
            if (!Alpine.store('system').commandsEnabled) {
                this._setCommandState(id, 'blocked');
                return;
            }

            const body = this._heaterBodyFor(button);
            if (!body) {
                console.warn(`Unrecognised heater body for '${button.label}'; using generic button toggle`);
                return this.toggleButton(id);
            }

            try {
                this._setCommandState(id, 'sending');

                const resp = await fetch('/api/equipment/heater', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ body, enable })
                });

                if (!resp.ok) {
                    const reason = await this._readErrorReason(resp);
                    console.warn(`Heater command for '${button.label}' rejected (HTTP ${resp.status}):`, reason);
                    this._setCommandState(id, 'rejected');
                    Alpine.store('toast').show(window.AquaI18n.t('toast.heater_failed_reason', { reason }), 'error');
                    return;
                }

                try { await resp.json(); } catch (_) { /* body intentionally ignored */ }
                // 200 = accepted for transmission. Wait for the authoritative
                // ButtonStateChange WS event to confirm the heater actually changed.
                this._setCommandState(id, 'sending', COMMAND_CONFIRM_TIMEOUT_MS);
            } catch (e) {
                console.error('Failed to set heater mode:', e);
                this._setCommandState(id, 'rejected');
                Alpine.store('toast').show(window.AquaI18n.t('toast.heater_failed_conn'), 'error');
            }
        }
    });
});
