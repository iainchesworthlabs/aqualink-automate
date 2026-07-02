/**
 * Chemistry Gauge — Semi-circular SVG arc gauge component
 *
 * Three-tier bands: Good / Okay / Bad
 * Configurable via localStorage (see ui-constants.js CHEMISTRY_BANDS_KEY).
 *
 * Band defaults + the localStorage key are the single source of truth in
 * scripts/config/ui-constants.js (shared with settings-view.js); the
 * display-only fields (min/max/unit/decimals/label) live here.
 */

// Dial geometry: a ~270-degree ring (r=54) drawn as two SVG circles with
// stroke-dasharray on a full circle. The SVG is rotated 135deg so the 90deg
// opening sits at the bottom. Track shows the full 270deg arc; the value
// circle shows `percentage` of it (its round line-cap forms the value dot).
const GAUGE_RADIUS = 54;
const GAUGE_CIRCUMFERENCE = 2 * Math.PI * GAUGE_RADIUS;   // full circle
const GAUGE_ARC = 0.75 * GAUGE_CIRCUMFERENCE;             // 270deg visible dial

function chemistryGauge(type) {
    // Display-only configuration (not user-editable; kept local to the gauge).
    // Labels resolve through the catalog at access time (see cfg.label getter
    // below) so a locale switch re-renders them.
    const display = {
        ph:   { labelKey: 'chem.ph',   min: 6.5, max: 8.5,  unit: '',     decimals: 1 },
        orp:  { labelKey: 'chem.orp',  min: 400, max: 900,  unit: ' mV',  decimals: 0 },
        salt: { labelKey: 'chem.salt', min: 0,   max: 6000, unit: ' ppm', decimals: 0 }
    };

    // Band defaults from the shared config (fall back to a local copy so the
    // gauge still renders if ui-constants.js is not loaded).
    const ui = (typeof window !== 'undefined' && window.AquaUI) || {};
    const bandsKey = ui.CHEMISTRY_BANDS_KEY || 'chemistryBands';
    const bandDefaults = (ui.CHEMISTRY_BAND_DEFAULTS) || {
        ph:   { goodMin: 7.4,  goodMax: 7.6,  okayMin: 7.2,  okayMax: 7.8,  badMin: 7.0,  badMax: 8.0 },
        orp:  { goodMin: 700,  goodMax: 750,  okayMin: 650,  okayMax: 700,  badMin: 650,  badMax: 800 },
        salt: { goodMin: 3500, goodMax: 4000, okayMin: 2700, okayMax: 3500, badMin: 2700, badMax: 4500 }
    };

    const displayCfg = display[type] || display.ph;
    const bandCfg = bandDefaults[type] || bandDefaults.ph;

    // Load overrides from localStorage
    const stored = JSON.parse(localStorage.getItem(bandsKey) || '{}');
    const overrides = stored[type] || {};
    const cfg = { ...displayCfg, ...bandCfg, ...overrides };
    Object.defineProperty(cfg, 'label', {
        get() { return window.AquaI18n.t(this.labelKey); },
        enumerable: true,
    });

    return {
        cfg,

        // 270deg dial dasharrays (drawn on a full circle): the track shows the
        // whole visible arc; the value shows the filled fraction. The trailing
        // full-circumference value hides the remainder.
        get trackDash() { return `${GAUGE_ARC.toFixed(2)} ${GAUGE_CIRCUMFERENCE.toFixed(2)}`; },
        get valueDash() {
            const filled = this.numericValue === null ? 0 : (this.percentage / 100) * GAUGE_ARC;
            return `${filled.toFixed(2)} ${GAUGE_CIRCUMFERENCE.toFixed(2)}`;
        },

        get value() {
            const store = Alpine.store('pool');
            let raw;
            switch (type) {
                case 'ph':   raw = store.ph; break;
                case 'orp':  raw = store.orp; break;
                case 'salt': raw = store.saltPpm; break;
                default:     return '--';
            }
            // Treat 0 as unknown — real sensors never report exactly 0
            if (raw === 0 || raw === '0' || raw === '0.0') return '--';
            return raw;
        },

        get numericValue() {
            const v = parseFloat(this.value);
            return isNaN(v) ? null : v;
        },

        get displayValue() {
            if (this.numericValue === null) return '--';
            return this.numericValue.toFixed(cfg.decimals) + cfg.unit;
        },

        get percentage() {
            if (this.numericValue === null) return 0;
            return Math.max(0, Math.min(100,
                ((this.numericValue - cfg.min) / (cfg.max - cfg.min)) * 100
            ));
        },

        /** Three-tier band: 'good', 'okay', or 'bad' */
        get band() {
            const v = this.numericValue;
            if (v === null) return null;
            if (v >= cfg.goodMin && v <= cfg.goodMax) return 'good';
            if (v >= cfg.okayMin && v <= cfg.okayMax) return 'okay';
            return 'bad';
        },

        get statusColor() {
            switch (this.band) {
                case 'good': return 'var(--good)';
                case 'okay': return 'var(--warn)';
                case 'bad':  return 'var(--bad)';
                default:     return 'var(--text-faint)';
            }
        },

        get statusLabel() {
            switch (this.band) {
                case 'good': return window.AquaI18n.t('tier.good');
                case 'okay': return window.AquaI18n.t('tier.okay');
                case 'bad':  return window.AquaI18n.t('tier.bad');
                default:     return '';
            }
        }
    };
}
