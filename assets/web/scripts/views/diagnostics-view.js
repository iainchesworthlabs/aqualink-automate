/**
 * Diagnostics View — Charts and stats component
 *
 * Chart.js instances and event listeners are stored outside Alpine's
 * reactive proxy to prevent infinite recursion when Chart.js
 * introspects its own proxied properties.
 */

// Private state kept outside Alpine
const _diag = {
    chart: null,
    statsListener: null,
    windowSeconds: 60,
    emuDeviceTimer: null,
    actualDeviceTimer: null,
    recordingTimer: null,
    profilingTimer: null,
    // One-shot guards so a persistently-degraded backend warns once per
    // poller instead of flooding the console on every 2s tick.
    warnedOnce: {}
};

/**
 * Handle a poller fetch failure. 404 / absent endpoints are expected (the
 * feature may not be compiled in) and stay quiet; 5xx or thrown errors mean a
 * degraded backend and warn exactly once per poller key.
 *
 * @param {string} key     poller identifier (for the once-guard)
 * @param {Response|null} resp  the fetch Response (null when fetch threw)
 * @param {Error|null} err  the thrown error, if any
 */
function _handlePollFailure(key, resp, err) {
    // Quiet for a plain 404 / not-found — endpoint simply isn't available.
    if (resp && resp.status === 404) return;
    if (_diag.warnedOnce[key]) return;
    _diag.warnedOnce[key] = true;
    if (resp) {
        console.warn(`[diagnostics] ${key} poll failed: HTTP ${resp.status}`);
    } else {
        console.warn(`[diagnostics] ${key} poll failed:`, err);
    }
}

