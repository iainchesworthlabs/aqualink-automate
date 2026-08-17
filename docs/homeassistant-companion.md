# Home Assistant companion package

Ready-made Home Assistant content that works out of the box with aqualink-automate:
a pool dashboard, a set of alert and self-healing automation **blueprints**, and a
**helpers package** they build on. It lives in the repository under
[`homeassistant/companion/`](https://github.com/iainchesworthlabs/aqualink-automate/tree/main/homeassistant/companion)
and ships with every release as `aqualink-automate-homeassistant-companion-<version>.zip`.

**Prerequisites:** aqualink-automate running with MQTT and Home Assistant discovery
enabled — `--mqtt --home-assistant` (see [MQTT & Home Assistant](mqtt-home-assistant.md)),
or the [Home Assistant add-on](homeassistant-addon.md), which enables discovery by
default. Once discovery runs, Home Assistant shows an **aqualink-automate** device
under Settings → Devices & Services → MQTT.

!!! note "Why the entity ids just work"
    Home Assistant derives entity ids from the discovery device name, which
    aqualink-automate fixes as `aqualink-automate` — so ids like
    `sensor.aqualink_automate_pool_temperature` are **identical on every install**
    and everything below works unedited. The only install-specific entities are
    your panel's own devices (pumps, aux circuits, chlorinator, heaters), whose
    ids follow your panel labels; the companion content asks for those via
    blueprint inputs or clearly marked placeholders.

## Blueprints

Each blueprint carries the same hygiene scaffold: quiet hours (via the helpers
package), a minimum interval between notifications, guards against
`unknown`/`unavailable` sensors, and actionable message content. Import with the
buttons below (Home Assistant fetches the blueprint straight from this repository's
`main` branch — re-import later to pick up fixes), then create an automation from
it under **Settings → Automations & scenes → Blueprints**.

| Blueprint | What it does |
|---|---|
| **Salt level low** | Below-target salt sends a notification that includes roughly **how many kg of salt to add**, computed from the ppm deficit and your pool volume. |
| **Equipment problem** | Any of the app's six problem sensors (chlorinator fault/warning, salt low, service mode, serial comms loss, stale temperatures) raises a notification; optionally notifies on clear. |
| **App offline** | The app stopping publishing (crash, broker loss, host down) notifies after a grace period; optionally notifies on recovery. |
| **Pump runtime shortfall** | A daily check compares filtration hours against your target — advisory, or (opt-in) turns the pump back on and tells you it did. |
| **Chlorinator self-heal** | Pump running in your chosen window but chlorinator off → turns it back on (optionally only while ORP is low) and notifies. |
| **Swim ready** | The water warming past your swim temperature sends a "pool is ready" note. |
| **Freeze advisory** | Air temperature at the pad dropping to freeze risk sends an advisory (deliberately ignores quiet hours). |
| **Spa ready** | The spa reaching its setpoint sends a "spa is ready" note — reads the *Spa readiness* helper sensor below. |
| **Activity scene auto-off** | Pairs with the *Activity scene* script (below): when its duration timer finishes, turns everything the scene started back off. |

[Import: Salt level low](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fraw.githubusercontent.com%2Fiainchesworth%2Faqualink-automate%2Fmain%2Fhomeassistant%2Fcompanion%2Fblueprints%2Fautomation%2Faqualink-automate%2Fsalt-level-low.yaml){ .md-button }
[Import: Equipment problem](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fraw.githubusercontent.com%2Fiainchesworth%2Faqualink-automate%2Fmain%2Fhomeassistant%2Fcompanion%2Fblueprints%2Fautomation%2Faqualink-automate%2Fequipment-problem.yaml){ .md-button }
[Import: App offline](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fraw.githubusercontent.com%2Fiainchesworth%2Faqualink-automate%2Fmain%2Fhomeassistant%2Fcompanion%2Fblueprints%2Fautomation%2Faqualink-automate%2Fapp-offline.yaml){ .md-button }
[Import: Pump runtime shortfall](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fraw.githubusercontent.com%2Fiainchesworth%2Faqualink-automate%2Fmain%2Fhomeassistant%2Fcompanion%2Fblueprints%2Fautomation%2Faqualink-automate%2Fpump-runtime-shortfall.yaml){ .md-button }
[Import: Chlorinator self-heal](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fraw.githubusercontent.com%2Fiainchesworth%2Faqualink-automate%2Fmain%2Fhomeassistant%2Fcompanion%2Fblueprints%2Fautomation%2Faqualink-automate%2Fchlorinator-self-heal.yaml){ .md-button }
[Import: Swim ready](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fraw.githubusercontent.com%2Fiainchesworth%2Faqualink-automate%2Fmain%2Fhomeassistant%2Fcompanion%2Fblueprints%2Fautomation%2Faqualink-automate%2Fswim-ready.yaml){ .md-button }
[Import: Freeze advisory](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fraw.githubusercontent.com%2Fiainchesworth%2Faqualink-automate%2Fmain%2Fhomeassistant%2Fcompanion%2Fblueprints%2Fautomation%2Faqualink-automate%2Ffreeze-advisory.yaml){ .md-button }
[Import: Spa ready](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fraw.githubusercontent.com%2Fiainchesworth%2Faqualink-automate%2Fmain%2Fhomeassistant%2Fcompanion%2Fblueprints%2Fautomation%2Faqualink-automate%2Fspa-ready.yaml){ .md-button }
[Import: Activity scene auto-off](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fraw.githubusercontent.com%2Fiainchesworth%2Faqualink-automate%2Fmain%2Fhomeassistant%2Fcompanion%2Fblueprints%2Fautomation%2Faqualink-automate%2Factivity-scene-autooff.yaml){ .md-button }

No My-Home-Assistant? Copy the files from
`homeassistant/companion/blueprints/automation/aqualink-automate/` into
`<config>/blueprints/automation/aqualink-automate/` and reload automations.

### Script blueprint: activity scene

Unlike the alert blueprints above, **Activity scene** is a *script* blueprint —
a reusable start/stop scene (swim, spa, jet pool, whatever you like) for any
switches you point it at, with an optional duration timer and dark-only extra
lighting. Create one script instance per activity under **Settings →
Automations & scenes → Blueprints** (it'll ask you to create an `input_boolean`
helper first, if you don't already have one to track that activity). A
dashboard tile's tap action toggles it; its hold action can call the same
script with `turn_off` to force it off. Pair each instance that uses a
duration timer with an **Activity scene auto-off** automation (above) pointed
at the same timer and script, so running out turns everything off
automatically.

[Import: Activity scene (script)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fraw.githubusercontent.com%2Fiainchesworth%2Faqualink-automate%2Fmain%2Fhomeassistant%2Fcompanion%2Fblueprints%2Fscript%2Faqualink-automate%2Factivity-scene.yaml){ .md-button }

No My-Home-Assistant? Copy the file from
`homeassistant/companion/blueprints/script/aqualink-automate/` into
`<config>/blueprints/script/aqualink-automate/` and reload scripts.

## Helpers package

[`packages/aqualink_automate.yaml`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/homeassistant/companion/packages/aqualink_automate.yaml)
provides what the blueprints and dashboard expect:

- `input_datetime.aqualink_quiet_hours_start` / `_end` — the shared no-notification
  window (set your times once under Settings → Devices & Services → Helpers; a
  window wrapping midnight works).
- `input_number.aqualink_pool_volume`, `input_number.aqualink_target_salt_ppm`,
  `input_number.aqualink_pump_runtime_target` — pool volume (litres), the salt
  level you top up towards, and your daily filtration goal.
- `sensor.aqualink_salt_to_add` — kilograms of salt to reach the target
  (`deficit_ppm × volume_L ÷ 1 000 000`).
- `sensor.aqualink_pool_temperature_delta_today` — how much the water has warmed
  since midnight (with a restart-safe midnight baseline sensor behind it).
- `sensor.aqualink_spa_readiness` — `Off` / `Warming Up` / `Ready`, with
  `current_temperature`, `target_temperature`, and a `percent_ready` attribute
  measuring progress from the temperature captured when spa mode last turned on.
  Feeds the **Spa ready** blueprint above.
- Commented `history_stats` recipes for pump-runtime / heater-heating-time
  counters — these reference your panel's own device entities, so uncomment and
  point them at yours.

Install: enable [packages](https://www.home-assistant.io/docs/configuration/packages/)
once in `configuration.yaml`, drop the file into `<config>/packages/`, and restart:

```yaml
homeassistant:
  packages: !include_dir_named packages
```

The blueprints' quiet-hours/target inputs default to these helpers, and the
dashboard's "Salt to add" / "Warmed today" tiles appear automatically once the
package is installed. Everything still works without it — pick different entities
at blueprint-import time instead.

## Dashboard

[`dashboards/aqualink-pool.yaml`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/homeassistant/companion/dashboards/aqualink-pool.yaml)
is a two-view pool dashboard (Overview + Trends) built **only from stock cards** —
no HACS or custom cards required. Water temperatures and setpoints, body switching
on dual-body systems, chemistry, a self-hiding alerts section, and history/statistics
graphs. Cards for entities your system doesn't publish (e.g. spa entities on a
pool-only panel) hide themselves.

Install: **Settings → Dashboards → Add dashboard → Start from scratch**, open it,
enter edit mode, open the three-dot menu → **Raw configuration editor**, and replace
the content with the file. Then personalise the *Equipment* section with your
panel's own switches (commented examples are in the file). Alternatively register
it as a [YAML-mode dashboard](https://www.home-assistant.io/dashboards/dashboards-and-views/)
pointing at the file.

### Showcase dashboard (optional, needs HACS)

[`dashboards/aqualink-pool-showcase.yaml`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/homeassistant/companion/dashboards/aqualink-pool-showcase.yaml)
is a more polished alternative demonstrating what's possible with a few popular
HACS custom cards — [button-card](https://github.com/custom-cards/button-card),
[card-mod](https://github.com/thomasloven/lovelace-card-mod),
[layout-card](https://github.com/thomasloven/lovelace-layout-card),
[lovelace-mushroom](https://github.com/piitaya/lovelace-mushroom), and
[bar-card](https://github.com/custom-cards/bar-card). It's not a drop-in
replacement for the stock dashboard above — install those five via HACS first,
then follow the same install steps. Like the stock dashboard, it uses only
entities the app and the helpers package publish, with commented sections for
your own equipment and any *Activity scene* instances.

## Add-on auto-install (optional)

Running the [Home Assistant add-on](homeassistant-addon.md)? Its
**`install_companion_package`** option (off by default) copies the bundled
blueprints straight into Home Assistant's `blueprints/` folder on every start,
so they appear under **Settings → Automations & Scenes → Blueprints** with no
import step. It requests a read-write view of Home Assistant's own
configuration directory to do this — broader than anything else the add-on
asks for — so it stays opt-in, and it only ever adds or updates files under
`blueprints/`; it never touches `configuration.yaml` or anything else in your
config. See the [Home Assistant add-on's Configuration
section](homeassistant-addon.md#configuration) for details.

## Customisation notes

- **Fahrenheit panels**: temperature *sensors* convert automatically to your Home
  Assistant unit system; blueprint temperature thresholds are plain numbers in
  **your display unit**, so just enter °F values there.
- **Renamed entities**: if you've renamed the app's entities, override the
  blueprint defaults at import time and adjust the dashboard/package references
  to match.
- **Multiple installs**: a second aqualink-automate instance gets `_2`-suffixed
  entity ids from Home Assistant; point a second copy of the content at those.
- The full machine-readable list of the app's fixed entity ids is
  [`entity-manifest.json`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/homeassistant/companion/entity-manifest.json)
  — CI validates the companion content against it, and a unit test keeps it in
  lock-step with the discovery code, so it is always current for your release.

## Getting the files

- **Your running instance** — every install serves its own copy under `/homeassistant/`
  on the app's web port (through Home Assistant ingress on the add-on), matching the
  exact version you're running and requiring no internet access to GitHub:
    - `/homeassistant/blueprints/automation/aqualink-automate/<name>.yaml` — the nine automation blueprints
    - `/homeassistant/blueprints/script/aqualink-automate/activity-scene.yaml` — the script blueprint
    - `/homeassistant/packages/aqualink_automate.yaml` — the helpers package
    - `/homeassistant/dashboards/aqualink-pool.yaml`, `/homeassistant/dashboards/aqualink-pool-showcase.yaml` — the two dashboards
    - `/homeassistant/entity-manifest.json`, `/homeassistant/README.md`

    Handy for air-gapped or LAN-only installs: download from your own instance and
    paste into Home Assistant's raw configuration editor or `blueprints/automation/`
    folder — no My-Home-Assistant redirect or GitHub reachability required.
- **Repository** (tracks `main`): [`homeassistant/companion/`](https://github.com/iainchesworthlabs/aqualink-automate/tree/main/homeassistant/companion)
- **Release bundle** (version-pinned, GPG-signed and attested like every other
  release artifact): `aqualink-automate-homeassistant-companion-<version>.zip` on
  the [releases page](https://github.com/iainchesworthlabs/aqualink-automate/releases) —
  see [Releasing](releasing.md) for signature verification.
