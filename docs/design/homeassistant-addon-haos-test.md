# Home Assistant add-on — HAOS test checklist

*Validation checklist (drafted 2026-07-08) for the first real install of the add-on on
**Home Assistant OS / Supervised**. It collects every "confirm on HAOS" flag raised while
building the add-on (PR #96). Everything here is untestable without a live Supervisor —
this is the pass that clears it. Companion to [homeassistant-addon.md](homeassistant-addon.md).*

## Key risks to watch (unverified until this pass)

These are the specific things authored blind that most need eyes-on:

- ⚠ **Ingress base-path** — the whole web UI under HA's `/api/hassio_ingress/<token>/` prefix
  (assets, `/api/*`, WebSocket). The `base-path.js` shim is the single point of failure.
- ⚠ **MQTT services API** — `bashio::services 'mqtt' …` actually returning the HA broker.
- ⚠ **`uart` device mapping** — the `serial_port` dropdown populating with host TTYs.
- ⚠ **`SUPERVISOR_TOKEN` on a non-s6 image** — bashio reading options/services without
  s6's `with-contenv`.
- ⚠ **bashio install** — the tarball URL/version in the add-on `Dockerfile` fetching + linking.
- ⚠ **Watchdog form** — `watchdog: "http://[HOST]:80/api/health"` (literal container port,
  not `[PORT:80]`) actually being reachable by the Supervisor.
- ⚠ **GHCR package visibility** — the new `homeassistant-{aarch64,amd64}` packages default
  **private**; make them public or the Supervisor's anonymous pull fails.
- ⚠ **AppArmor** — profile name acceptance + enforce-mode denials (separate section below).

## 0. Before you start

- [ ] Host is **HAOS or Supervised** on **aarch64 or amd64** (armv7 is unsupported).
- [ ] Install the **Mosquitto broker** add-on (for the MQTT auto path) and the **Terminal &
      SSH** add-on (for `dmesg` / logs during the AppArmor step).
- [ ] **Image availability — pick one** (the wrapper images only exist after a real release):
  - **A. Cut a prerelease first** so CI publishes `homeassistant-{arch}:<ver>` — then make
    those two GHCR packages **public**, then install (tests the real pull path). *Recommended.*
  - **B. Test an unpublished build:** comment out the `image:` line in `config.yaml`, which
    makes the Supervisor **local-build** the wrapper on-device (tests the Dockerfile/bashio
    install directly). Re-enable `image:` before shipping.

## 1. Install & boot

- [ ] Add `https://github.com/iainchesworth/aqualink-automate` as a custom repository.
- [ ] **Both** add-ons appear: **Aqualink Automate** and **Aqualink Automate (Edge)**, each
      with an *experimental* badge.
- [ ] Install stable; it **pulls a prebuilt image** (quick) — or local-builds if `image:` is
      commented (Dockerfile: apt `jq`, bashio tarball, `run.sh`).
- [ ] Start it. Log tab shows `[entrypoint] starting aqualink-automate` then
      `Starting aqualink-automate...` and the app's own startup lines — no crash loop.
- [ ] No double-init / zombie warnings (tini is PID 1; `init: false` honored). ⚠ confirms
      `SUPERVISOR_TOKEN`/bashio work (options were read to build the argv).

## 2. Serial (uart)

- [ ] `serial_mode: usb` → the **`serial_port` field is a device dropdown** populated with the
      host's TTYs (⚠ `uart` mapping). Pick a `/dev/serial/by-id/...` entry.
- [ ] The app **opens the port** and decodes bus traffic (log shows frames / device discovery),
      no permission error.
- [ ] (If available) `serial_mode: network` + `remote_serial_port host:port` connects.

## 3. MQTT + Home Assistant discovery

- [ ] `mqtt_mode: auto` with Mosquitto running → log: **"MQTT auto-configured from the Home
      Assistant broker."** and the client connects (⚠ services API).
- [ ] Pool entities **auto-appear** in Home Assistant (Settings → Devices & Services → MQTT).
- [ ] `mqtt_mode: auto` with **no** broker → warning logged, add-on keeps running (no crash).
- [ ] `mqtt_mode: manual` + host/port/user/pass connects to the named broker.

## 4. Web UI via ingress ⚠ (the big one)

- [ ] The add-on shows **Open Web UI** and an **Aqualink** sidebar panel (pool icon).
- [ ] Opening it loads the UI **inside Home Assistant** (URL contains
      `/api/hassio_ingress/<token>/`), behind HA login, **not** on the LAN.
- [ ] Browser devtools **Console + Network**: **no 404s** for `styles/`, `scripts/`, `vendor/`,
      `i18n/en.js`, `favicon`, `manifest` (relativised assets resolve under the prefix).
- [ ] Live data flows → the **WebSocket connected** (dashboard updates; no WS errors). This is
      the `wsUrl()` + shim validation.
- [ ] `/api/*` calls succeed (equipment, preferences, health) — the `fetch` rebasing works.
- [ ] Navigate across views (Dashboard/Diagnostics/Trends/…) — hash routing keeps the base right.
- [ ] Switch **language** — the dynamically injected catalog (`i18n/<code>.js`) loads (no
      missing-key warnings).
- [ ] Console shows **no service-worker registration** (skipped under ingress) and no SW errors.

## 5. Opt-in direct LAN port + web UI login

- [ ] Assign a host port to `80/tcp` in the **Network** panel → UI reachable at
      `http://<host>:<port>/` directly (served at root; assets/API/WS all work there too).
- [ ] With `enable_auth: false`, the direct port is **open** (no login) — as documented.
- [ ] Set `enable_auth: true` + `auth_username`/`auth_password` → log: **"Web UI authentication
      enabled (admin: …)"**; **both** ingress and the direct port now require login; you can sign
      in with the bootstrap admin.
- [ ] `enable_auth: true` with **empty** password → add-on **fails fast** with the clear message.
- [ ] `/api/health` still returns 200 **without** auth (watchdog stays valid).

## 6. Watchdog ⚠

- [ ] Add-on stays **healthy** in the Supervisor (no false-positive restarts) — confirms
      `http://[HOST]:80/api/health` resolves to the internal container port.
- [ ] Supervisor log (Settings → System → Logs → Supervisor) shows **no watchdog probe errors**
      for the add-on.
- [ ] (Optional) Stop the app process inside the container and confirm the Supervisor
      **restarts** the add-on.

## 7. Persistence & HA-deferring defaults

- [ ] `history` and `scheduler` are **off** by default (no `history.db`/`schedules.json` in
      `/data`); Matter is **off** (log: "Matter disabled").
- [ ] Toggle `enable_history` / `enable_scheduler` on → `/data/history.db` /
      `/data/schedules.json` are created.
- [ ] Change a **UI preference**, restart the add-on → the preference **survives**
      (`/data/preferences.json`); the dashboard restores fast (`/data/equipment-cache.json`).
- [ ] Update or reinstall the add-on → `/data` state survives.
- [ ] `log_level: debug` → visibly more verbose Log tab.

## 8. Channels (stable + edge)

- [ ] Installing **Edge** pulls the **prerelease**-tagged image; its version differs from stable
      once they diverge.
- [ ] Do **not** run stable + edge **against the same panel** at once (serial-device / MQTT-topic
      contention) — verify the docs' warning holds if you try.

## 9. AppArmor activation (only AFTER 1–8 pass clean)

The profile ships as `apparmor.txt.draft` (**inert**). Activate and tune only once the baseline
above works, so a profile bug is distinguishable from other issues.

- [ ] Rename `aqualink-automate/apparmor.txt.draft` → `apparmor.txt`, run
      `pwsh scripts/gen-homeassistant-edge-addon.ps1` (edge gets it as `aqualink_automate_edge`),
      commit, and update the add-on.
- [ ] The Supervisor **loads** the profile (no "invalid profile / name" error) — ⚠ confirm the
      `profile <name>` value is accepted and stable + edge don't collide.
- [ ] Re-run **§2 serial, §3 MQTT, §4 ingress, §5 auth** under enforce.
- [ ] `dmesg | grep -i 'apparmor.*DENIED'` (via SSH/Terminal) is **empty**. For each denial, add
      the matching rule to `apparmor.txt.draft`, regenerate, re-test — repeat until clean.
- [ ] (Standalone image, optional) `apparmor_parser -r apparmor.txt.draft` +
      `docker run --security-opt apparmor=aqualink_automate …` runs cleanly.

## Sign-off

- [ ] All ⚠ risks cleared; §1–§8 green; AppArmor either enforced-and-clean or consciously left
      staged.
- [ ] `icon.png` / `logo.png` added to both add-on folders (via the generator).
- [ ] Re-enable `image:` if it was commented for local-build testing.
- [ ] PR #96 taken out of draft and merged.

Record results (and any rule additions / doc fixes) back into
[homeassistant-addon.md](homeassistant-addon.md) as the reconciled record.
