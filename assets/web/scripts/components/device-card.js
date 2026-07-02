/**
 * Device Card — shared presenter for the Diagnostics "Emulated Devices" and
 * "Actual Devices" sections.
 *
 * ONE component renders every device type and BOTH sections (see the
 * deviceGroups() loop in index.html); the per-type tailoring — icon, role,
 * "what it's doing" summary, the detail rows and the live-activity indicator —
 * lives in the REGISTRY below, keyed by device_type. This is the single source
 * of truth, so the two sections can never drift: they differ only by which
 * device list is bound.
 *
 * The card is a pure presenter over the `dev` object delivered by the
 * /api/diagnostics/{emulated,actual}-devices pollers. It owns no polling and
 * issues no commands. All values are rendered with x-text (auto-escaped).
 */
function deviceCard() {
    const t = (key, params) => window.AquaI18n.t(key, params);
    const yn = (v) => (v ? t('common.yes') : t('common.no'));
    const orDash = (v) => (v === undefined || v === null || v === '' ? '--' : String(v));
    // "Busy" if the state exists and isn't one of the listed idle/terminal states.
    const isBusy = (state, idleStates) => !!state && !idleStates.includes(String(state));

    function fmtAge(secs) {
        if (secs === undefined || secs === null) { return ''; }
        const s = Number(secs);
        if (!Number.isFinite(s)) { return ''; }
        if (s < 60) { return t('time.seconds_ago', { n: s }); }
        if (s < 3600) { return t('time.minutes_ago', { n: Math.floor(s / 60) }); }
        return t('time.hours_ago', { n: Math.floor(s / 3600) });
    }

    // Per-device-type presentation:
    //   summary(d)  -> one-line "what it's doing"
    //   sections(d) -> [{ label, rows: [[key, value], ...] }]
    //   active(d)   -> whether the live-activity dot should pulse
    const REGISTRY = {
        OneTouch: {
            icon: 'onetouch',
            roleKey: 'devcard.role_onetouch',
            summary(d) {
                if (d.spider_engine && isBusy(d.spider_engine.state, ['Idle', 'Complete', 'Done'])) {
                    return t('devcard.surveying_menus', { count: orDash(d.spider_engine.visited_count) });
                }
                if (d.navigator && isBusy(d.navigator.state, ['Idle', 'Synced'])) {
                    return t('devcard.navigating_to', { page: orDash(d.navigator.target_page) });
                }
                if (d.navigator) { return t('devcard.idle_on', { page: orDash(d.navigator.current_page) }); }
                return t('devcard.monitoring_controller');
            },
            sections(d) {
                const out = [];
                if (d.navigator) {
                    out.push({ label: t('devcard.navigator'), rows: [
                        [t('devcard.state'), orDash(d.navigator.state)],
                        [t('devcard.current'), orDash(d.navigator.current_page)],
                        [t('devcard.target'), orDash(d.navigator.target_page)],
                        [t('devcard.cursor'), orDash(d.navigator.cursor_line)],
                        [t('devcard.synced'), yn(d.navigator.synced)],
                    ] });
                }
                if (d.spider_engine) {
                    out.push({ label: t('devcard.spider_engine'), rows: [
                        [t('devcard.state'), orDash(d.spider_engine.state)],
                        [t('devcard.visited'), orDash(d.spider_engine.visited_count)],
                        [t('devcard.target'), orDash(d.spider_engine.current_target)],
                    ] });
                }
                out.push({ label: t('devcard.scraping'), rows: [
                    [t('devcard.stall_counter'), orDash(d.scraping_stall_counter)],
                    [t('devcard.highlighted'), orDash(d.highlighted_line)],
                    [t('devcard.key_cmd'), orDash(d.pending_key_command)],
                    [t('devcard.ack_type'), orDash(d.ack_type)],
                ] });
                return out;
            },
            active(d) {
                return (d.spider_engine && isBusy(d.spider_engine.state, ['Idle', 'Complete', 'Done']))
                    || (d.navigator && isBusy(d.navigator.state, ['Idle', 'Synced']))
                    || d.operating_state === 'Scraping';
            },
        },

        IAQ: {
            icon: 'iaq',
            roleKey: 'devcard.role_iaq',
            summary(d) {
                if (d.awaiting_control_ready) { return t('devcard.waiting_control_ready'); }
                if ((d.command_queue_depth || 0) > 0) { return t('devcard.sending_command', { queue: d.command_queue_depth }); }
                return t('devcard.monitoring_panel');
            },
            sections(d) {
                return [{ label: t('devcard.command'), rows: [
                    [t('devcard.pending_cmd'), orDash(d.pending_command)],
                    [t('devcard.queue_depth'), orDash(d.command_queue_depth)],
                    [t('devcard.awaiting_ctrl_ready'), yn(d.awaiting_control_ready)],
                    [t('devcard.control_data'), orDash(d.control_data_value)],
                ] }];
            },
            active(d) { return d.awaiting_control_ready === true || (d.command_queue_depth || 0) > 0; },
        },

        SerialAdapter: {
            icon: 'serial',
            roleKey: 'devcard.role_serial_adapter',
            summary(d) {
                if (d.has_pending_command) { return t('devcard.dispatching_command'); }
                return t('devcard.decoding_status', { count: orDash(d.status_collection_count) });
            },
            sections(d) {
                return [{ label: t('devcard.decoder'), rows: [
                    [t('devcard.status_types'), orDash(d.status_collection_count)],
                    [t('devcard.status_received'), yn(d.status_message_received)],
                    [t('devcard.pending_cmd'), yn(d.has_pending_command)],
                    [t('devcard.pending_count'), orDash(d.pending_command_count)],
                ] }];
            },
            active(d) { return d.has_pending_command === true; },
        },

        PDA: {
            icon: 'pda',
            roleKey: 'devcard.role_pda',
            summary(d) { return t('devcard.scraping_state', { state: orDash(d.scrape_state) }); },
            sections(d) {
                return [{ label: t('devcard.scraping'), rows: [[t('devcard.scrape_state'), orDash(d.scrape_state)]] }];
            },
            active(d) { return isBusy(d.scrape_state, ['Idle', 'Done', 'Complete']); },
        },

        Keypad: {
            icon: 'keypad',
            roleKey: 'devcard.role_keypad',
            summary(d) {
                return (d.screen && d.screen.page_type) ? t('devcard.showing_page', { page: d.screen.page_type }) : t('devcard.mirroring_keypad');
            },
            sections() { return []; },
            active() { return false; },
        },

        SpasideRemote: {
            icon: 'remote',
            roleKey: 'devcard.role_spaside',
            summary(d) {
                if (d.last_button > 0) { return t('devcard.last_button', { n: d.last_button }); }
                return t('devcard.watching_buttons');
            },
            sections(d) {
                return [{ label: t('devcard.activity'), rows: [
                    [t('devcard.poll_count'), orDash(d.poll_count)],
                    [t('devcard.last_button_k'), orDash(d.last_button)],
                    [t('devcard.last_seen'), d.last_button_age_seconds == null ? '--' : fmtAge(d.last_button_age_seconds)],
                    [t('devcard.leds_seen'), yn(d.led_image_seen)],
                ] }];
            },
            active(d) { return d.last_button_age_seconds != null && d.last_button_age_seconds < 5; },
        },
    };

    const FALLBACK = {
        icon: 'power',
        roleKey: 'devcard.role_device',
        summary() { return ''; },
        sections() { return []; },
        active() { return false; },
    };

    return {
        cfg(d) { return (d && REGISTRY[d.device_type]) || FALLBACK; },
        iconKey(d) { return this.cfg(d).icon; },
        role(d) { return t(this.cfg(d).roleKey); },
        summary(d) { return this.cfg(d).summary(d); },
        sections(d) { return this.cfg(d).sections(d); },
        isActive(d) { return !!this.cfg(d).active(d); },

        // Emulation posture badge: active emulator vs passive decoder vs real device.
        emuLabel(d) {
            if (!d.is_emulated) { return t('devcard.real_device'); }
            return d.emulation_suppressed ? t('devcard.passive_decoder') : t('devcard.active_emulator');
        },
        emuClass(d) {
            if (!d.is_emulated) { return 'real'; }
            return d.emulation_suppressed ? 'passive' : 'active';
        },

        // Mirrors diagnosticsView.operatingStateClass so the card is self-contained.
        stateClass(state) {
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

        // Recent commands, newest first, capped to keep the card compact.
        recentCommands(d) {
            const list = Array.isArray(d.recent_commands) ? d.recent_commands : [];
            return list.slice().reverse().slice(0, 8);
        },
        fmtAge(secs) { return fmtAge(secs); },
        cmdOutcomeClass(outcome) { return outcome === 'Success' ? 'ok' : 'err'; },

        rawJson(d) { return JSON.stringify(d, null, 2); },

        // ---- Compact-card presenters (design: 3-col grid of clickable cards) -------
        // A short one-line status shown as a badge on the card. Real devices are
        // "Passive decoder"; the note beneath spells out the suppression reason.
        cardDotColor(d) { return this.isActive(d) ? 'var(--good)' : 'var(--warn)'; },

        // A device carries a suppression note when a real device shadows an emulator.
        hasSuppressedNote(d) {
            return d && d.is_emulated && d.emulation_suppressed;
        },
        suppressedNote(d) {
            return t('devcard.suppressed_note');
        },

        // Passive snooper note for real non-emulated decoders (design copy).
        hasPassiveNote(d) {
            return d && !d.is_emulated;
        },
        passiveNote() { return t('devcard.passive_note'); },

        // ---- Modal presenters (the rich detail moved off the card, design ~1284) ---
        // Screen present + title/lines for the "Panel Screen" block.
        hasScreen(d) { return !!(d && d.screen && Array.isArray(d.screen.lines) && d.screen.lines.length); },
        screenTitle(d) { return (d && d.screen && d.screen.page_type) || ''; },
        screenLines(d) { return (d && d.screen && Array.isArray(d.screen.lines)) ? d.screen.lines : []; },

        // Highlighted line index for OneTouch cursor rendering in the modal screen.
        isHighlighted(d, idx) {
            return d && d.device_type === 'OneTouch' && d.highlighted_line === idx;
        },

        // Spa-side observed button mapping for the modal (read-only). Groups keys by
        // switch and surfaces the lit indicator LEDs. Takes the plain diagnostics dev.
        hasConfig(d) { return d && d.device_type === 'SpasideRemote'; },
        litIndicators(d) {
            const leds = (d && Array.isArray(d.leds)) ? d.leds : [];
            const out = [];
            leds.forEach((state, i) => { if (state && state !== 'off') { out.push(i + 1); } });
            return out;
        },

        // Raw JSON rendered as key/value rows for the modal's "Raw Diagnostics" block.
        // Flattens one level so nested objects show as compact JSON strings.
        rawRows(d) {
            if (!d || typeof d !== 'object') return [];
            return Object.entries(d).map(([k, v]) => {
                let val;
                if (v === null || v === undefined) { val = '--'; }
                else if (typeof v === 'object') { val = JSON.stringify(v); }
                else { val = String(v); }
                return { k, v: val };
            });
        },
    };
}
