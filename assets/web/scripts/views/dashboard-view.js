/**
 * Dashboard View — Component logic for the main dashboard
 *
 * Includes a 1-second tick for telemetry freshness display.
 */
function dashboardView() {
    return {
        _tick: 0,
        _tickTimer: null,

        init() {
            this._tickTimer = setInterval(() => { this._tick++; }, 1000);
        },

        destroy() {
            if (this._tickTimer) { clearInterval(this._tickTimer); this._tickTimer = null; }
        },

        get lastUpdateText() {
            const lu = Alpine.store('pool').lastUpdate;
            if (!lu) return '';
            return lu.toLocaleTimeString();
        },

        ageText(field) {
            void this._tick; // reactive dependency on tick
            const a = Alpine.store('pool').age(field);
            const t = window.AquaI18n.t;
            if (a === null) return '';
            if (a < 5) return t('time.just_now');
            if (a < 60) return t('time.seconds_ago', { n: a });
            if (a < 3600) return t('time.minutes_ago', { n: Math.floor(a / 60) });
            return t('time.hours_ago', { n: Math.floor(a / 3600) });
        },

        isStale(field) {
            void this._tick;
            return Alpine.store('pool').isStale(field);
        },

        // ---- Equipment grouping ----------------------------------------
        // Heaters get their own setpoint+toggle block; the controllable
        // (toggleable) devices become switches; everything else
        // (configurable-but-not-controllable, e.g. sensors) is read-only.
        // The chlorinator is surfaced in the Water Chemistry section, not here.
        isHeater(b) {
            return b.device_type === 'Heater' || (b.label && b.label.toLowerCase().includes('heat'));
        },

        heaterButtons() {
            return Alpine.store('pool').buttons.filter(b => this.isHeater(b));
        },

        controlButtons() {
            return Alpine.store('pool').buttons.filter(b => b.controllable && !this.isHeater(b));
        },

        otherButtons() {
            return Alpine.store('pool').buttons.filter(b => !b.controllable && b.device_type !== 'Chlorinator');
        }
    };
}
