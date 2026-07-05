/**
 * Schedules View — unified time-based automation.
 *
 * Shows two complementary schedule sources on one surface:
 *   - App schedules  (/api/schedules)            — point actions the app fires; editable.
 *   - Controller schedules (/api/controller/...)  — the controller's own built-in
 *                                                   on→off programs; read-only span.
 *
 * Both are merged into one model and rendered two ways: a chronological list
 * (the editing surface) and a read-only 24-hour timeline "day view" for
 * comprehension. Overlaps where an app action contradicts a controller program
 * are flagged as conflicts. Mutating controls are disabled in monitor-only mode.
 */

// Labels resolve through the i18n catalog at render time (see the `days` /
// `actionTypes` getters below) so a locale switch re-renders them.
const SCHED_DAYS = [
    { bit: 0, labelKey: 'day.mon' },
    { bit: 1, labelKey: 'day.tue' },
    { bit: 2, labelKey: 'day.wed' },
    { bit: 3, labelKey: 'day.thu' },
    { bit: 4, labelKey: 'day.fri' },
    { bit: 5, labelKey: 'day.sat' },
    { bit: 6, labelKey: 'day.sun' },
];

const SCHED_ACTION_TYPES = [
    { value: 'button_on', labelKey: 'sched_action.button_on' },
    { value: 'button_off', labelKey: 'sched_action.button_off' },
    { value: 'button_toggle', labelKey: 'sched_action.button_toggle' },
    { value: 'pool_setpoint', labelKey: 'sched_action.pool_setpoint' },
    { value: 'spa_setpoint', labelKey: 'sched_action.spa_setpoint' },
    { value: 'chlorinator_percent', labelKey: 'sched_action.chlorinator_percent' },
    { value: 'circulation_mode', labelKey: 'sched_action.circulation_mode' },
];

function _schedDefaultForm() {
    return { name: '', enabled: true, days: 0b0011111, time: '08:00', actionType: 'button_toggle', target: '', value: 0 };
}

function _schedIsButton(type) {
    return type === 'button_on' || type === 'button_off' || type === 'button_toggle';
}
function _schedIsValue(type) {
    return type === 'pool_setpoint' || type === 'spa_setpoint' || type === 'chlorinator_percent';
}

// "HH:MM" -> minutes from local midnight (0..1439). Tolerant of bad input.
function _schedHm(t) {
    const parts = String(t || '00:00').split(':');
    const h = Number(parts[0]) || 0;
    const m = Number(parts[1]) || 0;
    return ((h * 60) + m) % 1440;
}
function _schedFmtMin(min) {
    const m = ((min % 1440) + 1440) % 1440;
    // Locale-digit clock time (zero-padded to 2 integer digits); the ':' is the
    // standard time separator across the locales we ship.
    const n = window.AquaI18n.formatNumber;
    return `${n(Math.floor(m / 60), { minimumIntegerDigits: 2 })}:${n(m % 60, { minimumIntegerDigits: 2 })}`;
}

function _schedToForm(s) {
    const f = _schedDefaultForm();
    f.name = s.name || '';
    f.enabled = s.enabled !== false;
    f.days = s.days_of_week || 0;
    f.time = s.time_local || '08:00';
    f.actionType = (s.action && s.action.type) || 'button_toggle';
    f.target = (s.action && s.action.target) || '';
    f.value = (s.action && typeof s.action.value === 'number') ? s.action.value : 0;
    return f;
}

function _schedFormToPayload(f) {
    const action = { type: f.actionType };
    if (_schedIsButton(f.actionType) || f.actionType === 'circulation_mode') {
        action.target = f.target;
    } else {
        action.value = Number(f.value);
    }
    return { name: f.name, enabled: f.enabled, days_of_week: f.days, time_local: f.time, action };
}