function diagnosticsView() {
    return {
        windowSeconds: 60,
        // On compact layouts (phone / iPad) the diagnostics page collapses to the
        // mockup's essential subset (System Health, Serial Port Utilization,
        // Bandwidth, Message Errors, Communication Latency). The heavier power-user
        // sections (device list, message statistics, MQTT broker, log levels, serial
        // recording) fold behind this toggle, closed by default. Desktop is
        // unaffected: the wrapper is display:contents and always shown there.
        showAdvanced: false,
        // Getter so the labels re-resolve on a locale switch (duration
        // abbreviations differ per language: "1m" vs "١ د" vs "1分").
        get windowOptions() {
            const t = window.AquaI18n.t;
            return [
                { label: t('time.abbr_seconds', { n: 1 }), value: 1 },
                { label: t('time.abbr_seconds', { n: 5 }), value: 5 },
                { label: t('time.abbr_seconds', { n: 10 }), value: 10 },
                { label: t('time.abbr_seconds', { n: 30 }), value: 30 },
                { label: t('time.abbr_minutes', { n: 1 }), value: 60 },
                { label: t('time.abbr_minutes', { n: 5 }), value: 300 },
                { label: t('common.all'), value: 0 }
            ];
        },

        get chartReady() { return _diag.chart != null; },

        _resolveColor(varName, fallback) {
            const val = getComputedStyle(document.documentElement).getPropertyValue(varName).trim();
            return val || fallback;
        },

        // Bandwidth chart series visibility, toggled from the left-hand legend
        // (the built-in Chart.js legend is disabled). Read = dataset 0 (--accent),
        // Write = dataset 1 (--spa) so the lines match the legend swatches.
        bwRead: true,
        bwWrite: true,
        toggleBw(which) {
            if (which === 'read') this.bwRead = !this.bwRead;
            else this.bwWrite = !this.bwWrite;
            const ch = _diag.chart;
            if (!ch) return;
            const idx = which === 'read' ? 0 : 1;
            ch.setDatasetVisibility(idx, which === 'read' ? this.bwRead : this.bwWrite);
            ch.update();
        },

        // Section visibility. The design shows flat, always-visible cards (no
        // accordions), so every panel defaults open; the toggle headers are
        // neutered to plain section titles in CSS (.section-toggle).
        showSerialHealth: true,
        showMessageErrors: true,
        showMessageStats: true,
        showLogLevels: true,
        showDeviceStatus: true,
        showEmulatedDevices: true,
        showActualDevices: true,
        showRecording: true,
        showProfiling: false,
        showMqtt: true,
        showMatter: false,

        // MQTT broker status diagnostics
        mqtt: { enabled: false },

        // System health (readiness + subsystem checks) from /api/health/detailed.
        health: { ready: false, status: '', uptime_seconds: 0, checks: {} },

        // Matter bridge status diagnostics (sidecar status + commissioning QR)
        matter: { enabled: false },

        // Serial recording control state
        recording: { recording: false, file: '', bytes: 0 },
        recordingFilename: 'capture.cap',
        recordingBusy: false,

        // Finished captures sitting in the server's capture directory, so a
        // capture can be downloaded without shell access to the host. Fetched on
        // demand (entering the view, after a stop, and on Refresh) rather than
        // polled — a directory listing is not live data.
        captures: [],
        capturesBusy: false,
        downloadingCapture: '',

        // Profiler control state (Tracy / Intel VTune / AMD uProf)
        profiling: { enabled: false, running: false, backend: '', available: [] },
        profilingBusy: false,

        // Spa-side remotes (Dual Spa Switch / Spa Link). Each remote carries a per-key `buttons`
        // array (wire press index + controller switch:button coordinate + live/requested function);
        // the keypad is rendered on the device card via spasideForDevice(). Press injection works on
        // emulated remotes; per-key programming works on any remote whose key mapping is decoded.
        spasideRemotes: [],
        spasideBusy: false,

        // The functions a connected controller can assign to a button (the strict chooser's options),
        // unioned server-side with any function already in use. [] when no controller can program.
        spasideAvailableFunctions: [],

        // Which key's inline function editor is open, as "<address>:<index>" (only one at a time).
        spasideEditKey: null,

        // Emulated device diagnostics
        emulatedDevices: [],

        // Actual (real, non-emulated) device diagnostics
        actualDevices: [],

        // Log level control state
        logChannels: {},
        severityLevels: [],
        globalLevel: '',
        logLevelsLoaded: false,

        // --- Modal state (design: device-detail, message-stats, log-levels) ----------
        // Device-detail modal. selectedDeviceId + selectedDeviceGroup let the modal
        // re-read the live device object from the polled lists each render, so the
        // open modal keeps updating as new poll data arrives (rather than freezing a
        // snapshot taken at click time).
        modalOpen: false,
        selectedDeviceId: null,
        selectedDeviceGroup: null,

        // Message-statistics modal (search over the full type list).
        msgModalOpen: false,
        msgSearch: '',

        // Log-levels modal (per-channel search + all/overrides filter).
        logModalOpen: false,
        logSearch: '',
        logFilter: 'all',      // 'all' | 'overrides'

        // Escape closes whichever modal is open. Registered once in initChart().
        _onKeydown: null,

        async fetchHealth() {
            try {
                const resp = await fetch('/api/health/detailed');
                if (!resp.ok) return;
                this.health = await resp.json();
            } catch (_) { /* offline / auth: keep last */ }
        },
        _fmtUptime(s) {
            s = Math.max(0, Math.floor(s || 0));
            const t = window.AquaI18n.t;
            const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
            if (d) return t('time.days_hours', { d, h });
            if (h) return t('time.hours_minutes', { h, m });
            return t('time.minutes', { m });
        },

        initChart() {
            Alpine.store('ws').connectStats();

            if (!_diag.chart) {
                this.$nextTick(() => this._createChart());
            }

            this._fetchLogLevels();
            this.fetchEmulatedDevices();
            this.fetchActualDevices();
            this.fetchRecordingStatus();
            this.fetchCaptures();
            this.fetchMqtt();
            this.fetchHealth();
            if (!_diag.healthTimer) {
                _diag.healthTimer = setInterval(() => this.fetchHealth(), 2000);
            }
            // Guard against a leaked interval if initChart() runs again before destroyChart().
            if (!_diag.emuDeviceTimer) {
                _diag.emuDeviceTimer = setInterval(() => this.fetchEmulatedDevices(), 2000);
            }
            if (!_diag.actualDeviceTimer) {
                _diag.actualDeviceTimer = setInterval(() => this.fetchActualDevices(), 2000);
            }
            if (!_diag.recordingTimer) {
                _diag.recordingTimer = setInterval(() => this.fetchRecordingStatus(), 2000);
            }
            this.fetchSpasideRemotes();
            if (!_diag.spasideTimer) {
                _diag.spasideTimer = setInterval(() => this.fetchSpasideRemotes(), 2000);
            }
            if (!_diag.mqttTimer) {
                _diag.mqttTimer = setInterval(() => this.fetchMqtt(), 2000);
            }

            // Escape closes any open diagnostics modal.
            if (!this._onKeydown) {
                this._onKeydown = (e) => {
                    if (e.key !== 'Escape') return;
                    if (this.modalOpen) { this.closeModal(); }
                    else if (this.msgModalOpen) { this.closeMsgModal(); }
                    else if (this.logModalOpen) { this.closeLogModal(); }
                };
                window.addEventListener('keydown', this._onKeydown);
            }
        },

        _createChart() {
            const ctx = this.$refs.utilizationChart;
            if (!ctx || typeof Chart === 'undefined') return;

            const textColor = this._resolveColor('--text-secondary', '#94a3b8');
            const gridColor = this._resolveColor('--grid-color', 'rgba(148,163,184,0.15)');
            const c = _bwColorSet();

            _diag.chart = new Chart(ctx, {
                type: 'line',
                data: {
                    datasets: [
                        {
                            label: 'Read %',
                            data: [],
                            borderColor: c.read,
                            backgroundColor: c.readFill,
                            borderWidth: 2,
                            tension: 0.4,
                            fill: true,
                            pointRadius: 0
                        },
                        {
                            label: 'Write %',
                            data: [],
                            borderColor: c.write,
                            backgroundColor: c.writeFill,
                            borderWidth: 2,
                            tension: 0.4,
                            fill: true,
                            pointRadius: 0
                        }
                    ]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    interaction: { mode: 'index', intersect: false },
                    scales: {
                        x: {
                            type: 'time',
                            time: {
                                displayFormats: {
                                    second: 'HH:mm:ss',
                                    minute: 'HH:mm',
                                    hour: 'HH:mm'
                                }
                            },
                            // Chart.js's own displayFormats always renders Latin digits
                            // (its date adapter ignores the app's active locale); override
                            // the tick label with the locale-aware time formatter so the
                            // axis matches the rest of the page (e.g. Arabic-Indic digits).
                            ticks: {
                                maxRotation: 0, autoSkip: true, maxTicksLimit: 8, color: textColor,
                                callback: (value) => window.AquaI18n.formatTime(value)
                            },
                            grid: { color: gridColor }
                        },
                        y: {
                            beginAtZero: true,
                            max: 100,
                            ticks: {
                                callback: v => window.AquaI18n.formatNumber(v) + '%',
                                color: textColor
                            },
                            grid: { color: gridColor }
                        }
                    },
                    plugins: {
                        // Built-in legend disabled — the left-hand .bw-legend chips
                        // are the interactive legend (see toggleBw()).
                        legend: { display: false },
                        tooltip: { callbacks: { label: item => item.dataset.label + ': ' + window.AquaI18n.formatNumber(item.parsed.y, { minimumFractionDigits: 2, maximumFractionDigits: 2 }) + '%' } }
                    },
                    animation: { duration: 0 }
                }
            });

            // Read windowSeconds directly from _diag — no Alpine proxy involved
            _diag.statsListener = () => {
                _updateChartData(_diag.windowSeconds);
            };
            window.addEventListener('stats-updated', _diag.statsListener);

            // Apply initial data
            _updateChartData(_diag.windowSeconds);
        },

        setWindow(seconds) {
            this.windowSeconds = seconds;
            _diag.windowSeconds = seconds;
            _updateChartData(seconds);
        },

        destroyChart() {
            Alpine.store('ws').disconnectStats();

            if (_diag.emuDeviceTimer) {
                clearInterval(_diag.emuDeviceTimer);
                _diag.emuDeviceTimer = null;
            }

            if (_diag.actualDeviceTimer) {
                clearInterval(_diag.actualDeviceTimer);
                _diag.actualDeviceTimer = null;
            }

            if (_diag.recordingTimer) {
                clearInterval(_diag.recordingTimer);
                _diag.recordingTimer = null;
            }

            if (_diag.profilingTimer) {
                clearInterval(_diag.profilingTimer);
                _diag.profilingTimer = null;
            }

            if (_diag.spasideTimer) {
                clearInterval(_diag.spasideTimer);
                _diag.spasideTimer = null;
            }

            if (_diag.mqttTimer) {
                clearInterval(_diag.mqttTimer);
                _diag.mqttTimer = null;
            }

            if (_diag.healthTimer) {
                clearInterval(_diag.healthTimer);
                _diag.healthTimer = null;
            }

            if (_diag.matterTimer) {
                clearInterval(_diag.matterTimer);
                _diag.matterTimer = null;
            }

            if (_diag.statsListener) {
                window.removeEventListener('stats-updated', _diag.statsListener);
                _diag.statsListener = null;
            }

            if (_diag.chart) {
                _diag.chart.destroy();
                _diag.chart = null;
            }

            if (this._onKeydown) {
                window.removeEventListener('keydown', this._onKeydown);
                this._onKeydown = null;
            }
            // Leaving the view drops any open modal so it doesn't reappear on return.
            this.modalOpen = false;
            this.msgModalOpen = false;
            this.logModalOpen = false;
        },

        async fetchEmulatedDevices() {
            try {
                const resp = await fetch('/api/diagnostics/emulated-devices');
                if (!resp.ok) { _handlePollFailure('emulated-devices', resp, null); return; }
                this.emulatedDevices = await resp.json();
                _diag.warnedOnce['emulated-devices'] = false;
            } catch (e) {
                _handlePollFailure('emulated-devices', null, e);
            }
        },

        async fetchActualDevices() {
            try {
                const resp = await fetch('/api/diagnostics/actual-devices');
                if (!resp.ok) { _handlePollFailure('actual-devices', resp, null); return; }
                this.actualDevices = await resp.json();
                _diag.warnedOnce['actual-devices'] = false;
            } catch (e) {
                _handlePollFailure('actual-devices', null, e);
            }
        },

        async fetchRecordingStatus() {
            try {
                const resp = await fetch('/api/diagnostics/recording');
                if (!resp.ok) { _handlePollFailure('recording', resp, null); return; }
                this.recording = await resp.json();
                _diag.warnedOnce['recording'] = false;
            } catch (e) {
                _handlePollFailure('recording', null, e);
            }
        },

        async fetchCaptures() {
            if (this.capturesBusy) return;
            this.capturesBusy = true;
            try {
                const resp = await fetch('/api/diagnostics/recording/captures');
                if (!resp.ok) { _handlePollFailure('captures', resp, null); return; }
                const data = await resp.json();
                this.captures = Array.isArray(data.captures) ? data.captures : [];
                _diag.warnedOnce['captures'] = false;
            } catch (e) {
                _handlePollFailure('captures', null, e);
            } finally {
                this.capturesBusy = false;
            }
        },

        /**
         * Download one capture.
         *
         * Goes through fetch (not a plain <a href>) for two reasons: the auth
         * wrapper attaches the bearer token to fetch only, and the ingress shim
         * rebases fetch URLs onto Home Assistant's per-session path prefix. The
         * response is turned into a blob and handed to a synthetic anchor so the
         * browser saves it under the capture's own name.
         */
        async downloadCapture(name) {
            if (!name || this.downloadingCapture) return;
            this.downloadingCapture = name;
            let objectUrl = '';
            try {
                const resp = await fetch('/api/diagnostics/recording/captures/' + encodeURIComponent(name));
                if (!resp.ok) {
                    const data = await resp.json().catch(() => ({}));
                    Alpine.store('toast').show(window.AquaI18n.apiError(data, window.AquaI18n.t('toast.capture_download_failed')), 'error');
                    return;
                }

                const blob = await resp.blob();
                objectUrl = URL.createObjectURL(blob);

                const anchor = document.createElement('a');
                anchor.href = objectUrl;
                anchor.download = name;
                document.body.appendChild(anchor);
                anchor.click();
                anchor.remove();
            } catch (e) {
                Alpine.store('toast').show(window.AquaI18n.t('toast.capture_download_failed'), 'error');
            } finally {
                if (objectUrl) { URL.revokeObjectURL(objectUrl); }
                this.downloadingCapture = '';
            }
        },

        async fetchProfilingStatus() {
            try {
                const resp = await fetch('/api/diagnostics/profiling');
                if (!resp.ok) { _handlePollFailure('profiling', resp, null); return; }
                this.profiling = await resp.json();
                _diag.warnedOnce['profiling'] = false;
            } catch (e) {
                _handlePollFailure('profiling', null, e);
            }
        },

        async fetchSpasideRemotes() {
            try {
                const resp = await fetch('/api/equipment/spaside-remotes');
                if (!resp.ok) { _handlePollFailure('spaside-remotes', resp, null); return; }
                const data = await resp.json();
                this._applySpasideData(data);
                _diag.warnedOnce['spaside-remotes'] = false;
            } catch (e) {
                _handlePollFailure('spaside-remotes', null, e);
            }
        },

        // Apply a spaside-remotes envelope (GET poll or a press/assign POST response) to view state.
        _applySpasideData(data) {
            this.spasideRemotes = (data && Array.isArray(data.remotes)) ? data.remotes : [];
            this.spasideAvailableFunctions = (data && Array.isArray(data.available_functions)) ? data.available_functions : [];
        },

        // The rich spaside remote matching a device card, by bus address. The diagnostics device
        // card is keyed by device_id ("0x10"); the equipment endpoint keys remotes by address in the
        // same form, so a direct match joins the two feeds. Returns null when no match (e.g. the
        // equipment controller isn't registered) -> the card simply omits the keypad.
        spasideForDevice(dev) {
            if (!dev || dev.device_type !== 'SpasideRemote') return null;
            return this.spasideRemotes.find(r => r.address === dev.device_id) || null;
        },

        // Inject a momentary press of `button` (wire index) on the emulated remote at `address`.
        // No-op for a real (observed) remote -- the server rejects it and we surface the reason.
        async pressSpasideButton(address, button) {
            if (this.spasideBusy) return;
            this.spasideBusy = true;
            try {
                // The list reports address as a hex string ("0x20") for display; the API expects a
                // numeric byte. parseInt auto-detects the 0x prefix.
                const addr = (typeof address === 'string') ? parseInt(address, 16) : address;
                const resp = await fetch('/api/equipment/spaside-remotes', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ action: 'press', address: addr, button: button })
                });
                const data = await resp.json().catch(() => ({}));
                if (resp.ok) {
                    this._applySpasideData(data);
                    Alpine.store('toast').show(window.AquaI18n.t('toast.spaside_pressed', { n: button }), 'info');
                } else {
                    Alpine.store('toast').show(window.AquaI18n.apiError(data, window.AquaI18n.t('toast.spaside_press_failed')), 'error');
                }
            } catch (e) {
                Alpine.store('toast').show(window.AquaI18n.t('toast.spaside_press_failed'), 'error');
            } finally {
                this.spasideBusy = false;
            }
        },

        // Toggle the inline function editor for one key. Identified by "<address>:<index>" so only
        // one editor is open at a time across all remote cards.
        spasideEditId(address, index) { return address + ':' + index; },
        toggleSpasideEdit(address, index) {
            const id = this.spasideEditId(address, index);
            this.spasideEditKey = (this.spasideEditKey === id) ? null : id;
        },

        // Program a key's function over the bus by its controller config switch:button coordinate
        // (drives the controller's Spa Switch / Spa Remotes config). Takes effect on whichever
        // controller can write it; the live function on the key refreshes once it re-reports. `fn`
        // comes straight from the strict dropdown of available functions, so no validation here.
        async setSpasideAssignment(sw, btn, fn) {
            if (this.spasideBusy) return;
            sw = parseInt(sw, 10);
            btn = parseInt(btn, 10);
            fn = (fn || '').trim();
            if (!Number.isInteger(sw) || sw < 1 || !Number.isInteger(btn) || btn < 1 || fn === '') {
                Alpine.store('toast').show(window.AquaI18n.t('toast.spaside_pick_function'), 'error');
                return;
            }
            this.spasideBusy = true;
            try {
                const resp = await fetch('/api/equipment/spaside-remotes', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ action: 'assign', switch: sw, button: btn, function: fn })
                });
                const data = await resp.json().catch(() => ({}));
                if (resp.ok) {
                    this._applySpasideData(data);
                    this.spasideEditKey = null;
                    Alpine.store('toast').show(window.AquaI18n.t('toast.spaside_programming', { sw, btn, fn }), 'info');
                } else {
                    Alpine.store('toast').show(window.AquaI18n.apiError(data, window.AquaI18n.t('toast.spaside_program_failed')), 'error');
                }
            } catch (e) {
                Alpine.store('toast').show(window.AquaI18n.t('toast.spaside_program_failed'), 'error');
            } finally {
                this.spasideBusy = false;
            }
        },

        // Map an indicator state to an existing badge class (green=on, amber=blink, grey=off).
        spasideLedBadgeClass(state) {
            if (state === 'on') return 'badge-freq';
            if (state === 'blink') return 'badge-status-warn';
            return 'badge-status-off';
        },

        // Group a device card's keys for the keypad. A Dual Spa Switch (6588 board) bridges two
        // physical switches, so the backend tags keys 1-4 with switch 2 and keys 5-8 with switch 3;
        // group by that switch number with a heading. Keys with no decoded mapping (assignable=false)
        // fall into a single unlabelled group. Driven entirely by backend `buttons` data (no protocol
        // knowledge here). Takes the diagnostics `dev` and joins to the rich remote by address.
        spasideKeyGroups(dev) {
            const remote = this.spasideForDevice(dev);
            const buttons = (remote && Array.isArray(remote.buttons)) ? remote.buttons : [];
            const groups = [];
            const bySwitch = new Map();
            for (const b of buttons) {
                const key = b.assignable ? window.AquaI18n.t('diag.switch_n', { n: b.switch }) : '';
                if (!bySwitch.has(key)) {
                    const g = { label: key, buttons: [] };
                    bySwitch.set(key, g);
                    groups.push(g);
                }
                bySwitch.get(key).buttons.push(b);
            }
            return groups;
        },

        async fetchMqtt() {
            try {
                const resp = await fetch('/api/diagnostics/mqtt');
                if (!resp.ok) { _handlePollFailure('mqtt', resp, null); return; }
                this.mqtt = await resp.json();
                _diag.warnedOnce['mqtt'] = false;
            } catch (e) {
                _handlePollFailure('mqtt', null, e);
            }
        },

        async fetchMatter() {
            try {
                const resp = await fetch('/api/diagnostics/matter');
                if (!resp.ok) { _handlePollFailure('matter', resp, null); return; }
                this.matter = await resp.json();
                _diag.warnedOnce['matter'] = false;
                this.$nextTick(() => this._renderMatterQr());
            } catch (e) {
                _handlePollFailure('matter', null, e);
            }
        },

        // Render the commissioning QR into the panel canvas when a QR library is
        // vendored (window.QRCode, davidshimjs/qrcodejs). Without one we still show the
        // manual pairing code + QR payload text, which pair every ecosystem.
        _renderMatterQr() {
            const payload = this.matter && this.matter.qr_payload;
            const el = this.$refs && this.$refs.matterQr;
            if (!el) return;
            if (!payload || typeof window.QRCode === 'undefined') {
                el.innerHTML = '';
                return;
            }
            if (_diag.matterQrPayload === payload && el.childElementCount > 0) return;
            _diag.matterQrPayload = payload;
            el.innerHTML = '';
            try {
                // eslint-disable-next-line no-new
                new window.QRCode(el, { text: payload, width: 200, height: 200, correctLevel: window.QRCode.CorrectLevel.M });
            } catch (e) {
                _handlePollFailure('matter-qr', null, e);
            }
        },

        async startRecording() {
            if (this.recordingBusy || !this.recordingFilename) return;
            this.recordingBusy = true;
            try {
                const resp = await fetch('/api/diagnostics/recording', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ action: 'start', filename: this.recordingFilename })
                });
                const data = await resp.json().catch(() => ({}));
                if (resp.ok) {
                    this.recording = data;
                    Alpine.store('toast').show(window.AquaI18n.t('toast.recording_started'), 'info');
                } else {
                    Alpine.store('toast').show(window.AquaI18n.apiError(data, window.AquaI18n.t('toast.recording_start_failed')), 'error');
                }
            } catch (e) {
                Alpine.store('toast').show(window.AquaI18n.t('toast.recording_start_failed'), 'error');
            } finally {
                this.recordingBusy = false;
            }
        },

        async stopRecording() {
            if (this.recordingBusy) return;
            this.recordingBusy = true;
            try {
                const resp = await fetch('/api/diagnostics/recording', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ action: 'stop' })
                });
                const data = await resp.json().catch(() => ({}));
                if (resp.ok) {
                    this.recording = data;
                    Alpine.store('toast').show(window.AquaI18n.t('toast.recording_stopped'), 'info');
                    // The capture just became downloadable — surface it immediately.
                    this.fetchCaptures();
                } else {
                    Alpine.store('toast').show(window.AquaI18n.apiError(data, window.AquaI18n.t('toast.recording_stop_failed')), 'error');
                }
            } catch (e) {
                Alpine.store('toast').show(window.AquaI18n.t('toast.recording_stop_failed'), 'error');
            } finally {
                this.recordingBusy = false;
            }
        },

        async _postProfiling(payload, okMessage, failMessage) {
            if (this.profilingBusy) return;
            this.profilingBusy = true;
            try {
                const resp = await fetch('/api/diagnostics/profiling', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(payload)
                });
                const data = await resp.json().catch(() => ({}));
                if (resp.ok) {
                    this.profiling = data;
                    Alpine.store('toast').show(okMessage, 'info');
                } else {
                    Alpine.store('toast').show(window.AquaI18n.apiError(data, failMessage), 'error');
                }
            } catch (e) {
                Alpine.store('toast').show(failMessage, 'error');
            } finally {
                this.profilingBusy = false;
            }
        },

        async startProfiling() {
            await this._postProfiling({ action: 'start' }, window.AquaI18n.t('toast.profiling_resumed'), window.AquaI18n.t('toast.profiling_resume_failed'));
        },

        async stopProfiling() {
            await this._postProfiling({ action: 'stop' }, window.AquaI18n.t('toast.profiling_paused'), window.AquaI18n.t('toast.profiling_pause_failed'));
        },

        async selectProfilingBackend(backend) {
            if (!backend) return;
            await this._postProfiling({ action: 'select', backend }, window.AquaI18n.t('toast.profiling_backend_set', { backend }), window.AquaI18n.t('toast.profiling_backend_failed'));
        },

        formatBytes(bytes) {
            const n = window.AquaI18n.formatNumber;
            if (!bytes || bytes === 0) return n(0) + ' B';
            const k = 1024;
            const sizes = ['B', 'KB', 'MB', 'GB'];
            const i = Math.floor(Math.log(bytes) / Math.log(k));
            return n(parseFloat((bytes / Math.pow(k, i)).toFixed(2))) + ' ' + sizes[i];
        },

        // Capture timestamps arrive as seconds since the Unix epoch; render them in
        // the active UI locale (not the browser's).
        formatCaptureTime(unixSeconds) {
            if (!unixSeconds) return '--';
            return window.AquaI18n.formatDateTime(unixSeconds * 1000);
        },

        formatMicros(us) {
            const n = window.AquaI18n.formatNumber;
            const t = window.AquaI18n.t;
            if (us == null) return '--';
            if (us < 1000) return t('diag.unit_microseconds', { n: n(us, { maximumFractionDigits: 0 }) });
            if (us < 1000000) return t('diag.unit_milliseconds', { n: n(us / 1000, { minimumFractionDigits: 2, maximumFractionDigits: 2 }) });
            return t('diag.unit_seconds', { n: n(us / 1000000, { minimumFractionDigits: 2, maximumFractionDigits: 2 }) });
        },

        utilColor(val) {
            const v = parseFloat(val) || 0;
            if (v < 50) return 'var(--gauge-good)';
            if (v < 80) return 'var(--gauge-warn)';
            return 'var(--gauge-bad)';
        },

        // Maps a device operating_state to a badge severity class.
        // NOTE: these string values are C++ enum names emitted verbatim by
        // magic_enum::enum_name() over OperatingStates in DescribeDiagnostics().
        // They form a cross-boundary contract — keep in sync if the C++ enum
        // names ever change.
        operatingStateClass(state) {
            switch (state) {
                case 'NormalOperation':
                case 'Scraping':
                    return 'badge-status-normal';
                case 'StartUp':
                case 'ColdStart':
                    return 'badge-status-warn';
                case 'FaultHasOccurred':
                case 'ScrapingFaulted':
                    return 'badge-status-danger';
                default:
                    return '';
            }
        },

        // The two device sections (Emulated + Actual) render through ONE shared card
        // definition: the template iterates these groups, so there is a single source of
        // truth for the card markup, differing only by the bound device list. `openKey`
        // names the existing collapse flag on this view so each section's toggle persists.
        deviceGroups() {
            const t = window.AquaI18n.t;
            return [
                { key: 'emulated', title: t('diag.emulated_devices'), openKey: 'showEmulatedDevices', empty: t('diag.no_emulated_devices'), devices: this.emulatedDevices },
                { key: 'actual', title: t('diag.actual_devices'), openKey: 'showActualDevices', empty: t('diag.no_actual_devices'), devices: this.actualDevices }
            ];
        },

        formatUptime(secs) {
            if (secs == null) return '--';
            const t = window.AquaI18n.t;
            const h = Math.floor(secs / 3600);
            const m = Math.floor((secs % 3600) / 60);
            if (h > 0) return t('time.hours_minutes', { h, m });
            return t('time.minutes', { m });
        },

        async _fetchLogLevels() {
            try {
                const resp = await fetch('/api/diagnostics/logging');
                if (!resp.ok) { _handlePollFailure('logging', resp, null); return; }
                const data = await resp.json();
                this.logChannels = data.channels || {};
                this.severityLevels = data.severity_levels || [];
                this.globalLevel = this._computeGlobalLevel();
                this.logLevelsLoaded = true;
                _diag.warnedOnce['logging'] = false;
            } catch (e) {
                _handlePollFailure('logging', null, e);
            }
        },

        // Derive the global indicator: the shared level when every channel
        // agrees, otherwise '' (Mixed). Guards against an empty channel map
        // ([].every() is true, which would otherwise yield undefined).
        _computeGlobalLevel() {
            const values = Object.values(this.logChannels);
            return values.length > 0 && values.every(v => v === values[0]) ? values[0] : '';
        },

        async setChannelLevel(channel, level) {
            const previous = this.logChannels[channel];
            this.logChannels[channel] = level;            // optimistic
            this.globalLevel = this._computeGlobalLevel();
            try {
                const resp = await fetch('/api/diagnostics/logging', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ channel, level })
                });
                if (!resp.ok) {
                    throw new Error(`HTTP ${resp.status}`);
                }
            } catch (e) {
                // Revert the optimistic mutation and surface the failure.
                if (previous === undefined) {
                    delete this.logChannels[channel];
                } else {
                    this.logChannels[channel] = previous;
                }
                this.globalLevel = this._computeGlobalLevel();
                console.error(`[diagnostics] failed to set log level for ${channel}:`, e);
                Alpine.store('toast').show(window.AquaI18n.t('toast.log_level_failed', { channel }), 'error');
            }
        },

        async setGlobalLevel(level) {
            const previous = { ...this.logChannels };
            const previousGlobal = this.globalLevel;
            this.globalLevel = level;                     // optimistic
            for (const ch in this.logChannels) {
                this.logChannels[ch] = level;
            }
            try {
                const resp = await fetch('/api/diagnostics/logging', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ global: level })
                });
                if (!resp.ok) {
                    throw new Error(`HTTP ${resp.status}`);
                }
            } catch (e) {
                // Revert the optimistic mutation and surface the failure.
                this.logChannels = previous;
                this.globalLevel = previousGlobal;
                console.error('[diagnostics] failed to set global log level:', e);
                Alpine.store('toast').show(window.AquaI18n.t('toast.log_global_failed'), 'error');
            }
        },

        // ---- Device-detail modal --------------------------------------------------
        // Open on a card click. We store id + group, then re-resolve the live object
        // each render so the modal tracks the 2s poll.
        openDeviceModal(group, dev) {
            this.selectedDeviceGroup = group;
            this.selectedDeviceId = dev && dev.device_id;
            this.modalOpen = true;
        },
        closeModal() {
            this.modalOpen = false;
            this.selectedDeviceId = null;
            this.selectedDeviceGroup = null;
        },
        // Live device object behind the open modal (or null once it disappears).
        selectedDevice() {
            if (!this.modalOpen || !this.selectedDeviceId) return null;
            const list = this.selectedDeviceGroup === 'emulated' ? this.emulatedDevices : this.actualDevices;
            return (Array.isArray(list) ? list : []).find(d => d.device_id === this.selectedDeviceId) || null;
        },

        // ---- Message-statistics modal ---------------------------------------------
        openMsgModal() { this.msgSearch = ''; this.msgModalOpen = true; },
        closeMsgModal() { this.msgModalOpen = false; },

        // Message rows shaped for the card/modal templates. Sorted by count desc so the
        // card's "top 6" is meaningful. `_msgRows()` is the shared source.
        _msgRows() {
            const rows = Array.isArray(this.$store.stats.messageCounts) ? this.$store.stats.messageCounts : [];
            return rows.slice().sort((a, b) => (b.count || 0) - (a.count || 0));
        },
        get msgTypeCount() { return this._msgRows().length; },
        msgStatsTop() { return this._msgRows().slice(0, 6); },
        msgStatsFiltered() {
            const q = (this.msgSearch || '').trim().toLowerCase();
            const rows = this._msgRows();
            if (!q) return rows;
            return rows.filter(m => (m.name || ('ID ' + m.id)).toLowerCase().includes(q));
        },
        msgLabel(m) { return m.name || window.AquaI18n.t('diag.msg_id', { id: m.id }); },
        msgRate(m) { return window.AquaI18n.t('diag.rate_per_s', { value: window.AquaI18n.formatNumber(m.frequency || 0, { minimumFractionDigits: 2, maximumFractionDigits: 2 }) }); },
        msgLastSeen(m) { return m.lastSeen ? window.AquaI18n.formatTime(m.lastSeen) : '--'; },

        // ---- Log-levels modal -----------------------------------------------------
        openLogModal() { this.logSearch = ''; this.logFilter = 'all'; this.logModalOpen = true; },
        closeLogModal() { this.logModalOpen = false; },

        // Channels whose level differs from the (majority) global level = overrides.
        logOverrides() {
            const g = this.globalLevel;
            // When globalLevel is '' (mixed), the majority is ambiguous; treat the
            // most common level as the baseline so overrides stay meaningful.
            const base = g || this._majorityLevel();
            return Object.entries(this.logChannels)
                .filter(([, lvl]) => lvl !== base)
                .sort((a, b) => a[0].localeCompare(b[0]));
        },
        get logOverrideCount() { return this.logOverrides().length; },
        get logHasOverrides() { return this.logOverrideCount > 0; },
        _majorityLevel() {
            const counts = {};
            for (const lvl of Object.values(this.logChannels)) { counts[lvl] = (counts[lvl] || 0) + 1; }
            let best = '', n = -1;
            for (const [lvl, c] of Object.entries(counts)) { if (c > n) { n = c; best = lvl; } }
            return best;
        },
        get logGlobalLabel() { return this.globalLevel || window.AquaI18n.t('diag.mixed'); },
        // A channel is "overridden" if it differs from the baseline level.
        logIsOverride(ch) {
            const base = this.globalLevel || this._majorityLevel();
            return this.logChannels[ch] !== base;
        },
        // Filtered + sorted channel list for the modal table.
        logFilteredChannels() {
            const q = (this.logSearch || '').trim().toLowerCase();
            let entries = Object.entries(this.logChannels).sort((a, b) => a[0].localeCompare(b[0]));
            if (this.logFilter === 'overrides') { entries = entries.filter(([ch]) => this.logIsOverride(ch)); }
            if (q) { entries = entries.filter(([ch]) => ch.toLowerCase().includes(q)); }
            return entries;
        },
        // Reset one channel (and all channels) back to the baseline global level.
        resetChannelLevel(ch) {
            const base = this.globalLevel || this._majorityLevel();
            if (base) { this.setChannelLevel(ch, base); }
        },
        async resetAllLogs() {
            const base = this.globalLevel || this._majorityLevel();
            if (base) { await this.setGlobalLevel(base); }
        },

        // A dot colour per severity, for the log summary/modal indicators.
        logLevelColor(lvl) {
            switch (lvl) {
                case 'Trace':
                case 'Debug': return 'var(--text-faint)';
                case 'Info': return 'var(--good)';
                case 'Notify': return 'var(--accent)';
                case 'Warning': return 'var(--warn)';
                case 'Error':
                case 'Fatal': return 'var(--bad)';
                default: return 'var(--text-dim)';
            }
        },

        stopProp(e) { if (e) e.stopPropagation(); }
    };
}

