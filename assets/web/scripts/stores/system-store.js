/**
 * System Store — Global system state machine
 *
 * Derives an effective UI state from backend-reported state + WebSocket connectivity.
 *
 * States:
 *   Ready        — system operational, commands allowed
 *   Starting     — backend has not yet discovered equipment
 *   Disconnected — WebSocket connection lost
 *   ServiceMode  — controller is in service mode
 *   MonitorOnly  — user-toggled read-only mode
 *
 * Priority: ServiceMode > Starting > Disconnected > MonitorOnly > Ready
 */
document.addEventListener('alpine:init', () => {
    Alpine.store('system', {
        // Backend-reported state (from SystemStateUpdate WS event)
        backendState: 'starting',
        poolConfiguration: 'Unknown',
        equipmentMode: 'Normal',

        // Client-side flags
        monitorOnly: JSON.parse(localStorage.getItem('monitorOnly') || 'false'),

        // Server info (from /api/version REST response)
        serverStartTime: null,
        uptimeSeconds: null,

        // Transition tracking for service mode modal
        _prevBackendState: null,
        _serviceModalDismissed: false,

        // Device status tracking (from SystemStatusChange events)
        deviceStatuses: [],

        // Formatted server uptime + start time (from /api/version) for display.
        get uptime() {
            const s = this.uptimeSeconds;
            if (s == null) return '';
            const t = window.AquaI18n.t;
            const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
            if (d) return t('time.days_hours', { d, h });
            if (h) return t('time.hours_minutes', { h, m });
            return t('time.minutes', { m });
        },
        get startedAt() {
            if (!this.serverStartTime) return '';
            try { return window.AquaI18n.formatDateTime(this.serverStartTime); } catch (_) { return this.serverStartTime; }
        },

        get state() {
            const wsConnected = Alpine.store('ws').connected;

            if (this.backendState === 'service_mode') return 'ServiceMode';
            if (this.backendState === 'starting') return 'Starting';
            if (!wsConnected) return 'Disconnected';
            if (this.monitorOnly) return 'MonitorOnly';
            return 'Ready';
        },

        get commandsEnabled() {
            return this.state === 'Ready';
        },

        get disableReason() {
            const t = window.AquaI18n.t;
            switch (this.state) {
                case 'ServiceMode': return t('system.reason_service');
                case 'Starting': return t('system.reason_starting');
                case 'Disconnected': return t('system.reason_disconnected');
                case 'MonitorOnly': return t('system.reason_monitor');
                default: return '';
            }
        },

        get showServiceModal() {
            return this.state === 'ServiceMode' &&
                   this._prevBackendState !== null &&
                   this._prevBackendState !== 'service_mode' &&
                   !this._serviceModalDismissed;
        },

        toggleMonitorOnly() {
            this.monitorOnly = !this.monitorOnly;
            localStorage.setItem('monitorOnly', JSON.stringify(this.monitorOnly));
        },

        dismissServiceModal() {
            this._serviceModalDismissed = true;
        },

        /**
         * Single source of truth for the inferred 'starting' -> 'ready' transition.
         * Called from both this store (SystemStatusChange) and the pool store
         * (initial /api/equipment load) so the inference lives in one place.
         */
        markReadyIfStarting() {
            if (this.backendState === 'starting') {
                this._prevBackendState = this.backendState;
                this.backendState = 'ready';
                return true;
            }
            return false;
        },

        handleEvent(msg) {
            if (!msg || !msg.type) return;

            if (msg.type === 'SystemStateUpdate') {
                const p = msg.payload;
                if (!p) return;

                this._prevBackendState = this.backendState;

                if (p.state) {
                    this.backendState = p.state;
                    // Reset service modal dismiss when leaving service mode
                    if (p.state !== 'service_mode') {
                        this._serviceModalDismissed = false;
                    }
                }
                if (p.pool_configuration) this.poolConfiguration = p.pool_configuration;
                if (p.equipment_mode) this.equipmentMode = p.equipment_mode;
            }

            if (msg.type === 'SystemStatusChange') {
                const p = msg.payload;
                if (!p) return;
                // Device status transitions: Initializing → Normal
                // When any device reports Normal, system is operational
                if (p.status_type === 'Normal') {
                    this.markReadyIfStarting();
                }

                // Track per-device status for diagnostics
                if (p.source_name && p.status_type) {
                    const key = p.source_name + ':' + (p.source_type || '');
                    const idx = this.deviceStatuses.findIndex(d => d.key === key);
                    const entry = {
                        key,
                        sourceName: p.source_name,
                        sourceType: p.source_type || '',
                        statusType: p.status_type,
                        lastSeen: window.AquaI18n.formatTime(new Date())
                    };
                    if (idx !== -1) {
                        this.deviceStatuses[idx] = entry;
                    } else {
                        this.deviceStatuses = [...this.deviceStatuses, entry];
                    }
                }
            }
        }
    });
});
