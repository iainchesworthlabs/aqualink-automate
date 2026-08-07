# AquaLink Automate — Home Assistant companion package

Ready-made Home Assistant content for aqualink-automate installs running with MQTT and
Home Assistant discovery enabled (`--mqtt --home-assistant`, or the Home Assistant
add-on with its default `home_assistant_discovery: true`).

Full installation and customisation guide: **docs/homeassistant-companion.md** in this
repository, published at <https://iainchesworth.github.io/aqualink-automate/homeassistant-companion/>.

## Contents

| Folder | What it is | How to install |
|---|---|---|
| `blueprints/automation/aqualink-automate/` | Alert & self-healing automation blueprints (salt low with "add N kg" advice, equipment problems, app offline, pump-runtime shortfall, chlorinator self-heal, swim ready, freeze advisory) | Import via the buttons on the docs page, or copy into `<config>/blueprints/automation/aqualink-automate/` |
| `packages/aqualink_automate.yaml` | Helpers the blueprints/dashboard build on: quiet-hours datetimes, pool volume + target-salt inputs, salt-to-add and temperature-delta template sensors, commented runtime-sensor recipes | Copy into `<config>/packages/` (enable [packages](https://www.home-assistant.io/docs/configuration/packages/) once) |
| `dashboards/aqualink-pool.yaml` | A pool dashboard built only from stock cards (sections + tiles) | Paste into a new dashboard's raw configuration editor |
| `entity-manifest.json` | The machine-readable list of entity ids the app publishes; CI validates all companion YAML against it | (not installed — reference/CI) |
| `test-harness/` | CI scaffolding for validating this content inside a real Home Assistant container | (not installed — CI only) |

## Design rules for contributors

- Entity ids of the form `*.aqualink_automate_<slug>` are **stable across every
  install** (the discovery device name is fixed), so active YAML may reference the
  static entities listed in `entity-manifest.json` directly.
- Panel-label-driven entities (pumps, aux circuits, chlorinators, heaters) are
  **install-specific**: reference them only in commented examples or as blueprint
  inputs without defaults. CI (`scripts/check-ha-companion-entities.ps1`) enforces
  this by validating every active `*.aqualink_automate_*` reference against the
  manifest.
- Helper entities created *by this package* use the `aqualink_` prefix (never
  `aqualink_automate_`) so app-published and package-created entities cannot collide.
- Stock Home Assistant cards/integrations only — no HACS dependencies.
- Every alert blueprint keeps the hygiene scaffold: quiet hours, repeat-interval
  dedup, unavailable-state guards, actionable message content.

This directory ships with each release as
`aqualink-automate-homeassistant-companion-<version>.zip` (with a `VERSION` stamp file
added at packaging time; `test-harness/` excluded).