// Standalone function — no Alpine proxy involvement
// Resolve the bandwidth series colors from CSS tokens so the chart lines match
// the left-hand legend swatches (--accent = Read, --spa = Write) and re-track
// live theme/accent changes. Fills are a translucent mix of the same colour.
function _bwColorSet() {
    const cs = getComputedStyle(document.documentElement);
    const read = cs.getPropertyValue('--accent').trim() || '#10b981';
    const write = cs.getPropertyValue('--spa').trim() || '#3b82f6';
    return {
        read,
        write,
        readFill: `color-mix(in srgb, ${read} 14%, transparent)`,
        writeFill: `color-mix(in srgb, ${write} 14%, transparent)`
    };
}

function _updateChartData(windowSeconds) {
    if (!_diag.chart) return;

    const h = window.__statsChartHistory;
    if (!h) return;

    // Keep the line colours in sync with the current theme/accent tokens.
    const c = _bwColorSet();
    const d0 = _diag.chart.data.datasets[0];
    const d1 = _diag.chart.data.datasets[1];
    if (d0) { d0.borderColor = c.read; d0.backgroundColor = c.readFill; }
    if (d1) { d1.borderColor = c.write; d1.backgroundColor = c.writeFill; }

    const now = Date.now();
    let startIdx = 0;

    if (windowSeconds > 0 && h.times.length > 0) {
        const cutoff = now - windowSeconds * 1000;
        startIdx = h.times.length;
        for (let i = 0; i < h.times.length; i++) {
            if (h.times[i] >= cutoff) { startIdx = i; break; }
        }
    }

    // Build {x,y} point arrays so each point carries its own timestamp
    const slicedTimes = h.times.slice(startIdx);
    const slicedReads = h.reads.slice(startIdx);
    const slicedWrites = h.writes.slice(startIdx);

    _diag.chart.data.labels = undefined;
    _diag.chart.data.datasets[0].data = slicedTimes.map((t, i) => ({ x: t, y: slicedReads[i] }));
    _diag.chart.data.datasets[1].data = slicedTimes.map((t, i) => ({ x: t, y: slicedWrites[i] }));

    // Set explicit axis range so the chart always shows the full selected window
    const xAxis = _diag.chart.options.scales.x;
    if (windowSeconds > 0) {
        xAxis.min = now - windowSeconds * 1000;
        xAxis.max = now;
    } else {
        xAxis.min = undefined;
        xAxis.max = undefined;
    }

    _diag.chart.update('none');
}
