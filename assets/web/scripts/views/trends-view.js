/**
 * Trends View — history charts (WS2), redesigned.
 *
 * The page is built around three faceted, vertically-stacked panels that share
 * one time axis and one synchronized hover crosshair:
 *   1. Temperature — a real °C axis (all temps share a unit).
 *   2. Water chemistry — an auto-scaled overlay: each series is normalised to its
 *      own range so the SHAPES are comparable even though salt (ppm), pH, ORP
 *      (mV) and SWG (%) have wildly different magnitudes. Exact values live in
 *      the hover readout and the stat strip.
 *   3. Equipment runtime — on/off state drawn as a timeline of bars (not a
 *      meaningless 0/1 line), reconstructed from transition samples, plus a
 *      "% runtime in window" stat.
 *
 * All drawing is hand-rolled on one <canvas> (Chart.js can't do the equipment
 * lane or a crosshair shared across stacked panels). The canvas + its data and
 * DOM listeners live in the module-side `_trends` object, OUTSIDE Alpine's
 * reactive proxy — a proxied canvas/RAF state recurses badly.
 */

const _trends = { data: {}, geom: null, hoverIdx: null, bound: false };

// Canvas <ctx.font> can't read CSS custom properties, so the reskin UI font is
// spelled out here as a literal stack (matches --font-ui, including the
// vendored Arabic face for translated panel labels).
const TREND_FONT = "\"Hanken Grotesk\", \"Noto Sans Arabic\", system-ui, sans-serif";

// Labels resolve through the i18n catalog at render time (see the `ranges`
// getter) — duration abbreviations differ per language ("7d" vs "٧ ي" vs "7日").
const TREND_RANGES = [
    { n: 1, unitKey: 'time.abbr_hours', seconds: 3600 },
    { n: 6, unitKey: 'time.abbr_hours', seconds: 21600 },
    { n: 24, unitKey: 'time.abbr_hours', seconds: 86400 },
    { n: 7, unitKey: 'time.abbr_days', seconds: 604800 },
    { n: 30, unitKey: 'time.abbr_days', seconds: 2592000 },
];

// Stable colours for the well-known analog series, tied to the reskin design
// tokens so the chart tracks the active theme/accent. Values here are CSS custom
// property names; `_resolveColor()` turns them into concrete colours at draw time
// (canvas strokeStyle can't read `var(--x)` itself). Devices cycle a palette.
const TREND_COLORS = {
    'temp/pool': 'var(--accent)', 'temp/spa': 'var(--spa)', 'temp/air': 'var(--good)',
    'chem/salt_ppm': 'var(--warn)', 'chem/ph': 'var(--accent)', 'chem/orp': 'var(--bad)',
    'swg/percent': 'var(--accent)',
};
// Equipment lanes need several distinguishable hues; the reskin has no discrete
// per-device tokens, so this palette is a fixed set of oklch hues chosen to read
// on both the light and dark surfaces.
const DEVICE_PALETTE = [
    'oklch(0.72 0.13 250)', 'oklch(0.78 0.13 82)', 'oklch(0.76 0.12 162)',
    'oklch(0.72 0.13 330)', 'oklch(0.68 0.16 25)', 'oklch(0.74 0.11 200)',
    'oklch(0.70 0.14 300)', 'oklch(0.76 0.13 130)',
];

// Catalog keys for the well-known analog series; resolved at display time so a
// locale switch re-renders them. Device series use their (device-originated)
// labels verbatim.
const TREND_LABEL_KEYS = {
    'temp/pool': 'common.pool', 'temp/spa': 'common.spa', 'temp/air': 'common.air',
    'chem/salt_ppm': 'chem.salt', 'chem/ph': 'chem.ph', 'chem/orp': 'chem.orp', 'swg/percent': 'chem.swg',
};

function _titleCase(s) {
    return s.split(/[_/]/).filter(Boolean)
        .map((w) => w.charAt(0).toUpperCase() + w.slice(1)).join(' ');
}

