/**
 * Chlorinator (SWG) tile — shows the ACTUAL reported output and lets the user
 * set the TARGET output inline. The chlorinator is configured by output %, not an
 * on/off relay, so the control is a 0–100 target slider plus a boost toggle.
 *
 * Commands go via POST /api/equipment/chlorinator (ICommandDispatcher::
 * SetChlorinatorPercentage / SetChlorinatorBoost, which the backend turns into
 * iAQ panel navigation). The target slider seeds from the live CONFIGURED setpoint
 * (chlorinator.setpoint_percent) until the user grabs it, then submits on "Set".
 * "SWG Output" still shows the instantaneous generating %, which is 0 while idle —
 * so it is captioned with the server-derived chlorinator.generating_reason, which
 * distinguishes a chlorinator that is switched OFF from one that is configured and
 * healthy but not producing because the filter pump is not running.
 */
function chlorinatorControl() {
    // Instantaneous reported output (the "SWG Output" gauge); 0 while the cell is idle.
    function actualNum() {
        const v = Alpine.store('pool').swgGeneratingPercent;
        const n = (v === '--' || v == null) ? NaN : Number(v);
        return isNaN(n) ? null : Math.max(0, Math.min(100, Math.round(n)));
    }

    // Configured output setpoint for ONE body. Pool and spa are INDEPENDENT on the panel —
    // you can run the spa at 70% and the pool at 40% — so each has its own slider and each
    // seeds from its own value, never from a single "active body" figure.
    function setpointNum(body) {
        const v = Alpine.store('pool')[body === 'spa' ? 'swgSpaSetpoint' : 'swgPoolSetpoint'];
        const n = (v === '--' || v == null) ? NaN : Number(v);
        return isNaN(n) ? null : Math.max(0, Math.min(100, Math.round(n)));
    }

    return {
        // One target per body, each tracking its own configured setpoint until grabbed.
        targets: { pool: 50, spa: 50 },
        touched: { pool: false, spa: false },
        boost: false,
        busy: false,
        feedback: '',    // '' | 'applied' | 'rejected'
        _timer: null,

        get present() {
            return Alpine.store('pool').chlorinatorPresent === true;
        },

        // Actual (reported) output.
        get actual() { const n = actualNum(); return n == null ? 0 : n; },
        get actualLabel() { const n = actualNum(); return n == null ? '--' : (window.AquaI18n.formatNumber(n) + '%'); },

        // One-line explanation of that output, from the server-derived reason. A bare "0%"
        // reads as "the chlorinator is off or broken" even when it is configured, healthy and
        // simply waiting for the filter pump — this is the line that says which it is. Empty
        // while generating, so the number stands alone when it needs no explanation.
        get reasonLabel() { return Alpine.store('pool').chlorinatorReasonLabel; },

        // Everything except a real cell fault is informational -- style it muted so a waiting
        // (or not-yet-scraped) chlorinator does not read as a broken one.
        get reasonIsIdle() {
            const r = String(Alpine.store('pool').swgGeneratingReason || '');
            return r === 'PumpOff' || r === 'Idle' || r === 'Off' || r === 'Unknown';
        },

        // Only offer the spa row on a system that actually has a spa body.
        get bodies() { return Alpine.store('pool').hasDualBody ? ['pool', 'spa'] : ['pool']; },

        bodyLabel(body) { return window.AquaI18n.t(body === 'spa' ? 'common.spa' : 'common.pool'); },

        target(body) { return this.targets[body]; },
        setTarget(body, value) { this.targets[body] = Number(value); this.touched[body] = true; },

        // "Set" is meaningful when this body's target differs from its configured setpoint
        // (or, until that is known, the live actual output).
        changed(body) {
            const ref = setpointNum(body);
            const base = (ref == null) ? this.actual : ref;
            return Number(this.targets[body]) !== base;
        },

        // Health -> colour + band + label.
        get healthColor() {
            const h = String(Alpine.store('pool').chlorinatorHealth || '');
            if (h === 'Ok' || h === 'TurningOff') return 'var(--gauge-good)';
            if (h.startsWith('Warning')) return 'var(--gauge-warn)';
            if (h.startsWith('Error') || h === 'GeneralFault') return 'var(--gauge-bad)';
            return 'var(--text-muted)';
        },
        get healthBand() {
            const h = String(Alpine.store('pool').chlorinatorHealth || '');
            if (h === 'Ok' || h === 'TurningOff') return 'good';
            if (h.startsWith('Warning')) return 'okay';
            if (h.startsWith('Error') || h === 'GeneralFault') return 'bad';
            return null;
        },
        get healthLabel() {
            return Alpine.store('pool').chlorinatorHealthLabel;
        },

        // Every OTHER active health flag, translated, for a secondary "also:" list under
        // the primary badge above. Empty unless the cell is reporting more than one flag
        // at once (the wire status byte is a true bitfield).
        get secondaryHealthLabels() {
            const pool = Alpine.store('pool');
            const all = pool.chlorinatorHealthFlags;
            if (!Array.isArray(all) || all.length <= 1) { return []; }
            const primary = pool.chlorinatorHealth;
            return pool.chlorinatorHealthFlagsLabels.filter((_, i) => all[i] !== primary);
        },

        init() {
            // Track the live CONFIGURED setpoint into the target slider until the user grabs
            // it. The setpoint usually arrives on a poll AFTER this mounts, and can update
            // later (e.g. a fresh menu scrape), so keep re-seeding rather than latching once.
            this._seed();
            this._timer = setInterval(() => {
                if (this.touched.pool && this.touched.spa) { clearInterval(this._timer); this._timer = null; return; }
                this._seed();
            }, 1000);
        },

        destroy() {
            if (this._timer) { clearInterval(this._timer); this._timer = null; }
        },

        _seed() {
            for (const body of ['pool', 'spa']) {
                if (this.touched[body]) { continue; }
                const n = setpointNum(body);
                if (n != null) { this.targets[body] = n; }
            }
            return true;
        },

        async setPercent(body) {
            await this._post({ percentage: Number(this.targets[body]), body: body });
        },

        async toggleBoost() {
            const next = !this.boost;
            if (await this._post({ boost: next })) { this.boost = next; }
        },

        async _post(payload) {
            // Belt-and-suspenders alongside the button's :disabled="busy" binding: a second
            // click landing before Alpine re-renders that attribute (or any other reentrant
            // call) is a no-op rather than firing a duplicate request the panel is still
            // mid-way through the first one -- the exact overlap that used to surface as a
            // misleading "rejected" once the server had no way to say "busy, try again".
            if (this.busy) { return false; }
            this.busy = true;
            this.feedback = '';
            try {
                const resp = await fetch('/api/equipment/chlorinator', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(payload),
                });
                const ok = resp.ok;
                this.feedback = ok ? 'applied' : 'rejected';
                setTimeout(() => { this.feedback = ''; }, 2500);
                if (!ok) {
                    // A rejected command is an expected operational outcome (e.g. a previous
                    // chlorinator command is still being applied — HTTP 409), not a UI fault —
                    // warn, don't error, and tell the user why so a same-value retry a moment
                    // later doesn't read as a mysterious failure.
                    const reason = await this._readErrorReason(resp);
                    console.warn(`Chlorinator command rejected (HTTP ${resp.status}):`, reason);
                    Alpine.store('toast').show(window.AquaI18n.t('toast.chlorinator_failed_reason', { reason }), 'error');
                }
                return ok;
            } catch (e) {
                this.feedback = 'rejected';
                setTimeout(() => { this.feedback = ''; }, 2500);
                Alpine.store('toast').show(window.AquaI18n.t('toast.chlorinator_failed_conn'), 'error');
                return false;
            } finally {
                this.busy = false;
            }
        },

        // Mirrors pool-store's _readErrorReason: the server's structured {error, code} body
        // translated via the catalog when a matching 'error.<code>' key exists, else its raw
        // English message, else the HTTP status text.
        async _readErrorReason(resp) {
            try {
                const text = await resp.text();
                if (text) {
                    try {
                        const j = JSON.parse(text);
                        const translated = window.AquaI18n.apiError(j, null);
                        if (translated) { return String(translated); }
                    } catch (_) { /* not JSON — use raw text */ }
                    return text;
                }
            } catch (_) { /* body unreadable */ }
            return resp.statusText || `HTTP ${resp.status}`;
        },
    };
}