function schedulesView() {
    return {
        appAvailable: true,          // app scheduler enabled (--schedules-file)
        loading: false,
        error: '',
        schedules: [],               // app-side point schedules
        controller: { status: 'pending_capture', active_group: '', schedules: [] },
        buttons: [],                 // device labels for the target dropdown
        editing: null,               // null | 'new' | <uuid>
        form: _schedDefaultForm(),
        get days() {
            return SCHED_DAYS.map((d) => ({ bit: d.bit, label: window.AquaI18n.t(d.labelKey) }));
        },
        get actionTypes() {
            return SCHED_ACTION_TYPES.map((a) => ({ value: a.value, label: window.AquaI18n.t(a.labelKey) }));
        },
        view: 'list',                // 'list' | 'timeline'
        timelineDay: 0,              // bit 0..6 (Mon..Sun) shown in the timeline
        _loaded: false,

        onShown() {
            if (!this._loaded) {
                this._loaded = true;
                // Default the timeline to today (JS Sun=0..Sat=6 -> our Mon=0..Sun=6).
                this.timelineDay = (new Date().getDay() + 6) % 7;
            }
            this.load();
        },

        get monitorOnly() {
            try { return Alpine.store('system').monitorOnly; } catch (_) { return false; }
        },

        async load() {
            this.loading = true;
            this.error = '';
            try {
                const resp = await fetch('/api/schedules');
                if (resp.status === 503) { this.appAvailable = false; this.schedules = []; }
                else { this.appAvailable = true; if (resp.ok) { this.schedules = await resp.json(); } }

                try {
                    const c = await fetch('/api/controller/schedules');
                    if (c.ok) {
                        const cj = await c.json();
                        this.controller = {
                            status: cj.status || 'pending_capture',
                            active_group: cj.active_group || '',
                            schedules: Array.isArray(cj.schedules) ? cj.schedules : [],
                        };
                    }
                } catch (_) { /* controller source is best-effort */ }

                try {
                    const b = await fetch('/api/equipment/buttons');
                    if (b.ok) {
                        const bj = await b.json();
                        const list = Array.isArray(bj) ? bj : (bj.buttons || []);
                        // De-duplicate labels: the dropdown targets a device BY label, so a
                        // repeated label is meaningless here — and a duplicate would collide
                        // on the x-for :key, which crashes Alpine's list reconciliation and
                        // takes down the whole schedules form.
                        this.buttons = [...new Set(list.map((x) => x.label).filter(Boolean))];
                    }
                } catch (_) { /* dropdown is best-effort */ }
            } catch (e) {
                this.error = window.AquaI18n.t('sched.load_failed');
            } finally {
                this.loading = false;
            }
        },

        isButtonAction(t) { return _schedIsButton(t); },
        isValueAction(t) { return _schedIsValue(t); },

        // An app schedule can be promoted to a controller program only when it is
        // one edge (button_on OR button_off) of a pair the backend can fold into
        // an on→off span: a complementary app schedule must exist that toggles the
        // SAME target the opposite way on the SAME days. (button_toggle and
        // value/mode actions have no span form and are never promotable.)
        promotableApp(s) {
            const a = (s && s.action) || {};
            if (a.type !== 'button_on' && a.type !== 'button_off') { return false; }
            const want = (a.type === 'button_on') ? 'button_off' : 'button_on';
            const target = a.target || '';
            const days = s.days_of_week || 0;
            if (!target) { return false; }
            return (this.schedules || []).some((o) => {
                if (o === s || o.uuid === s.uuid) { return false; }
                const oa = o.action || {};
                return oa.type === want
                    && (oa.target || '') === target
                    && (o.days_of_week || 0) === days;
            });
        },

        actionSummary(s) {
            const t = window.AquaI18n.t;
            const a = s.action || {};
            if (_schedIsButton(a.type)) { return `${t('sched.act_' + a.type.replace('button_', ''))} ${a.target}`; }
            if (a.type === 'circulation_mode') { return t('sched.summary_circulation', { target: a.target }); }
            return `${a.type} ${a.value}`;
        },

        daySummary(mask) {
            if ((mask & 0x7f) === 0x7f) { return window.AquaI18n.t('sched.every_day'); }
            return SCHED_DAYS.filter((d) => (mask & (1 << d.bit)) !== 0).map((d) => window.AquaI18n.t(d.labelKey)).join(' ') || '—';
        },

        // --- unified model -------------------------------------------------

        // The device/circuit an app action drives, for conflict-matching and
        // timeline grouping. Setpoint/chlorinator/circulation map to friendly
        // synthetic targets so they get their own timeline rows.
        _appTarget(s) {
            const a = s.action || {};
            if (_schedIsButton(a.type)) { return a.target || ''; }
            if (a.type === 'chlorinator_percent') { return 'Chlorinator'; }
            if (a.type === 'pool_setpoint') { return 'Pool heater'; }
            if (a.type === 'spa_setpoint') { return 'Spa heater'; }
            if (a.type === 'circulation_mode') { return 'Circulation'; }
            return '';
        },

        _within(t, start, end) {
            if (end > start) { return t >= start && t < end; }
            if (end < start) { return t >= start || t < end; }  // crosses midnight
            return false;                                        // zero-length span
        },

        // Flag app actions that contradict an active controller program on a
        // shared day: an OFF or TOGGLE on the same device inside the program's
        // ON window. Mutates entries in place (sets .conflict + .conflictMsg).
        _annotateConflicts(entries) {
            const spans = entries.filter((e) => e.owner === 'controller' && e.kind === 'span' && e.enabled);
            for (const e of entries) {
                if (e.owner !== 'app' || !e.enabled) { continue; }
                const type = (e.raw.action || {}).type;
                if (type !== 'button_off' && type !== 'button_toggle') { continue; }
                for (const sp of spans) {
                    if (!sp.target || !e.target || sp.target.toLowerCase() !== e.target.toLowerCase()) { continue; }
                    if ((sp.days & e.days) === 0) { continue; }
                    if (this._within(e.start, sp.start, sp.end)) {
                        e.conflict = true;
                        sp.conflict = true;
                        e.conflictMsg = window.AquaI18n.t('sched.conflict_msg', {
                            target: e.target,
                            how: window.AquaI18n.t(type === 'button_off' ? 'sched.conflict_how_off' : 'sched.conflict_how_toggle'),
                            start: _schedFmtMin(sp.start),
                            end: _schedFmtMin(sp.end),
                        });
                    }
                }
            }
        },

        get unifiedEntries() {
            const out = [];
            for (const c of (this.controller.schedules || [])) {
                out.push({
                    key: `c:${c.id}`, owner: 'controller', editable: false, kind: 'span',
                    name: c.name || c.target || window.AquaI18n.t('sched.unnamed_program'), target: c.target || '',
                    group: c.group || '',
                    days: c.days_of_week || 0, enabled: c.enabled !== false,
                    start: _schedHm(c.on_local), end: _schedHm(c.off_local),
                    summary: window.AquaI18n.t('sched.span_summary', { on: c.on_local, off: c.off_local }), raw: c, conflict: false,
                });
            }
            for (const s of (this.schedules || [])) {
                out.push({
                    key: `a:${s.uuid}`, owner: 'app', editable: true, kind: 'point',
                    name: s.name || window.AquaI18n.t('sched.unnamed'), target: this._appTarget(s),
                    days: s.days_of_week || 0, enabled: s.enabled !== false,
                    start: _schedHm(s.time_local), end: null,
                    summary: this.actionSummary(s), raw: s, conflict: false,
                });
            }
            this._annotateConflicts(out);
            out.sort((a, b) => (a.start - b.start) || a.owner.localeCompare(b.owner));
            return out;
        },

        get conflicts() {
            return this.unifiedEntries.filter((e) => e.conflict && e.owner === 'app');
        },

        get controllerPending() { return this.controller.status === 'pending_capture'; },
        get controllerUnsupported() { return this.controller.status === 'unsupported'; },
        // The program group (A/B or a custom label) the listed controller programs
        // belong to. Only the active group is observable on the wire, so this labels
        // what is shown and hints that another group exists on the controller.
        get controllerActiveGroup() { return this.controller.active_group || ''; },
        get hasControllerSchedules() { return (this.controller.schedules || []).length > 0; },

        // --- timeline ------------------------------------------------------

        get timelineRows() {
            const bit = this.timelineDay;
            const active = this.unifiedEntries.filter((e) => (e.days & (1 << bit)) !== 0);
            const groups = new Map();
            for (const e of active) {
                const key = e.target || e.name;
                if (!groups.has(key)) { groups.set(key, { label: key, spans: [], points: [] }); }
                const g = groups.get(key);
                if (e.kind === 'span') { g.spans.push(e); } else { g.points.push(e); }
            }
            return [...groups.values()];
        },

        // The "now" marker is only meaningful when the timeline shows today.
        get showNow() { return this.timelineDay === ((new Date().getDay() + 6) % 7); },
        get nowLeft() {
            const now = new Date();
            const min = (now.getHours() * 60) + now.getMinutes();
            return `${(min / 1440 * 100).toFixed(2)}%`;
        },

        fmtMin(min) { return _schedFmtMin(min); },
        barLeft(e) { return `${(e.start / 1440 * 100).toFixed(2)}%`; },
        barWidth(e) {
            let d = e.end - e.start;
            if (d <= 0) { d += 1440; }
            return `${(d / 1440 * 100).toFixed(2)}%`;
        },
        pointLeft(e) { return `${(e.start / 1440 * 100).toFixed(2)}%`; },
        setView(v) { this.view = v; },
        setTimelineDay(bit) { this.timelineDay = bit; },

        // --- CRUD (app schedules only) -------------------------------------

        newSchedule() { this.form = _schedDefaultForm(); this.editing = 'new'; this.error = ''; this.view = 'list'; },
        editSchedule(s) { this.form = _schedToForm(s); this.editing = s.uuid; this.error = ''; this.view = 'list'; },
        cancel() { this.editing = null; },

        toggleDay(bit) { this.form.days = this.form.days ^ (1 << bit); },
        hasDay(bit) { return (this.form.days & (1 << bit)) !== 0; },

        async save() {
            const payload = _schedFormToPayload(this.form);
            const isNew = this.editing === 'new';
            const url = isNew ? '/api/schedules' : `/api/schedules/${this.editing}`;
            const resp = await fetch(url, {
                method: isNew ? 'POST' : 'PUT',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(payload),
            });
            if (!resp.ok) {
                let detail = '';
                try { detail = await resp.text(); } catch (_) { /* ignore */ }
                this.error = `Save failed (${resp.status})${detail ? ': ' + detail : ''}`;
                return;
            }
            this.editing = null;
            await this.load();
        },

        async toggleEnabled(s) {
            const payload = _schedFormToPayload(_schedToForm(s));
            payload.enabled = !s.enabled;
            await fetch(`/api/schedules/${s.uuid}`, {
                method: 'PUT',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(payload),
            });
            await this.load();
        },

        async remove(s) {
            if (!window.confirm(`Delete schedule "${s.name || s.uuid}"?`)) { return; }
            await fetch(`/api/schedules/${s.uuid}`, { method: 'DELETE' });
            await this.load();
        },

        // --- promote an app edge-pair to a controller program -------------

        // Localize a backend blocker code from the 400 { blockers:[...] } list.
        _promoteBlockerText(code) {
            const t = window.AquaI18n.t;
            switch (code) {
                case 'day_selection_not_expressible': return t('sched.promote_blocker_day_selection_not_expressible');
                case 'target_missing': return t('sched.promote_blocker_target_missing');
                case 'time_invalid': return t('sched.promote_blocker_time_invalid');
                case 'action_not_on_off': return t('sched.promote_blocker_action_not_on_off');
                default: return code;
            }
        },

        async promote(s) {
            this.error = '';
            let resp;
            try {
                resp = await fetch(`/api/schedules/${s.uuid}/promote`, { method: 'POST' });
            } catch (_) {
                this.error = window.AquaI18n.t('sched.promote_failed');
                return;
            }
            if (resp.ok) {
                Alpine.store('toast').show(window.AquaI18n.t('sched.promote_queued'), 'info');
                // The controller program appears once the write is applied and the
                // programs are re-read; refresh shortly to pick it up.
                setTimeout(() => { this.load(); }, 1500);
                return;
            }
            if (resp.status === 400) {
                let blockers = [];
                try { const data = await resp.json(); blockers = Array.isArray(data.blockers) ? data.blockers : []; } catch (_) { /* ignore */ }
                const reasons = blockers.map((b) => this._promoteBlockerText(b)).filter(Boolean).join(' ');
                this.error = window.AquaI18n.t('sched.promote_not_representable', { reasons: reasons || window.AquaI18n.t('sched.promote_reason_unknown') });
                return;
            }
            if (resp.status === 422) {
                this.error = window.AquaI18n.t('sched.promote_no_span');
                return;
            }
            this.error = window.AquaI18n.t('sched.promote_failed');
        },
    };
}