function _groupOf(key) {
    if (key.startsWith('temp/')) return 'temp';
    if (key.startsWith('chem/') || key.startsWith('swg/')) return 'chem';
    if (key.startsWith('device/')) return 'device';
    return 'other';
}

function _displayName(s) {
    if (TREND_LABEL_KEYS[s.key]) return window.AquaI18n.t(TREND_LABEL_KEYS[s.key]);
    if (s.key.startsWith('device/')) {
        if (s.label) return s.label;
        // Legacy label-keyed series: device/<slug>/state.
        const m = s.key.match(/^device\/(.+?)\/state$/);
        return _titleCase(m ? m[1] : s.key.replace(/^device\//, ''));
    }
    return _titleCase(s.key.replace(/^[^/]+\//, '').replace(/\/state$/, ''));
}

// Per-series value formatting for the readout / stats / chips. Digits follow
// the active locale; temperatures follow the display-units preference.
function _fmt(key, unit, v) {
    const n = window.AquaI18n.formatNumber;
    if (v == null || Number.isNaN(v)) return '—';
    if (key === 'chem/salt_ppm' || unit === 'ppm') return n(Math.round(v)) + ' ppm';
    if (key === 'chem/ph' || unit === 'pH') return n(v, { minimumFractionDigits: 2, maximumFractionDigits: 2 });
    if (key === 'chem/orp' || unit === 'mV') return n(Math.round(v)) + ' mV';
    if (key === 'swg/percent' || unit === '%') return n(Math.round(v)) + '%';
    // Temperatures stay °C here regardless of the display-units preference:
    // the chart's axis/scaling is Celsius, and a °F readout against a °C axis
    // would disagree. Converting the whole chart is a follow-up.
    if (key.startsWith('temp/') || unit === 'C') return n(v, { minimumFractionDigits: 1, maximumFractionDigits: 1 }) + '°C';
    return n(v, { maximumFractionDigits: 1 });
}

function _cssVar(name, fallback) {
    const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    return v || fallback;
}

// Resolve a stored series colour to a concrete value the canvas can paint.
// Series colours are kept as CSS token references (e.g. "var(--accent)") so the
// HTML swatches theme automatically; the canvas can't read custom properties, so
// unwrap `var(--x)` to its computed value here. Concrete colours pass through.
function _resolveColor(color) {
    if (typeof color !== 'string') return color;
    const m = color.match(/^var\((--[\w-]+)\)$/);
    return m ? _cssVar(m[1], '#888') : color;
}

function trendsView() {
    return {
        available: true,
        loading: false,
        error: '',
        series: [],          // visible, deduped model rows
        inactiveCount: 0,    // device series with no data in the window
        showInactive: false,
        selected: {},        // key -> bool
        rangeSeconds: 86400,
        get ranges() {
            return TREND_RANGES.map((r) => ({ ...r, label: window.AquaI18n.t(r.unitKey, { n: r.n }) }));
        },
        _loaded: false,
        _model: [],          // full model incl. hidden/inactive

        onShown() {
            if (this._loaded) { this.refresh(); return; }
            this._loaded = true;
            this._loadPrefs();
            this.loadSeries();
        },

        // Restore the last-used range / selection / inactive toggle across reloads.
        _loadPrefs() {
            try {
                const p = JSON.parse(localStorage.getItem('aqualink.trends.prefs') || '{}');
                if (typeof p.rangeSeconds === 'number') this.rangeSeconds = p.rangeSeconds;
                if (typeof p.showInactive === 'boolean') this.showInactive = p.showInactive;
                if (p.selected && typeof p.selected === 'object') this.selected = { ...p.selected };
            } catch (_) { /* corrupt prefs — fall back to defaults */ }
        },
        _savePrefs() {
            try {
                localStorage.setItem('aqualink.trends.prefs', JSON.stringify({
                    rangeSeconds: this.rangeSeconds,
                    showInactive: this.showInactive,
                    selected: this.selected,
                }));
            } catch (_) { /* storage unavailable — non-fatal */ }
        },

        // --- data ---------------------------------------------------------

        async loadSeries() {
            this.loading = true;
            this.error = '';
            try {
                const resp = await fetch('/api/history/series');
                if (resp.status === 503) { this.available = false; return; }
                this.available = true;
                if (!resp.ok) { this.error = window.AquaI18n.t('trends.load_failed'); return; }

                const raw = await resp.json();
                this._model = raw.map((s) => ({
                    key: s.key,
                    unit: s.unit || '',
                    label: s.label || '',
                    last_ts: s.last_ts || 0,
                    group: _groupOf(s.key),
                    name: _displayName(s),
                }));

                // Deduplicate device series that share a display name (legacy label
                // variants of the same output): keep the most recently updated.
                const byName = {};
                this._model.forEach((s) => {
                    if (s.group !== 'device') return;
                    const k = s.name.toLowerCase();
                    if (!byName[k] || s.last_ts > byName[k].last_ts) byName[k] = s;
                });
                this._model.forEach((s) => {
                    s.dup = (s.group === 'device') && byName[s.name.toLowerCase()] !== s;
                });

                this.assignColors();

                if (Object.keys(this.selected).length === 0) {
                    this._model.forEach((s) => {
                        this.selected[s.key] = s.key.startsWith('temp/') || s.key === 'chem/salt_ppm';
                    });
                }
                await this.refresh();
            } catch (e) {
                this.error = window.AquaI18n.t('trends.load_failed');
            } finally {
                this.loading = false;
            }
        },

        assignColors() {
            let di = 0;
            this._model.forEach((s) => {
                s.color = TREND_COLORS[s.key] || DEVICE_PALETTE[(di++) % DEVICE_PALETTE.length];
            });
        },

        // Fetch points for every selected series in parallel (not the old
        // serial N+1), then rebuild the visible model and redraw.
        async refresh() {
            if (!this.available) return;
            const to = Math.floor(Date.now() / 1000);
            const from = to - this.rangeSeconds;
            _trends.from = from; _trends.to = to;

            // Classify device series for this window from metadata: one whose last
            // sample predates the window has no in-window transitions to chart, so
            // it is "inactive" (a stale legacy alias or removed equipment) and is
            // tucked behind the "show inactive" reveal.
            this._model.forEach((s) => {
                s.inactive = s.group === 'device' && !s.dup && (s.last_ts || 0) < from;
            });
            this.inactiveCount = this._model.filter((s) => s.inactive).length;

            const targets = this._model.filter((s) => this.selected[s.key] && !s.inactive);
            await Promise.all(targets.map(async (s) => {
                try {
                    // State series want near-raw points so the step reconstructs
                    // cleanly; analog series downsample to ~500 buckets.
                    const mp = s.group === 'device' ? 2000 : 500;
                    const resp = await fetch(
                        `/api/history/series?key=${encodeURIComponent(s.key)}&from=${from}&to=${to}&max_points=${mp}`);
                    if (!resp.ok) { _trends.data[s.key] = []; return; }
                    const body = await resp.json();
                    _trends.data[s.key] = (body.points || []).map((p) => ({ t: p.ts, v: p.value }));
                } catch (_) { _trends.data[s.key] = []; }
            }));

            this.series = this._model.filter((s) => {
                if (s.dup) return false;                       // legacy label-variant duplicate
                if (s.inactive && !this.showInactive) return false;
                return true;
            });

            this.bindCanvas();
            this.draw();
            this.buildStats();
        },

        // --- interaction --------------------------------------------------

        setRange(seconds) { this.rangeSeconds = seconds; this._savePrefs(); this.refresh(); },
        toggleSeries(key) { this.selected[key] = !this.selected[key]; this._savePrefs(); this.refresh(); },
        toggleInactive() { this.showInactive = !this.showInactive; this._savePrefs(); this.refresh(); },

        // Latest fetched value for a selected series (chip / used by stats).
        currentValue(key) {
            const pts = _trends.data[key];
            if (!pts || pts.length === 0) return null;
            return pts[pts.length - 1].v;
        },
        currentLabel(s) {
            const v = this.currentValue(s.key);
            return v == null ? '' : _fmt(s.key, s.unit, v);
        },

        // All visible series in a group (selected or not) — drives the pickers.
        groupSeries(group) { return this.series.filter((s) => s.group === group); },
        // Only the selected series in a group — drives drawing / readout / stats.
        selectedGroup(group) { return this.series.filter((s) => s.group === group && this.selected[s.key]); },

        bindCanvas() {
            const canvas = this.$refs.trendsCanvas;
            if (!canvas || _trends.bound) return;
            _trends.bound = true;
            canvas.addEventListener('mousemove', (ev) => {
                const g = _trends.geom; if (!g) return;
                const rect = canvas.getBoundingClientRect();
                const mx = ev.clientX - rect.left;
                const frac = (mx - g.GL) / g.plotW;
                _trends.hoverIdx = Math.max(0, Math.min(1, frac));
                _trends.hoverX = mx;
                this.draw();
                this.updateReadout(mx);
            });
            canvas.addEventListener('mouseleave', () => {
                _trends.hoverIdx = null;
                this.$refs.trendsReadout.style.display = 'none';
                this.draw();
            });
            // The first draw can run while the view is still being laid out (the
            // route just switched, so clientWidth is 0 and draw() bails). Redraw
            // whenever the canvas gains or changes its layout size — this covers
            // both the initial reveal and window resizes.
            if (window.ResizeObserver) {
                _trends.ro = new ResizeObserver(() => this.draw());
                _trends.ro.observe(canvas);
            }
        },

        // --- drawing ------------------------------------------------------

        draw() {
            const canvas = this.$refs.trendsCanvas;
            if (!canvas) return;
            const ctx = canvas.getContext('2d');

            const cssW = canvas.clientWidth || (canvas.parentElement.clientWidth - 16);
            // The panel can be momentarily unlaid-out (route switching, first paint);
            // never resize the canvas to a zero width — it would blank the chart.
            if (cssW <= 0) return;
            const tsel = this.selectedGroup('temp');
            const csel = this.selectedGroup('chem');
            const esel = this.selectedGroup('device');

            const GL = 46, GR = 14, TOP = 16, GAP = 26, TH = 92, CH = 92, AXH = 22;
            const eqRows = Math.max(1, esel.length);
            const EH = 16 + eqRows * 20;
            const cssH = TOP + TH + GAP + CH + GAP + EH + AXH;

            const dpr = window.devicePixelRatio || 1;
            // Only reassign the canvas size when it actually changes — resizing it
            // alters its layout height and would re-trigger the ResizeObserver in a
            // loop otherwise.
            const wantW = Math.round(cssW * dpr), wantH = Math.round(cssH * dpr);
            if (canvas.width !== wantW || canvas.height !== wantH) {
                canvas.width = wantW; canvas.height = wantH;
                canvas.style.height = cssH + 'px';
            }
            ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
            ctx.clearRect(0, 0, cssW, cssH);

            const plotW = cssW - GL - GR;
            const from = _trends.from, to = _trends.to, span = (to - from) || 1;
            const xAt = (t) => GL + ((t - from) / span) * plotW;
            // Canvas colours read the reskin tokens so the chart tracks the active
            // theme/accent (the design uses --text-faint for axis text, --border for
            // gridlines, --text-dim for panel headings).
            const muted = _cssVar('--text-faint', '#94a3b8');
            const grid = _cssVar('--border', 'rgba(0,0,0,0.06)');

            const tempY = TOP, chemY = TOP + TH + GAP, eqY = chemY + CH + GAP;
            _trends.geom = { GL, GR, plotW, from, to, span, xAt, tempY, TH, chemY, CH, eqY, EH };

            const hx = _trends.hoverIdx != null ? (GL + _trends.hoverIdx * plotW) : null;
            const ht = _trends.hoverIdx != null ? (from + _trends.hoverIdx * span) : null;

            // ---- Temperature panel: real °C axis ----
            this._panelLabel(ctx, window.AquaI18n.t('trends.panel_temperature'), GL, tempY, muted);
            if (tsel.length) {
                let lo = Infinity, hi = -Infinity;
                tsel.forEach((s) => (_trends.data[s.key] || []).forEach((p) => { lo = Math.min(lo, p.v); hi = Math.max(hi, p.v); }));
                if (!isFinite(lo)) { lo = 0; hi = 1; }
                const pad = (hi - lo) * 0.15 || 1; lo -= pad; hi += pad;
                ctx.strokeStyle = grid; ctx.fillStyle = muted; ctx.lineWidth = 0.5;
                ctx.font = `11px ${TREND_FONT}`; ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
                for (let k = 0; k < 3; k++) {
                    const yy = tempY + 8 + k * (TH - 16) / 2, vv = hi - (hi - lo) * k / 2;
                    ctx.beginPath(); ctx.moveTo(GL, yy); ctx.lineTo(cssW - GR, yy); ctx.stroke();
                    ctx.fillText(vv.toFixed(1), GL - 6, yy);
                }
                tsel.forEach((s) => this._line(ctx, _trends.data[s.key], tempY + 8, TH - 16, lo, hi, _resolveColor(s.color), xAt, ht));
            } else this._empty(ctx, GL, plotW, tempY, TH, muted);

            // ---- Chemistry panel: auto-scaled overlay ----
            this._panelLabel(ctx, window.AquaI18n.t('trends.water_chemistry'), GL, chemY, muted, window.AquaI18n.t('trends.autoscaled_note'));
            if (csel.length) {
                ctx.strokeStyle = grid; ctx.lineWidth = 0.5;
                for (let k = 0; k < 3; k++) {
                    const yy = chemY + 8 + k * (CH - 16) / 2;
                    ctx.beginPath(); ctx.moveTo(GL, yy); ctx.lineTo(cssW - GR, yy); ctx.stroke();
                }
                csel.forEach((s) => {
                    const pts = _trends.data[s.key] || []; if (!pts.length) return;
                    let lo = Infinity, hi = -Infinity;
                    pts.forEach((p) => { lo = Math.min(lo, p.v); hi = Math.max(hi, p.v); });
                    if (hi - lo < 1e-6) hi = lo + 1;
                    this._line(ctx, pts, chemY + 8, CH - 16, lo, hi, _resolveColor(s.color), xAt, ht);
                });
            } else this._empty(ctx, GL, plotW, chemY, CH, muted);

            // ---- Equipment runtime timeline ----
            this._panelLabel(ctx, window.AquaI18n.t('trends.equipment_runtime'), GL, eqY, muted);
            if (esel.length) {
                esel.forEach((s, r) => {
                    const ry = eqY + 16 + r * 20 + 5;
                    ctx.strokeStyle = grid; ctx.lineWidth = 0.5;
                    ctx.beginPath(); ctx.moveTo(GL, ry); ctx.lineTo(cssW - GR, ry); ctx.stroke();
                    ctx.fillStyle = muted; ctx.font = `11px ${TREND_FONT}`;
                    ctx.textAlign = 'left'; ctx.textBaseline = 'middle';
                    ctx.fillText(s.name, GL + 2, ry - 9);
                    this._timeline(ctx, _trends.data[s.key] || [], ry, _resolveColor(s.color), xAt, from, to);
                });
            } else this._empty(ctx, GL, plotW, eqY + 10, EH - 10, muted);

            // ---- shared X axis ----
            ctx.strokeStyle = grid; ctx.lineWidth = 0.5;
            ctx.beginPath(); ctx.moveTo(GL, eqY + EH + 1); ctx.lineTo(cssW - GR, eqY + EH + 1); ctx.stroke();
            ctx.fillStyle = muted; ctx.font = `11px ${TREND_FONT}`; ctx.textBaseline = 'top';
            const ticks = this._timeTicks();
            ticks.forEach((lab, i) => {
                const x = GL + (i / (ticks.length - 1)) * plotW;
                ctx.textAlign = i === 0 ? 'left' : i === ticks.length - 1 ? 'right' : 'center';
                ctx.fillText(lab, x, eqY + EH + 6);
            });

            // ---- crosshair ----
            if (hx != null) {
                ctx.strokeStyle = muted; ctx.globalAlpha = 0.5; ctx.lineWidth = 1;
                ctx.beginPath(); ctx.moveTo(hx, tempY + 2); ctx.lineTo(hx, eqY + EH); ctx.stroke();
                ctx.globalAlpha = 1;
            }
        },

        _panelLabel(ctx, text, x, y, muted, hint) {
            ctx.fillStyle = _cssVar('--text-dim', '#64748b');
            ctx.font = `600 12px ${TREND_FONT}`;
            ctx.textAlign = 'left'; ctx.textBaseline = 'alphabetic';
            ctx.fillText(text, x, y);
            if (hint) {
                ctx.fillStyle = muted; ctx.font = `11px ${TREND_FONT}`;
                ctx.fillText('   ·   ' + hint, x + ctx.measureText(text).width, y);
            }
        },

        _line(ctx, pts, py, ph, lo, hi, color, xAt, ht) {
            if (!pts || !pts.length) return;
            const yAt = (v) => py + ph - 8 - ((v - lo) / (hi - lo)) * (ph - 16);
            ctx.strokeStyle = color; ctx.lineWidth = 1.8; ctx.lineJoin = 'round';
            ctx.beginPath();
            pts.forEach((p, i) => { const x = xAt(p.t), y = yAt(p.v); i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); });
            ctx.stroke();
            if (ht != null) {
                const p = this._nearest(pts, ht);
                if (p) { ctx.fillStyle = color; ctx.beginPath(); ctx.arc(xAt(p.t), yAt(p.v), 3, 0, 7); ctx.fill(); }
            }
        },

        // Reconstruct an on/off step from transition samples and draw "on" runs.
        // A transition to value v implies the state was !v immediately before, so
        // the lead-in before the first in-window sample is its inverse; the last
        // sample's state extends to the window end.
        _timeline(ctx, pts, ry, color, xAt, from, to) {
            if (!pts.length) return;
            const on = (v) => v >= 0.5;
            const segs = [];
            let prevT = from, prevState = !on(pts[0].v);
            pts.forEach((p) => { if (prevState) segs.push([prevT, p.t]); prevT = p.t; prevState = on(p.v); });
            if (prevState) segs.push([prevT, to]);
            ctx.fillStyle = color; ctx.globalAlpha = 0.85;
            segs.forEach(([a, b]) => {
                const x1 = xAt(a), x2 = Math.max(xAt(b), x1 + 2), h = 9, t = ry - h / 2, rr = 3;
                ctx.beginPath();
                ctx.moveTo(x1 + rr, t);
                ctx.arcTo(x2, t, x2, t + h, rr); ctx.arcTo(x2, t + h, x1, t + h, rr);
                ctx.arcTo(x1, t + h, x1, t, rr); ctx.arcTo(x1, t, x2, t, rr);
                ctx.fill();
            });
            ctx.globalAlpha = 1;
        },

        _empty(ctx, GL, plotW, py, ph, muted) {
            ctx.fillStyle = muted; ctx.font = `11px ${TREND_FONT}`;
            ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
            ctx.fillText(window.AquaI18n.t('trends.no_series_selected'), GL + plotW / 2, py + ph / 2);
        },

        _nearest(pts, t) {
            if (!pts.length) return null;
            let best = pts[0], bd = Math.abs(pts[0].t - t);
            for (let i = 1; i < pts.length; i++) { const d = Math.abs(pts[i].t - t); if (d < bd) { bd = d; best = pts[i]; } }
            return best;
        },

        // State of a transition series at time t (last sample at/before t; if none,
        // the inverse of the first sample, matching the lead-in reconstruction).
        _stateAt(pts, t) {
            if (!pts.length) return null;
            let state = !(pts[0].v >= 0.5);
            for (const p of pts) { if (p.t <= t) state = p.v >= 0.5; else break; }
            return state;
        },

        _timeTicks() {
            const map = {
                3600: ['60m', '45m', '30m', '15m', 'now'],
                21600: ['6h', '4h', '2h', '1h', 'now'],
                86400: ['24h', '18h', '12h', '6h', 'now'],
                604800: ['7d', '5d', '3d', '1d', 'now'],
                2592000: ['30d', '21d', '14d', '7d', 'now'],
            };
            return map[this.rangeSeconds] || ['', '', 'now'];
        },

        updateReadout(mx) {
            const g = _trends.geom; if (!g || _trends.hoverIdx == null) return;
            const ro = this.$refs.trendsReadout;
            const ht = g.from + _trends.hoverIdx * g.span;
            const ago = Math.max(0, g.to - ht);
            const when = ago < 90 ? 'now' : this._humanAgo(ago) + ' ago';

            let rows = '';
            ['temp', 'chem'].forEach((grp) => {
                this.selectedGroup(grp).forEach((s) => {
                    const p = this._nearest(_trends.data[s.key] || [], ht);
                    rows += this._roRow(s.color, s.name, p ? _fmt(s.key, s.unit, p.v) : '—', '');
                });
            });
            this.selectedGroup('device').forEach((s) => {
                const st = this._stateAt(_trends.data[s.key] || [], ht);
                rows += this._roRow(s.color, s.name, st ? 'on' : 'off', st ? 'on' : 'off');
            });

            ro.innerHTML = `<div class="trends-ro-when">${when}</div>${rows}`;
            ro.style.display = 'block';
            const rw = ro.offsetWidth;
            const left = Math.max(2, Math.min(mx + 14, g.GL + g.plotW + g.GR - rw));
            ro.style.left = left + 'px';
        },

        _roRow(color, name, value, stateClass) {
            const sw = stateClass
                ? `<span class="trends-ro-sw" style="background:${stateClass === 'on' ? color : 'transparent'};border:1px solid ${color}"></span>`
                : `<span class="trends-ro-sw" style="background:${color}"></span>`;
            const vClass = stateClass ? `trends-ro-${stateClass}` : '';
            return `<div class="trends-ro-row">${sw}<span class="trends-ro-name">${name}</span><span class="trends-ro-val ${vClass}">${value}</span></div>`;
        },

        _humanAgo(sec) {
            if (sec < 3600) return Math.round(sec / 60) + 'm';
            if (sec < 86400) return Math.round(sec / 3600) + 'h';
            return Math.round(sec / 86400) + 'd';
        },

        // --- stat strip ---------------------------------------------------

        buildStats() {
            const cards = [];
            ['temp', 'chem'].forEach((grp) => {
                this.selectedGroup(grp).forEach((s) => {
                    const pts = _trends.data[s.key] || []; if (!pts.length) return;
                    let mn = Infinity, mx = -Infinity, sum = 0;
                    pts.forEach((p) => { mn = Math.min(mn, p.v); mx = Math.max(mx, p.v); sum += p.v; });
                    cards.push({
                        key: s.key, color: s.color, name: s.name,
                        now: _fmt(s.key, s.unit, pts[pts.length - 1].v),
                        sub: `min ${_fmt(s.key, s.unit, mn)} · max ${_fmt(s.key, s.unit, mx)} · avg ${_fmt(s.key, s.unit, sum / pts.length)}`,
                    });
                });
            });
            this.selectedGroup('device').forEach((s) => {
                const pts = _trends.data[s.key] || []; if (!pts.length) return;
                cards.push({
                    key: s.key, color: s.color, name: s.name,
                    now: Math.round(this._runtimeFraction(pts) * 100) + '%',
                    sub: window.AquaI18n.t('trends.runtime_in_window'),
                });
            });
            this.statCards = cards;
        },
        statCards: [],

        _runtimeFraction(pts) {
            const from = _trends.from, to = _trends.to, span = (to - from) || 1;
            const on = (v) => v >= 0.5;
            let onTime = 0, prevT = from, prevState = !on(pts[0].v);
            pts.forEach((p) => { if (prevState) onTime += (p.t - prevT); prevT = p.t; prevState = on(p.v); });
            if (prevState) onTime += (to - prevT);
            return Math.max(0, Math.min(1, onTime / span));
        },
    };
}
