/**
 * Alerts Store — tracks active fault conditions raised by the backend
 * AlertMonitor (WS3) and surfaces them as toasts + a persistent nav badge.
 *
 * WebSocket payload mapping (/ws/equipment):
 *   AlertTransition -> { condition, state: "raised"|"cleared", ts, detail }
 */

// Catalog keys for the known condition keys (falls back to the raw key).
const ALERT_LABEL_KEYS = {
    chlorinator_fault: 'alert.chlorinator_fault',
    chlorinator_warning: 'alert.chlorinator_warning',
    salt_low: 'alert.salt_low',
    service_mode: 'alert.service_mode',
    serial_comms_loss: 'alert.serial_comms_loss',
};

document.addEventListener('alpine:init', () => {
    Alpine.store('alerts', {
        // condition key -> { detail, ts }
        active: {},

        label(condition) {
            const key = ALERT_LABEL_KEYS[condition];
            return key ? window.AquaI18n.t(key) : condition;
        },

        // Translated detail text: built from condition + structured params
        // (alert_detail.<condition> catalog templates) when the backend sent
        // them; otherwise the backend's English detail passes through.
        detailText(condition, entry) {
            const api = window.AquaI18n;
            const key = 'alert_detail.' + condition;
            if (entry && entry.params && api.has(key)) {
                const params = { ...entry.params };
                // Chlorinator conditions carry the raw health enum name; map it
                // to its translated label (falls back to the raw name).
                if (params.health) {
                    const hk = window.AquaUI.swgHealthKey(params.health);
                    params.health = hk ? api.t(hk) : String(params.health).replace(/_/g, ' ');
                }
                return api.t(key, params);
            }
            return (entry && entry.detail) || '';
        },

        get count() {
            return Object.keys(this.active).length;
        },

        get hasAlerts() {
            return this.count > 0;
        },

        // List form for rendering (sorted by key for stable display). `detail`
        // is the translated text (localized template when params arrived).
        get list() {
            return Object.keys(this.active)
                .sort()
                .map((key) => ({
                    condition: key,
                    label: this.label(key),
                    ...this.active[key],
                    detail: this.detailText(key, this.active[key]),
                }));
        },

        handleEvent(msg) {
            if (!msg || msg.type !== 'AlertTransition') return;

            const p = msg.payload || {};
            const condition = p.condition;
            if (!condition) return;

            if (p.state === 'raised') {
                // Reassign the object so Alpine's reactivity sees the change.
                const entry = { detail: p.detail || '', params: p.params || null, ts: p.ts || 0 };
                this.active = { ...this.active, [condition]: entry };
                Alpine.store('toast').show(window.AquaI18n.t('alert.raised', { label: this.label(condition), detail: this.detailText(condition, entry) || window.AquaI18n.t('alert.fault_detected') }), 'error', 8000);
            } else {
                const next = { ...this.active };
                delete next[condition];
                this.active = next;
                Alpine.store('toast').show(window.AquaI18n.t('alert.cleared', { label: this.label(condition) }), 'info', 4000);
            }
        },
    });
});
