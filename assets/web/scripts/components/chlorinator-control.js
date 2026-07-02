/**
 * Chlorinator (SWG) tile — shows the ACTUAL reported output and lets the user
 * set the TARGET output inline. The chlorinator is configured by output %, not an
 * on/off relay, so the control is a 0–100 target slider plus a boost toggle.
 *
 * Commands go via POST /api/equipment/chlorinator (ICommandDispatcher::
 * SetChlorinatorPercentage / SetChlorinatorBoost, which the backend turns into
 * iAQ panel navigation). The target slider seeds from the live CONFIGURED setpoint
 * (chlorinator.setpoint_percent) until the user grabs it, then submits on "Set".
 * "SWG Output" still shows the instantaneous generating %, which is 0 while idle.
 */
function chlorinatorControl() {
    // Instantaneous reported output (the "SWG Output" gauge); 0 while the cell is idle.
    function actualNum() {
        const v = Alpine.store('pool').swgGeneratingPercent;
        const n = (v === '--' || v == null) ? NaN : Number(v);
        return isNaN(n) ? null : Math.max(0, Math.min(100, Math.round(n)));
    }

    // Configured output setpoint for the active body (what the Target slider should show).
    function setpointNum() {
        const v = Alpine.store('pool').swgSetpointPercent;
        const n = (v === '--' || v == null) ? NaN : Number(v);
        return isNaN(n) ? null : Math.max(0, Math.min(100, Math.round(n)));
    }

    return {
        target: 50,
        boost: false,
        busy: false,
        feedback: '',    // '' | 'applied' | 'rejected'
        touched: false,  // true once the user grabs the slider
        _timer: null,

        get present() {
            return Alpine.store('pool').chlorinatorPresent === true;
        },

        // Actual (reported) output.
        get actual() { const n = actualNum(); return n == null ? 0 : n; },
        get actualLabel() { const n = actualNum(); return n == null ? '--' : (n + '%'); },

        // "Set" is meaningful when the target differs from the configured setpoint
        // (or, until that is known, the live actual output).
        get changed() {
            const ref = setpointNum();
            const base = (ref == null) ? this.actual : ref;
            return Number(this.target) !== base;
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

        init() {
            // Track the live CONFIGURED setpoint into the target slider until the user grabs
            // it. The setpoint usually arrives on a poll AFTER this mounts, and can update
            // later (e.g. a fresh menu scrape), so keep re-seeding rather than latching once.
            this._seed();
            this._timer = setInterval(() => {
                if (this.touched) { clearInterval(this._timer); this._timer = null; return; }
                this._seed();
            }, 1000);
        },

        destroy() {
            if (this._timer) { clearInterval(this._timer); this._timer = null; }
        },

        _seed() {
            if (this.touched) { return true; }
            const n = setpointNum();
            if (n == null) { return false; }
            this.target = n;
            return true;
        },

        async setPercent() {
            await this._post({ percentage: Number(this.target) });
        },

        async toggleBoost() {
            const next = !this.boost;
            if (await this._post({ boost: next })) { this.boost = next; }
        },

        async _post(payload) {
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
                return ok;
            } catch (e) {
                this.feedback = 'rejected';
                setTimeout(() => { this.feedback = ''; }, 2500);
                return false;
            } finally {
                this.busy = false;
            }
        },
    };
}
