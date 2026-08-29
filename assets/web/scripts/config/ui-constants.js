/**
 * UI Constants — single source of truth for cross-component UI configuration.
 *
 * Loaded as a classic (non-module) script so the values are exposed on the
 * global `window.AquaUI` namespace, matching the existing component/view/store
 * scripts (all plain global functions invoked by Alpine's `x-data`).
 *
 * Load order: this file MUST be referenced from index.html BEFORE the
 * component and view scripts (e.g. immediately after the store scripts and
 * before the `<!-- Component scripts -->` block). Consumers access the
 * namespace lazily (inside getters/methods that Alpine calls after all scripts
 * have loaded), and each consumer keeps an inline fallback so the UI still
 * renders correctly even if this script is absent.
 *
 * Consumed by:
 *   - chemistry-gauge.js / settings-view.js  (chemistry band defaults + key)
 *   - equipment-button.js / pool-graphic.js  (isActiveStatus + device keywords)
 */
(function (global) {
    'use strict';

    // ---- Chemistry bands -----------------------------------------------------
    // localStorage key holding per-gauge band overrides.
    const CHEMISTRY_BANDS_KEY = 'chemistryBands';

    // Three-tier band defaults (Good / Okay / Bad) for each chemistry gauge.
    // Shared between the gauge component (rendering) and the settings view
    // (editing + reset-to-defaults). Display-only fields (min/max/unit/etc.)
    // live in the gauge component; only the editable band fields are here.
    const CHEMISTRY_BAND_DEFAULTS = {
        ph: {
            goodMin: 7.4, goodMax: 7.6,
            okayMin: 7.2, okayMax: 7.8,
            badMin:  7.0, badMax:  8.0
        },
        orp: {
            goodMin: 700, goodMax: 750,
            okayMin: 650, okayMax: 700,
            badMin:  650, badMax:  800
        },
        salt: {
            goodMin: 3500, goodMax: 4000,
            okayMin: 2700, okayMax: 3500,
            badMin:  2700, badMax:  4500
        }
    };

    // ---- Active-status predicate --------------------------------------------
    // Status strings the backend reports (magic_enum names) that mean the
    // device is currently "on/active": On, Running, Heating, Enabled.
    const ACTIVE_STATUS_VALUES = ['on', 'running', 'heating', 'enabled'];

    function isActiveStatus(status) {
        const s = String(status == null ? '' : status).toLowerCase();
        return ACTIVE_STATUS_VALUES.includes(s);
    }

    // ---- Device-classification keyword lists --------------------------------
    // Label substrings (lower-case) used to classify a button/device when no
    // explicit device_type trait is available. Order within a list does not
    // matter; the first matching CLASSIFIER (in icon resolution) wins.
    const DEVICE_KEYWORDS = {
        chlorinator: ['chlor', 'aquapure', 'salt'],
        spa:         ['spa'],
        pump:        ['pump', 'filter'],
        heater:      ['heat']
    };

    function labelMatchesKeywords(label, keywords) {
        const l = String(label == null ? '' : label).toLowerCase();
        return keywords.some((kw) => l.includes(kw));
    }

    // ---- Chlorinator (SWG) health display keys --------------------------------
    // Catalog key for a backend ChlorinatorHealth enum name (see docs/i18n.md),
    // or null for an unknown value (callers fall back to the raw name).
    // Shared by the pool store's health label and the alerts store's
    // chlorinator alert details.
    const SWG_HEALTH_KEYS = {
        'Ok': 'swg_health.ok',
        'TurningOff': 'swg_health.turning_off',
        'Warning_NoFlow': 'swg_health.no_flow',
        'Warning_LowSalt': 'swg_health.low_salt',
        'Warning_HighSalt': 'swg_health.high_salt',
        'Warning_HighCurrent': 'swg_health.high_current',
        'Warning_CleanCell': 'swg_health.clean_cell',
        'Warning_LowVoltage': 'swg_health.low_voltage',
        'Warning_LowTemperature': 'swg_health.low_temperature',
        'Error_CheckPCB': 'swg_health.check_pcb',
        'GeneralFault': 'swg_health.general_fault',
        'Unknown': 'common.unknown'
    };

    function swgHealthKey(health) {
        return SWG_HEALTH_KEYS[String(health)] || null;
    }

    // ---- Chlorinator (SWG) output-state display keys ---------------------------
    // Catalog key for a backend ChlorinatorGeneratingReason enum name — WHY the
    // cell is at its current output. The instantaneous output is 0% whenever the
    // cell is idle, which on its own reads as "off or broken"; the reason is what
    // distinguishes a chlorinator that has been turned off from one that is
    // configured and healthy but waiting for the filter pump. Null for an unknown
    // value (callers fall back to showing nothing).
    const SWG_REASON_KEYS = {
        'Generating': 'swg_reason.generating',
        'Off': 'swg_reason.off',
        'PumpOff': 'swg_reason.pump_off',
        'NoFlow': 'swg_reason.no_flow',
        'Fault': 'swg_reason.fault',
        'Idle': 'swg_reason.idle',
        'Unknown': 'swg_reason.unknown'
    };

    function swgReasonKey(reason) {
        return SWG_REASON_KEYS[String(reason)] || null;
    }

    // ---- Device operating-state display keys ----------------------------------
    // Catalog key for a backend OperatingStates enum name (magic_enum::enum_name
    // over IAQDevice/OneTouchDevice's OperatingStates — a cross-boundary contract,
    // see diagnostics-view.js's operatingStateClass), or null for an unknown value
    // (callers fall back to the raw name). Shared by the diagnostics device cards
    // and the dashboard/detail equipment status badges.
    const OPERATING_STATE_KEYS = {
        'ColdStart': 'devcard.op_state_coldstart',
        'StartUp': 'devcard.op_state_startup',
        'Scraping': 'devcard.op_state_scraping',
        'NormalOperation': 'devcard.op_state_normal',
        'ScrapingFaulted': 'devcard.op_state_scraping_faulted',
        'FaultHasOccurred': 'devcard.op_state_fault',
        'NotPresent': 'devcard.op_state_not_present'
    };

    function operatingStateKey(state) {
        return OPERATING_STATE_KEYS[String(state)] || null;
    }

    // ---- Equipment button status display keys ----------------------------------
    // Catalog key for a backend button-status enum name (magic_enum names over
    // Auxillary/Pump/Heater status — see equipment-button.js's header comment for
    // the full per-device-type value list), or null for an unknown value (callers
    // fall back to the raw name). Shared by the dashboard equipment cards and the
    // Detailed view's per-body equipment rows, so both render the same label for
    // the same wire value.
    const BUTTON_STATUS_KEYS = {
        On: 'common.on',
        Off: 'common.off',
        Enabled: 'common.enabled',
        Pending: 'status.pending',
        Unknown: 'common.unknown',
        Running: 'common.running',
        Heating: 'status.heating',
        NotInstalled: 'status.not_installed',
    };

    function buttonStatusKey(status) {
        return BUTTON_STATUS_KEYS[String(status)] || null;
    }

    // Ready-to-render label: translated status, raw fallback for an unknown
    // value, or '--' for no status at all.
    function buttonStatusLabel(status) {
        if (!status) { return '--'; }
        const key = buttonStatusKey(status);
        return key ? window.AquaI18n.t(key) : String(status);
    }

    // ---- Equipment version field labels ----------------------------------------
    // Catalog key for the backend's EquipmentVersionField.label — a small, fixed
    // set of English labels sent verbatim in the /api/equipment "version.fields"
    // array (see json_equipment.cpp's GenerateJson_Equipment_Version). An unknown
    // label (custom equipment) falls back to the raw string, matching every other
    // wire-enum display convention in this app.
    const EQUIPMENT_VERSION_LABEL_KEYS = {
        'Model': 'about.model',
        'Type': 'about.type',
        'Revision': 'about.revision',
    };

    function equipmentVersionLabel(label) {
        if (!label) { return ''; }
        const key = EQUIPMENT_VERSION_LABEL_KEYS[String(label)];
        return key ? window.AquaI18n.t(key) : String(label);
    }

    global.AquaUI = {
        CHEMISTRY_BANDS_KEY,
        CHEMISTRY_BAND_DEFAULTS,
        ACTIVE_STATUS_VALUES,
        isActiveStatus,
        DEVICE_KEYWORDS,
        labelMatchesKeywords,
        swgHealthKey,
        swgReasonKey,
        operatingStateKey,
        buttonStatusKey,
        buttonStatusLabel,
        equipmentVersionLabel
    };
})(typeof window !== 'undefined' ? window : globalThis);
