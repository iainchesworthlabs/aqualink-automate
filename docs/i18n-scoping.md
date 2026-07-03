# Internationalization (i18n) Scoping

> **Status: scoping snapshot, 2026-07-02.** This is a point-in-time analysis and roadmap, not
> current-truth documentation. Symbols and behaviours cited below were verified against the code
> on this date; re-verify before relying on them later.
>
> **Reconciliation 2026-07-02 (same day): Phase 0 is implemented** on `feat/i18n` — see
> [docs/i18n.md](i18n.md) for the as-built documentation. Two deviations from the plan below:
> catalogs are per-locale **JS registration files** (`assets/web/i18n/<code>.js`), not fetched
> JSON — English loads synchronously as a classic script so the fallback needs no fetch and the
> first paint never flashes raw keys; and locale persistence needed a small backend change
> (`/api/preferences` now shallow-merges top-level `ui.*` keys instead of replacing the blob).
> The key-completeness checker exists at `scripts/check-i18n-keys.ps1`.
>
> **Reconciliation 2026-07-02 (later the same day): Phase 1 is implemented** on
> `feat/i18n-phase1` — locale-aware value formatting (docs/i18n.md "Value formatting"),
> WS `TemperatureUpdate` switched to raw dual-unit objects (both live defects fixed:
> the hardcoded-Celsius `TranslationsAndUnitsFormatter` is deleted; the HA setpoint
> number entities follow `Temperature_DisplayUnits` with discovery republish). Sensors
> deliberately stay °C (HA self-converts sensors — the "should still be honest" note
> below was half-right). Trends chart stays °C pending a chart-wide conversion.
>
> **Reconciliation 2026-07-03: Phase 2 is implemented** on `feat/i18n-phase2` —
> HTTP errors carry `{error, code, params}` (additive; `error` stays the English
> string, deviating from the plan's error-object shape for API compatibility),
> alert WS/webhook payloads carry structured `params` next to `detail`, and the
> UI translates both from `error.<code>` / `alert_detail.<condition>` catalog
> entries. See docs/i18n.md "Structured errors and alerts".
>
> **Reconciliation 2026-07-03: Phase 3 is implemented** on `feat/i18n-phase3` —
> the stylesheets converted to logical properties, `[dir='rtl']` overrides for
> switch knobs/toast slide, and time-axis geometry pinned LTR (docs/i18n.md
> "RTL layout"). The alerts-dropdown narrow-viewport clipping turned out to be
> pre-existing in BOTH directions and is tracked separately. *(Fixed 2026-07-03:
> `.alerts-dropdown` now anchors to `.nav-inner` instead of the bell — see the
> Phase 3 breaker list below.)*
>
> **Reconciliation 2026-07-03: Phase 4 is implemented** on `feat/i18n-phase4` —
> a vendored Noto Sans Arabic variable woff2 (Arabic `unicode-range` slice only,
> so it acts as a per-glyph fallback behind the Latin brand faces) plus a named
> Japanese system-font stack under `html[lang='ja']`. Deviates from the plan in
> one respect: the "optional" vendored Noto subset was taken up for Arabic so the
> rendered face is consistent across platforms instead of whatever `system-ui`
> resolves to; CJK stays on named system faces. See docs/i18n.md "Fonts and
> scripts".
>
> **Reconciliation 2026-07-03: Phase 5 is implemented** on `feat/i18n-phase5` —
> four more locales (es, fr, he, zh), bringing the shipped set to eight. Hebrew
> is the second RTL locale (it exercised the Phase 3 work unchanged) and got a
> vendored Noto Sans Hebrew per-glyph fallback; Chinese got a `html[lang='zh']`
> system-font stack. The pseudo-locale, catalog-parity CI check, and
> contributor workflow this phase planned had already shipped with Phase 0;
> the "owner decision" on first languages was overtaken by shipping de/ar/ja
> in Phase 0 directly. Also fixed here: the parity checker only matched
> single-quoted catalog values, silently skipping the ~14 keys whose English
> text contains an apostrophe (real key count 587, not the 573 it reported).
>
> Phase 5 also absorbed the first real guard-rail catch: the auth/identity
> feature merged to develop mid-phase with its login/account/administration
> UI hardcoded in English (47 visible offenders on one route). All ~182
> strings were extracted into `auth.*`/`account.*`/`admin.*` keys and
> translated into all seven locales, and two auth-integration defects were
> fixed: server preferences were token-gated (silently disabling per-user
> prefs/locale sync for the default auth-off posture — now gated on
> `can('prefs.self')`), and the boot-time server-locale pull could stomp a
> locale chosen while its request was in flight (now yields to an explicit
> in-session choice).

## Goal

Allow the web UI (and, where it is our responsibility, the outward-facing integration surfaces)
to present in the user's language and conventions: translated UI text, locale-aware number /
date / relative-time formatting, and right-to-left (RTL) layout for Arabic / Hebrew / Farsi.
Everything must be **general** — locale is a runtime user choice, never a build-time or
maintainer-specific assumption.

## Current state — the short version

| Surface | State today | i18n verdict |
|---|---|---|
| Web UI text | ~180–210 hardcoded English strings in `assets/web` (index.html ~45%, stores ~30%, views ~25%). No i18n infrastructure of any kind. | Bulk of the work; all ours. |
| Web UI formatting | Mixed: counts use `toLocaleString()` (locale-aware), but decimals use `toFixed()` (always "."), relative time ("5m ago") and uptime units ("5d 3h") are hardcoded English. | Fix with `Intl.*`. |
| CSS / RTL | Not RTL-capable: 50+ physical properties (`margin-left`, `right: 0`, `text-align: left`), `lang="en"` hardcoded, no `dir` switching. Layout is mostly flex/grid, which *does* auto-mirror once `dir="rtl"` is set. | Convert to logical properties; targeted fixes for absolutely-positioned components. |
| Fonts | Hanken Grotesk + Bricolage Grotesque — Latin-only coverage. No Arabic / Hebrew / CJK. | Add per-script fallbacks. |
| Backend REST/WS wire | Mostly i18n-friendly already: statuses are symbolic `magic_enum` names (`"On"`, `"Heating"`, `"Warning_LowSalt"`), timestamps are ISO 8601 / epoch, temperatures dual-emit `{celsius, fahrenheit}` numbers. | Frontend translates the symbolic keys; backend largely untouched. |
| Backend prose | ~40+ hardcoded English error sentences in HTTP handlers (`webroute_diagnostics_*`, `response_400/500`, spa-side remote routes). Alert `detail` prose in `AlertMonitor`. | Convert to symbolic codes + params; frontend translates. |
| WebSocket temperatures | `TranslationsAndUnitsFormatter::Localised()` (`src/core/localisation/`) pre-formats temperature *strings* in `DataHubConfigEvent_Temperature` and the equipment system-status event — with the unit **hardcoded to Celsius**, ignoring `PreferencesHub::Temperature_DisplayUnits`. REST sends raw numbers instead. | Existing defect + architectural split; move formatting to the frontend. |
| Device-originated text | Aux labels, pool names, panel menu/status text arrive over RS-485 from the controller (English or user-configured) and pass through verbatim. | **Cannot be translated at source.** Pass through; treat as user data. |
| MQTT / Home Assistant | Entity friendly names are hardcoded English in `HADiscovery` ("Pool Temperature", "Salt Level", …). State payloads are technical tokens (fine). `device_class` entities are translated by HA itself. `unit_of_measurement` is hardcoded `°C`, ignoring the display-units preference. | Small, bounded; decide whether names localize at all (HA users often rename entities). Unit fix is real. |
| Webhooks | Payload has symbolic `condition`/`state` (good) plus an English prose `detail` (e.g. "Salt 2400 ppm below threshold 2700 ppm"). | Add structured params alongside `detail`; consumers format. |
| Matter | Diagnostics-only today; no labels published from this branch. | Consumer's job; revisit when label publication lands. |
| CLI `--help`, logs | English, developer/operator-facing. | Out of scope — stays English. |

## Architectural principles

1. **Translate at the edge (frontend), keep the wire symbolic.** The backend already speaks in
   enum names, ISO timestamps, and raw numbers; extend that discipline rather than adding a
   server-side translation catalog. A C++ gettext/boost::locale stack would add heavy machinery
   for ~40 strings whose only consumer is our own frontend.
2. **Errors become codes + parameters.** `{"error":"Invalid severity level"}` →
   `{"error":{"code":"invalid_severity","params":{...},"message":"Invalid severity level"}}`.
   The English `message` stays for curl/API consumers and log greppability; the frontend
   translates from `code`. This is a swagger.yaml + `docs/usage-and-api.md` change.
3. **Device-originated and user-configured text is data, not UI copy.** Aux labels, pool names,
   `LabelOverrides`, spa-switch names, panel screen text: pass through verbatim, never machine-
   translate. UI copy *around* them is translated ("Status: {label}" pattern — beware
   concatenation-order assumptions; use placeholder templates, not string `+`).
4. **Locale is a user preference with a browser-detected default.** UI language/locale lives with
   the UI (persisted via the existing opaque `UiPreferences` blob + `navigator.language`
   fallback), so it is per-browser today and slots cleanly under per-user preferences when the
   auth redesign lands. A backend-visible locale (PreferencesHub field) is only needed if/when we
   localize backend-emitted prose such as webhook `detail` — deferred.
5. **Formatting is `Intl.*`, not hand-rolled.** `Intl.NumberFormat` (decimal separators, unit
   display), `Intl.DateTimeFormat`, `Intl.RelativeTimeFormat`, `Intl.PluralRules`. No vendored
   CLDR data needed — browsers ship it.
6. **Minimal translation runtime, not a framework.** For a vanilla Alpine app with ~200 strings,
   a small `t(key, params)` helper + per-locale JSON (`assets/web/i18n/<locale>.json`) + an
   Alpine `$store.i18n` is enough; `Intl.PluralRules` covers plural forms. Vendoring i18next is
   the fallback if message complexity grows (rich plurals/gender), not the starting point.

## Work inventory by phase

### Phase 0 — Foundations (frontend)

- `assets/web/i18n/en.json` with a stable key scheme (`nav.dashboard`, `status.heating`,
  `error.invalid_severity`, `alert.salt_low.detail`, …); extraction of the ~180–210 strings from
  `index.html`, stores, and views into `t()` calls / `x-text` bindings.
- `i18n` Alpine store: locale resolution (`UiPreferences` override → `navigator.language` →
  `en`), async catalog fetch, `t(key, params)` with placeholder substitution, missing-key
  fallback to English + console warning.
- `<html lang>` / `<html dir>` set dynamically from the active locale.
- Language picker in Settings view; persistence through the existing `UiPreferences` round-trip.
- Service worker: add `i18n/*.json` to the precache list (they already ride the network-first
  strategy, so staleness is bounded the same way as JS/CSS); bump shell version via the existing
  `stamp_sw_version.cmake` flow.
- PWA `manifest.json` stays English (app name is a brand, and manifests are single-locale).

### Phase 1 — Formatting correctness (frontend + small backend fix)

- Replace `toFixed(n) + ' °C'`-style concatenations with `Intl.NumberFormat` (including
  `style: 'unit'` where supported) for temperature, pH, ORP, salt PPM, percentages, latency.
- `Intl.RelativeTimeFormat` for "just now / 5m ago"; localized uptime units.
- `Intl.DateTimeFormat` for the three `toLocaleTimeString()`/`toLocaleString()` call sites
  (fine today, but route them through one helper so locale override — not just browser locale —
  applies).
- **Backend fix (with regression test):** stop pre-formatting temperature strings in
  `DataHubConfigEvent_Temperature` / the system-status event, or at minimum make
  `TranslationsAndUnitsFormatter::Localised()` honour `PreferencesHub::Temperature_DisplayUnits`
  instead of the hardcoded Celsius constant. Preferred end-state: WS events carry the same raw
  dual-unit numbers as REST and the frontend formats. This is a WS-payload shape change →
  update `docs/usage-and-api.md` WS event list (and swagger if the shapes are documented there).
- **HA discovery unit fix:** publish `unit_of_measurement` (`°C`/`°F`) from
  `Temperature_DisplayUnits` instead of hardcoded `°C` (temperature sensors, setpoint numbers).
  Note HA converts units itself per-entity, but the discovery default should still be honest.

### Phase 2 — Backend-reaching prose becomes symbolic

- Error-code refactor across `src/core/http/` handlers: shared helper producing
  `{code, params, message}`; enumerate the ~40 messages; swagger.yaml error-schema update;
  frontend `error.<code>` catalog entries.
- Alert notifications: keep `condition` (already symbolic) and add structured params
  (`{value, threshold, device_label}`) next to the English `detail` in both the WS
  `AlertNotification` and the webhook payload (additive, backward-compatible). Frontend builds
  localized alert text from condition + params; webhook consumers may do the same.
- Frontend maps for backend enum names: statuses (`Heating`, `Warning_LowSalt`, `Pending`, …),
  equipment mode (`Normal`/`Service`/`TimeOut`), circulation modes, system status — one
  `status.<EnumName>` catalog namespace, tolerant of unknown values (show raw name).

### Phase 3 — RTL

- CSS logical-properties conversion in `app.css` / `components.css`: `margin-left` →
  `margin-inline-start`, `left/right` offsets → `inset-inline-*`, `text-align: left` → `start`,
  `translateX` knob animation made direction-aware. Flex/grid layout mirrors for free once
  `dir="rtl"` is set.
- Targeted fixes for the known breakers: alerts dropdown anchor, toast container corner, alerts
  badge position, schedule-timeline axis padding, `margin-left: auto` right-aligners, trends
  hover readout.
  - *Reconciled 2026-07-03:* Phase 3 converted the alerts dropdown/badge anchors to
    `inset-inline-end`, which mirrored them in RTL but left the (pre-existing, both-direction)
    narrow-viewport clipping when the nav wraps. That is now fixed too: `.alerts-dropdown`
    anchors to `.nav-inner` instead of the bell (full-width sheet below the nav at narrow
    widths), so it can never overflow the viewport regardless of where the wrap point falls
    for a given locale.
- Numerals/percent widths in the timeline: keep LTR numerals inside RTL text via
  `unicode-bidi`/`dir="ltr"` islands where needed.
- Playwright RTL smoke test (load with an RTL locale, assert `dir`, screenshot key views).

### Phase 4 — Fonts & scripts

- Per-script fallback stacks (system fonts first: `system-ui` covers Arabic/Hebrew/CJK on all
  target platforms) and optionally vendored Noto Sans subsets for visual consistency. Keep the
  Latin brand fonts as the primary face.

### Phase 5 — Translations & guardrails

- First non-English locale(s) — **owner decision needed** (see below).
- Pseudo-locale (`en-XA`-style accented/expanded strings) generated from `en.json` to smoke-test
  for missed hardcoded strings and layout overflow; a CI/e2e check that every catalog key used
  in code exists in `en.json`.
- Contributor workflow: how to add a locale (one JSON file + registry entry), documented in
  `docs/configuration.md` or a new `docs/i18n.md`.

## Explicit non-goals

- Translating CLI `--help`, config-file descriptions, log/exception text (operator-facing).
- Machine-translating device-originated screen text or user-configured labels.
- Server-side locale negotiation (`Accept-Language` content negotiation for HTML) — the SPA
  resolves locale client-side.
- Matter label localization — nothing is published yet; revisit with that feature.

## Open decisions (owner input wanted)

1. **Initial target languages.** Determines translation effort and whether RTL (Phase 3) is
   needed early or can trail. RTL is the most invasive phase; if the first locales are e.g.
   es/fr/de, Phases 0–2 ship value without it.
2. **HA entity friendly names.** Recommend leaving them English: HA localizes `device_class`
   names itself, users rename entities freely, and translated discovery names churn HA entity
   registries. If localizing, they'd key off a new PreferencesHub locale (backend-side catalog —
   scope grows).
3. **Webhook `detail` prose.** Recommend additive params (Phase 2) and keep English `detail`;
   full localization would need the backend locale + catalog.
4. **Translation runtime.** Recommendation above is the minimal custom `t()`; confirm vs
   vendoring i18next.

## Known-defect callouts found during scoping

- `TranslationsAndUnitsFormatter::Localised(Temperature)` ignores
  `PreferencesHub::Temperature_DisplayUnits` (unit hardcoded to Celsius) — affects WS
  temperature strings today, independent of i18n.
- HA discovery `unit_of_measurement` hardcoded `°C` regardless of the same preference.
