# Home Assistant Companion Package — Design & Delivery Plan

**Date:** 2026-08-06 · **Status:** Phase 1 merged to `develop` 2026-08-07 (PR #128, [`homeassistant/companion/`](https://github.com/iainchesworth/aqualink-automate/tree/main/homeassistant/companion)); Phase 2 merged 2026-08-07 (PR #134, app-served under `/homeassistant/*`); Phase 3 in progress 2026-08-08 (add-on installer approved — see §11.2 — plus activity-scene blueprints, spa-readiness, and the showcase dashboard; the analytics/energy module was dropped as non-generalizable, see §7). Add-on installer verified end-to-end 2026-08-08 against a from-source build (see §7 addendum below) — two real bugs found and fixed: the `homeassistant_config` map mounts at `/homeassistant_config` (the Supervisor's documented `/<type-name>` default), not `/homeassistant` as first assumed; and `.dockerignore` was silently dropping `blueprints/`/`packages/`/`dashboards/` from every Docker-built image since Phase 2 (only `entity-manifest.json` was ever re-included), which also means Phase 2's HTTP-served bundle was broken on Docker/Linux runtimes — including the add-on — until now, even though this went undetected by CI (`docker-verify` only smoke-tests `--version`).

> Design/analysis snapshot. File/symbol citations were verified against the code on the
> date above; re-verify before relying on them.

## 1. Goal

Ship an installable "companion package" for Home Assistant alongside the application, so
that any user who runs aqualink-automate (add-on, Docker, or bare install) with MQTT +
HA discovery enabled can, in a few minutes, get:

1. a **pool dashboard** that works out of the box against the entities the app publishes,
2. a set of **alert/automation blueprints** (actionable + advisory notifications,
   optional self-healing actions), and
3. a **helpers package** (input helpers, derived template/history sensors) that the
   dashboard and blueprints build on.

Everything must be **general** — parameterised by blueprint inputs and helper entities,
never hardcoded to any one pool. The maintainer's own HA install is used below only as
evidence of what is useful, not as content to copy verbatim.

## 2. Evidence base

### 2.1 The entity contract (what the app publishes)

Source of truth: `HomeAssistantDiscovery` (`src/core/mqtt/ha_discovery.cpp`), which
emits one retained device-bundled discovery payload (`cmps` format). Key facts that
shape this design:

- **Entity IDs are deterministic across every install.** The device `name` is the
  hard-coded literal `"aqualink-automate"` and each component sets a `name`, so HA
  derives `<domain>.aqualink_automate_<name-slug>`. The per-install random
  `ha_device_id` (add-on `run.sh`) only affects `unique_id`s, not entity IDs. A shipped
  dashboard/package can therefore reference `sensor.aqualink_automate_pool_temperature`
  etc. directly.
- **Static entities** (sensor/binary_sensor/switch/number/select only): pool/spa/air
  temperatures with `*_last_updated` + `*_stale` companions, freeze-protect temperature,
  pool/spa setpoint numbers (writable), ORP / pH / salt-level, circulation mode
  (sensor + dual-body select/switch), equipment mode, spa/clean mode binary sensors,
  uptime.
- **Alert entities**: six `device_class: problem` binary sensors from
  `Alerting::AlertConditions` — `chlorinator_fault`, `chlorinator_warning`, `salt_low`,
  `service_mode`, `serial_comms_loss`, `temperature_stale`.
- **Dynamic entities** (panel-label dependent, install-specific entity IDs): per-device
  switches (pumps/aux/chlorinator), heater status sensors + `… Enable` switches,
  chlorinator generating %/boost/health sensors + generating-setpoint number + boost
  switch.
- **Entities are conditionally published** (body presence, dual-body gating) and absent
  ones are tombstoned. Companion content must tolerate missing entities (conditional
  cards; optional blueprint inputs).
- **Availability**: all entities share the `{prefix}/status/availability` LWT topic, so
  "app died" manifests as every entity becoming `unavailable` — a reliable offline
  signal.

Units: the app publishes °C or °F depending on panel config; blueprints must treat
temperature thresholds as unit-agnostic numbers. Salt is always ppm.

### 2.2 Proven patterns from a real deployment (generalised)

A long-running production HA install using the app surfaced these as the things worth
shipping:

- **Notification hygiene** (present in every hand-written alert): quiet-hours window,
  once-per-day dedup, `unknown`/`unavailable` guards, and *actionable* message content —
  e.g. the salt alert computes **kg of salt to add** from the ppm deficit and pool
  volume, not just "salt low".
- **Three alert tiers**: advisory (notify only), actionable (notify with a concrete
  action), self-healing (act, then notify) — e.g. re-enable the chlorinator if it is
  off while the pump runs; restart the filter pump if daily runtime is short.
- **Staleness watchdogs**: "water-quality sensors have not reported for > N hours".
  (The app now publishes `*_stale` binary sensors natively; the blueprint should use
  them and keep a generic max-gap fallback for arbitrary sensors.)
- **Activity scenes**: Swim / Spa / Jet-pool scenes built from `input_boolean` + `timer`
  + auto-off automation, sun-conditioned lighting, tap-to-start/hold-to-stop.
- **Derived analytics**: pump/heater/chlorinator runtime today, temperature delta today,
  spa warm-up readiness %; (heat-pump COP/energy models exist but depend on external
  power meters — optional module at best).
- **Dashboard reality check**: the hand-built dashboard uses six HACS custom cards
  (button-card, layout-card, card-mod, mushroom, bar-card, stack-in-card). A bundled
  dashboard must instead default to **core cards only** (sections view + tile cards) so
  it works on a stock HA install.

## 3. Design principles

1. **General by construction.** Anything referencing user-variable entities (aux
   switches, notify services, thresholds) is a **blueprint input with a selector**;
   anything referencing the deterministic `aqualink_automate_*` entities may be
   hardcoded to those defaults but remain overridable.
2. **Zero dependencies by default.** Core dashboard uses stock cards; no HACS, no
   custom integrations. Enhanced/"showcase" content is clearly optional.
3. **Tolerate absence.** Conditional cards for spa/dual-body/chemistry entities;
   optional blueprint inputs for equipment a config may not have.
4. **Notification hygiene baked in.** Every alert blueprint carries quiet hours,
   dedup interval, availability guards, and a customisable message with computed,
   actionable values.
5. **Never touch the user's `configuration.yaml` automatically.** All install paths are
   copy/import actions the user performs (or explicitly opts into for the add-on
   installer in a later phase).
6. **Single source of truth in the repo**; docs site, release artifact, and (later)
   app-served copies all derive from it.

## 4. Package contents

### 4.1 Automation blueprints — `blueprints/automation/aqualink-automate/`

All share the hygiene scaffold (quiet-hours `input_datetime` entity selectors defaulting
to the helpers package's entities, dedup interval, unavailable-guards, notify target as
input, message override). Phase-1 set:

| Blueprint | Tier | Trigger / logic | Key inputs (beyond scaffold) |
|---|---|---|---|
| `salt-level-low.yaml` | actionable | salt sensor below target for N min; message computes `kg = deficit_ppm × volume_L / 1 000 000` | salt sensor (default `sensor.aqualink_automate_salt_level`), target ppm, pool volume L |
| `equipment-problem.yaml` | advisory | any chosen `problem` binary_sensor on for N min (covers chlorinator fault/warning, salt low, service mode, comms loss, temperature stale) | alert entity/entities, per-entity friendly text |
| `app-offline.yaml` | advisory | sentinel app entity `unavailable` for N min (LWT-driven) | sentinel entity (default `sensor.aqualink_automate_uptime`), grace period |
| `pump-runtime-shortfall.yaml` | actionable / self-healing | at a check time, `pump runtime today < target h`; optional action: turn pump on during a user window, then notify | pump switch, runtime sensor (from helpers pkg), target hours, self-heal on/off + window |
| `chlorinator-self-heal.yaml` | self-healing | pump on ≥ N min during window ∧ chlorinator switch off → turn on + notify | pump switch, chlorinator switch, window, optional ORP sensor + floor |
| `swim-ready.yaml` | advisory | pool temp rises above threshold (optionally only while pump on) | temp sensor, threshold number (unit-agnostic), optional pump condition |
| `freeze-advisory.yaml` | advisory | air temp at/below threshold or below the panel's freeze-protect sensor | air temp sensor, threshold or freeze-protect entity |

Later phases: `service-mode-left-on` duration variant (or fold into
`equipment-problem`), a **script blueprint** `activity-scene.yaml` generalising the
Swim/Spa/Jet scenes (inputs: device switches, timer entity, scene boolean, optional
sun-gated lights).

### 4.2 Helpers package — `packages/aqualink_automate.yaml`

A standard HA YAML package (user enables `homeassistant: packages:` once — documented):

- `input_datetime.aqualink_quiet_hours_start` / `_end` (defaults 22:00 / 08:00)
- `input_number.aqualink_pool_volume` (L), `input_number.aqualink_target_salt_ppm`,
  `input_number.aqualink_pump_runtime_target` (h)
- Template sensors: `salt_to_add_kg` (deficit × volume), `pool_temperature_delta_today`
  (trigger-based, resets at midnight)
- `history_stats` sensors: `aqualink_pump_runtime_today` (requires the user to name
  their pump switch — shipped commented with the most common entity id and a doc note,
  since pump entity ids are panel-label dependent), heater heating-time today (from a
  heater status sensor's `Heating` state)
- Optional commented block: spa-readiness % (trigger-based template capturing start
  temperature when spa mode turns on)

Nothing in the package *requires* editing beyond the two commented, install-specific
entity references; everything else works untouched.

### 4.3 Dashboard — `dashboards/aqualink-pool.yaml`

Core-cards-only **sections view** dashboard:

- **Overview**: temperature hero (tile + gauge), circulation-mode select tile
  (conditional on dual-body), pool/spa setpoint number tiles, heater status tiles,
  chemistry section (salt/pH/ORP tiles + `salt_to_add_kg`), equipment section with a
  clearly-marked placeholder area for the user's panel-specific aux/pump switches,
  conditional alert tiles for all six problem binary sensors (visible only when `on`).
- **Trends**: history-graph / statistics-graph cards for temperatures, salt, runtime.

Install paths documented: paste into a new dashboard's raw configuration editor, or
register as a YAML-mode dashboard. A second, optional **showcase** dashboard (HACS
button-card styling, derived from the maintainer's design but fully generalised) is a
later phase and lives beside it clearly labelled with its dependencies.

## 5. Repo layout

```
homeassistant/                  # existing HA-related tree (art/, dev/) — edge-generator-safe
  companion/
    README.md                   # what's inside, install quick-start, links to docs page
    entity-manifest.json        # machine-readable list of static+alert entity_ids (see §7)
    blueprints/automation/aqualink-automate/*.yaml
    packages/aqualink_automate.yaml
    dashboards/aqualink-pool.yaml
```

Kept **outside** `aqualink-automate/` (the add-on folder) so the edge add-on generator
and the add-on CI lock-step checks are untouched.

## 6. Distribution channels (in delivery order)

1. **Docs site page** (`docs/homeassistant-companion.md`, added to the *User guide*
   nav): overview, per-blueprint **one-click import buttons** via
   `https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=<url-encoded
   raw.githubusercontent.com URL on main>`, package + dashboard install walk-throughs,
   customisation notes (renamed entities, °F panels, single-body configs). HA fetches
   blueprints server-side from the raw URL, so main-branch URLs give users fixes on
   re-import without waiting for a release.
2. **GitHub release artifact**: `aqualink-automate-homeassistant-companion-<version>.zip`
   produced by one `zip -r` step in `release.yml`'s `github-release` job before
   *Create GitHub Release* (everything in `release-artifacts/` is published). The
   existing GPG-signing glob (`*.zip`) picks it up automatically; consciously include it
   in (or exclude it from) the attestation globs.
3. **Served by the app itself** (Phase 2): copy `homeassistant/companion/` into
   `assets/web/homeassistant/` at build time (extend the existing
   `copy_directory_if_different` / CPack `assets/web/` install; the generic
   `StaticFileHandler` + existing `.yaml → application/yaml` MIME mapping serve it with
   no C++ changes — same mechanism as `/api/swagger.yaml`). Optionally link it from the
   web UI later (any UI text must go through i18n catalogs).
4. **Add-on opt-in installer** (Phase 3, needs a deliberate decision): add
   `homeassistant_config` to the add-on `map:` (a new user-visible permission) plus an
   `install_companion_package: false` option; `run.sh` then idempotently copies
   blueprints into `/config/blueprints/automation/aqualink-automate/` (never touching
   `configuration.yaml`). Requires schema + **every** `translations/<lang>.yaml`,
   `DOCS.md`, edge regeneration, and docs updates per the add-on rules.

## 7. Validation & CI

New job (either in `.github/workflows/homeassistant-addon.yml` or a sibling
`ha-companion.yml`), path-filtered on `homeassistant/companion/**`:

1. **yamllint** over the companion tree.
2. **Entity cross-check** — `scripts/check-ha-companion-entities.ps1`: extract every
   `(sensor|binary_sensor|switch|number|select)\.aqualink_automate_\w+` reference from
   companion YAML and require membership in `entity-manifest.json`. The manifest is
   kept honest against `ha_discovery.cpp` by extending
   `test/unit/mqtt/test_mqtt_ha_discovery.cpp` with a case that derives the entity-id
   list from the built discovery payload and compares it to the manifest (test data can
   read the checked-in JSON; the test tree already mirrors assets).
3. **Real HA config validation** — run the official
   `ghcr.io/home-assistant/home-assistant:stable` container against a synthetic config
   dir (in `homeassistant/companion/test-harness/`): `configuration.yaml` enabling
   packages + including an `automations.yaml` that instantiates **every blueprint**
   with dummy entity ids, blueprints copied into place; execute
   `python -m homeassistant --script check_config -c /config`. This catches schema
   errors in blueprints, package templates, and `history_stats` definitions
   (`check_config` validates schema, not entity existence — exactly what we want).

Dashboard YAML gets yamllint + the entity cross-check only (no practical schema
validator for Lovelace outside a running frontend).

## 8. Documentation integration

Per the repo's docs-accuracy rules, the same change that lands the package must:

- Add `docs/homeassistant-companion.md` and put it in the `mkdocs.yml` *User guide* nav
  (`mkdocs build --strict` will verify links). While editing the nav, also add the
  currently orphaned `docs/homeassistant-addon.md`.
- Cross-link from `docs/mqtt-home-assistant.md` (the entity reference) and
  `docs/homeassistant-addon.md`.
- Update `docs/releasing.md` / `docs/ci-cd.md` for the new release artifact + CI job.
- Screenshots: the app's `scripts/capture-doc-screenshots.js` harness cannot render an
  HA dashboard. Phase 1 ships the page without dashboard imagery (or with a clearly
  labelled manually-captured PNG kept out of the generated-screenshot contract);
  automating HA dashboard captures is explicitly out of scope.

Known pre-existing doc drift found during this survey (fix independently of the
package): `docs/mqtt-home-assistant.md` predates the freshness companions
(`*_updated`/`*_stale`), `pool_setpoint_2`, `pool_heater_2_enabled`, the dual-body
`spa_mode_switch`, and the per-heater `… Enable` switch; `docs/homeassistant-addon.md`
still says the direct LAN port is `80/tcp` (now `8099/tcp`).

## 9. Versioning

The companion tree carries **no independent version**: it rides the app version. The
release zip name embeds the app version; blueprints/README get the version stamped into
a header comment at zip time (release job `sed`), not in-tree. Compatibility statement
in the README: "requires aqualink-automate ≥ <version that introduced the current
entity contract>". If the discovery entity set changes later, the manifest + CI test
force the companion to be updated in the same PR — that is the compatibility mechanism.

## 10. Phased delivery

**Phase 0 — doc fixes (independent):** repair the drift listed in §8.

**Phase 1 — the package (one PR):**
1. Scaffold `homeassistant/companion/` (README, manifest).
2. Helpers package + the seven blueprints (§4.1–4.2).
3. Core-cards dashboard (§4.3).
4. CI job: yamllint + entity cross-check script + HA `check_config` harness; discovery
   unit-test ↔ manifest lock.
5. `docs/homeassistant-companion.md` + nav additions + cross-links.
6. `release.yml` zip step (+ signing/attestation decision) + `docs/releasing.md`,
   `docs/ci-cd.md`.

**Phase 2 — app integration:** build-time copy into `assets/web/homeassistant/` so the
running app serves the bundle; docs note the local URLs (works through add-on ingress).

**Phase 3 — richer content & add-on installer:**
- Add-on `install_companion_package` opt-in installer — **approved** (§11.2): adds the
  `homeassistant_config` map (rw) to `aqualink-automate/config.yaml`, off by default;
  `run.sh` copies the blueprints already baked into the app's own install tree
  (`share/aqualink-automate/web/homeassistant/blueprints/` — the same content Phase 2
  serves over HTTP) into Home Assistant's `blueprints/`, additive-only, never touching
  `configuration.yaml`.
- `activity-scene.yaml` script blueprint + `activity-scene-autooff.yaml` automation
  blueprint (generalised start/stop scene: equipment/timer/boolean/dark-only-lights as
  inputs, a `fields.action` toggle/turn_on/turn_off runtime parameter).
- Spa-readiness: two template sensors in the helpers package (trigger-based baseline +
  derived Off/Warming Up/Ready sensor with a `percent_ready` attribute, built only from
  `spa_mode`/`spa_temperature`/`spa_setpoint`) + a `spa-ready.yaml` notification
  blueprint.
- Showcase dashboard (`dashboards/aqualink-pool-showcase.yaml`, HACS deps: button-card,
  card-mod, layout-card, mushroom, bar-card) — generalised from the evidence base,
  using only app-published + companion-package entities; commented placeholders for
  equipment/scenes.
- Analytics module (heat-pump COP/energy-delivered) — **dropped**: depends on the
  evidence-base install's specific heat pump + power meters, not generalizable (see §7,
  [[open-source-generality]]).
- Web-UI "Home Assistant" page linking the served bundle (full i18n required) —
  **deferred again**, same reasoning as Phase 2.

## 11. Open questions (maintainer decisions)

1. **Attestation**: include the companion zip in the build-provenance/SBOM attestation
   globs, or exclude it? (Recommend include — it is shipped content.)
2. **Add-on installer** (Phase 3): is the `homeassistant_config` permission expansion
   acceptable for an opt-in convenience? Store-listing optics matter here. —
   **Decided 2026-08-08: yes, build it.** Off by default (`install_companion_package`);
   the map/option/permission are documented in `aqualink-automate/config.yaml`,
   `DOCS.md`, and `docs/homeassistant-addon.md` with the exact scope (adds/updates
   files under `blueprints/` only, never touches `configuration.yaml`).
3. **Blueprint URL channel**: main-branch raw URLs (fixes propagate on re-import) vs
   tag-pinned URLs (reproducible). Recommend main for the import buttons, with the
   release zip as the pinned alternative.
4. **Dashboard screenshot policy**: accept a labelled manual PNG for the docs page, or
   ship image-free until an automated capture path exists?
