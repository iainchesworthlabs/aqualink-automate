# Authentication & Authorization Redesign — Scope & Design

**Branch:** `feat/auth-authz` (off `develop`) · **Status:** scoping (2026-07-02, completeness pass done) · **Design snapshot, not yet implemented.**

This document scopes the move from the current single-shared-token model to a multi-mode
identity + session + authorization system with per-user preferences. It is the north star
for implementation; verify claims against code before relying on any file citation.

---

## 1. Goals (from the requester)

1. **Auth-disabled mode** — no authentication; every feature anonymously accessible.
2. **Authenticated mode** — users must log in.
3. **In-app user management** — local username+password accounts (form login).
4. **External OIDC/OAuth2** — both a trusted reverse-proxy forward-auth mode *and* a full in-app OIDC client.
5. **API keys** — machine credentials for integrations against the HTTP API.
6. **Guest mode** — when auth is ON, anonymous visitors get an admin-configured guest scope
   (which aux/controls are available is admin-chosen); logging in elevates.
7. **Design-language consistency** — build screens with existing `assets/web` patterns.
8. **Per-user preferences** — units, dark mode, accent, chemistry bands stored per user, with global defaults.

## 2. Decisions locked

| # | Decision | Choice |
|---|----------|--------|
| D1 | OIDC delivery | **Both**: trusted-proxy forward-auth **and** full in-app OIDC client |
| D2 | Browser session mechanism | **Stateless JWT** (access) + refresh token + server-side revocation/token-version so logout & disable-user work |
| D3 | Guest / global posture | **3 postures**: auth OFF = all anonymous; auth ON = anonymous→Guest group, login elevates |
| D4 | Local accounts | **Form login**, argon2id-hashed passwords, in-app user CRUD |
| D5 | Authorization model | **ABAC over entitlements** — decisions evaluate subject/resource/action/environment attributes; entitlements (claims/scopes) are the primary subject attribute. **No roles**: people → groups → entitlements (+ optional direct entitlements). Guest/Everyone are built-in groups |
| D6 | Auth UI design | **Follow existing design language** in `assets/web` |
| D7 | Per-user prefs | Units, theme, accent, chemistry bands — **all per-user**; global defaults remain |
| D8 | Dependencies | libsodium (argon2id) + jwt-cpp + OpenSSL (present) via vcpkg |
| D9 | ABAC richness v1 | Entitlement-selector matching + **posture** condition; no admin condition-language in v1 (engine extensible) |
| D10 | Assignment | **Groups + optional direct grants**; effective = `union(group-derived, direct, provider-claim-derived)` |
| D11 | JWT placement | Framework **Slice 1**, local issuance **Slice 2**, external OIDC/JWKS validation **Slice 4** |
| D12 | Store backend | **Atomic JSON files** in secure state dir (parity with prefs/schedules), schema-versioned |
| D13 | View granularity | **Binary view in v1** — `equipment.view` grants all live data; granularity applies to *control* only. No per-subject WS payload filtering in v1 |
| D14 | Schedule authz | Creating/editing a schedule **requires the entitlements for every action it performs** (checked at save). Existing schedules execute as system |
| D15 | Grant propagation | **Immediate** — entitlement/group/disable changes bump the user's `tokver`; next request re-issues or rejects. Effect within one request |
| D16 | In-scope extras | **Session/device list** (Slice 2) and **kiosk PIN** (Slice 3). TOTP 2FA + WebAuthn/passkeys = backlog; design must not preclude them |
| D17 | Audit destination | New **Audit logging channel** routed to **OS-native logging** (syslog/journald on Linux, Windows Event Log) + a local structured JSONL file in the state dir (feeds the in-app audit viewer; also the fallback when no OS sink) |
| D18 | OIDC secret at rest | Plain file, 0600, inside the hardened state dir (encryption-at-rest deferred) |
| D19 | Slice order | 1 Frameworks → 2 Users/API-keys → **3 Guest mode** → 4 OIDC → 5 Forwarded auth |

**Defaults taken** (change if wrong): API keys are entitlement-scoped, hashed at rest, shown once,
revocable, last-used tracked, **HTTP API only**; legacy `--api-auth-token` continues to work as a
bootstrap API key; existing global prefs file auto-migrates; HTTPS hard-required when auth is enabled
on a non-loopback bind; single OIDC provider in v1.

## 3. The three-layer model

Auth is **not** "5 modes"; it is three orthogonal layers:

1. **Identity providers** (how a human logs in): `local` accounts · `oidc` (in-app) · `proxy` (forward-auth header).
   One or more may be enabled; the login screen offers the enabled paths.
2. **Machine credentials**: API keys, always available when auth is ON, independent of the human login path.
   The legacy static token folds in here.
3. **Authorization**: every request resolves to a **subject** (anonymous/Guest, user, or api-key)
   carrying **entitlements**; a policy engine decides Permit/Deny.

Per-user preferences = preferences keyed by subject identity, falling back to global defaults.

### Global posture (D3)

- **Auth OFF** — no identity resolved; subject is "root-anonymous" with all entitlements; per-user prefs disabled (global/localStorage only). This is today's "no token" behavior, made explicit.
- **Auth ON** — anonymous request resolves to the **Guest** group (deny-by-default, admin-configured). A valid session/key elevates to that subject's entitlements.

## 4. Authorization: ABAC over entitlements (D5, D9, D10)

**No roles.** Access decisions are made by a policy engine (PDP) that evaluates **attributes** of the
request: subject, resource, action, and environment. **Entitlements** (structured claims/scopes) are the
primary subject attribute. Assignment flows **person → group → entitlements** (+ optional direct
entitlements + provider-claim-derived entitlements). Groups are an *assignment convenience*, never the
decision unit — the PDP never branches on a group/role name.

### Entitlements (the vocabulary — v1 set, confirmed)

Structured scope strings: `<domain>.<action>[:<resource-selector>]`. Selectors support `*`. The selector
syntax allows finer grain later without schema change.

- `system.admin` — manage users, groups, entitlements, API keys, auth config, system prefs
- `equipment.view` — read state, temps, chemistry, history, diagnostics (**binary in v1**, D13)
- `equipment.control.aux:*` / `equipment.control.aux:AUX3` — all aux, or one specific aux
- `equipment.control.heater` · `.setpoints` · `.circulation` · `.chlorinator` · `.iaq` · `.spaside`
- `schedules.view` / `schedules.edit` (subject to D14 action-entitlement gating)
- `diagnostics.view`
- `prefs.self` — manage own preferences

### Subject attributes & resolution

A **subject** carries: identity (`sub`, or anonymous), **effective entitlements** =
`union(direct, group-derived, provider-claim-derived)`, group memberships, provider, `authenticated?`.
Effective entitlements are computed at login and **embedded in the JWT** as the `ent`/`scope` claim
(§5) so they are self-describing and directly assertable. Built-in groups: **Everyone** (baseline),
**Guest** (the anonymous subject's group when auth is ON), **Administrators** (`system.admin`).

### Policy engine (PDP)

`PolicyEngine.Decide(subject, action, resource, env) → Permit | Deny`, **default deny**. A request is
permitted when the subject holds an entitlement whose action matches and whose **resource-selector
matches the resource's attributes** (e.g. aux id), subject to the **posture** environment condition
(extensible to time-of-day, source, etc. later — D9). Decisions are attribute-driven, not name-driven,
while the common case (a scoped entitlement match) stays cheap.

### Schedule gating (D14)

Schedules run as system, so `schedules.edit` alone would be an escalation path (schedule the heater you
can't control). Rule: **on save, every action a schedule performs is PDP-checked against the saving
subject**; save is rejected if any action would be denied. Existing schedules continue to execute.

### Enforcement change (the core refactor — Slice 1)

Today `EvaluateSecurity()` is a single all-or-nothing gate in
[`routing.cpp`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/src/core/http/server/routing/routing.cpp). New flow:

1. **Subject resolution** (new middleware): session JWT / API key / proxy header / anonymous → `Subject { id, entitlements, groups, attrs }`, attached to request context.
2. **Coarse gate**: keep rate-limit, Origin, CSRF (see §5); rate-limiter gains per-account tracking for login and **trusted-proxy-aware client-IP** (X-Forwarded-For honored only from the configured trusted CIDR — otherwise every client behind a proxy shares one IP bucket).
3. **Per-route authorization via PDP**: each route declares the `(action, resource-descriptor)` it needs; the handler resolves resource attributes (e.g. aux id from the path) and calls `PolicyEngine.Decide(...)`. Extends the per-route `RequiresAuthentication()` hook in [`iwebroute.h`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/src/core/interfaces/iwebroute.h) into a `RequiredAccess()` (action + resource-kind) concept.
4. **Response filtering**: control affordances gated client- **and** server-side by PDP decisions. New **`/api/auth/me`** returns the resolved subject + effective entitlements so the SPA can gate affordances (lock icons, hidden admin nav) from one source of truth.

## 5. Sessions & tokens (D2, D11, D15)

- **Access token**: short-lived JWT (e.g. 15 min), signed with a key in the secure state dir. Carries `sub`, `tokver`, `iss`/`aud`, and **entitlements as claims/scopes** (`ent`/`scope`) — the resolved set (§4) is embedded so authorization is self-describing and directly assertable in tests. OIDC scopes/group-claims and proxy headers map → groups → entitlements; the same claim shape is produced regardless of provider, so the PDP and tests are provider-agnostic.
  - **Size budget / overflow rule**: large ABAC grants (dozens of per-aux selectors) can approach the ~4KB cookie ceiling. Entitlements-in-token is the fast path; if the encoded set exceeds the budget, the token instead carries groups + an entitlement-set version and the server resolves entitlements from its stores. Both paths tested.
- **Refresh token**: longer-lived, opaque, server-recorded; rotates on use; browser transport is a cookie path-scoped to the refresh endpoint.
- **Revocation & propagation (D15)**: per-user `tokver` bumped on logout-all / password change / disable / **any entitlement or group change** → the next request with a stale access token re-validates against the store and silently re-issues (or rejects, if access was revoked). Plus a short refresh denylist. Grant changes take effect within one request despite stateless access tokens.
- **Session/device list (D16)**: the refresh-token store records device/user-agent/last-seen per session → "active sessions" view with per-session revoke ("log out that browser").
- **Key management**: signing keys carry `kid`; rotation keeps old + new valid during a grace window. Token validation tolerates bounded clock skew — embedded Pi deployments boot with wrong clocks (no RTC), so validation must behave sanely until NTP sync (documented startup grace).
- **Transport**: JWT in `Authorization: Bearer` for API clients; for browsers, cookie (HttpOnly+Secure+SameSite=Strict) — which **reintroduces CSRF**, mitigated by SameSite + the existing CSRF-header option. WebSocket: browser cookie carries auth (drop the `bearer.<token>` subprotocol hack for browser sessions) but keep subprotocol for API-key WS clients.
- **WebSocket lifetime**: sockets outlive tokens. The server tracks each connection's subject; on `tokver` bump, revocation, or access-token expiry the socket is closed (policy code) and the client reconnects with fresh credentials.
- **HTTPS**: hard-required when auth enabled on non-loopback bind.

## 6. Identity providers (D1, D4)

- **Local** — argon2id (libsodium) password hashing; login form; user CRUD; group membership; first-run setup screen when no users exist; `--bootstrap-admin` for headless/container (env/file-sourced secret — see §10).
  - **Hashing off the kernel thread**: argon2id is deliberately slow; the app's cooperative single-threaded loop also drives RS-485. Password hash/verify runs on a worker offload with async completion — **never inline on the protocol loop**.
  - **Password lifecycle**: change-own-password; admin reset (no email infra → no self-service forgot-password); minimum length 12 (no composition rules); all sessions invalidated on change (`tokver`).
  - **Account lockout**: per-account backoff/lockout on failed logins in addition to per-IP limiting (usernames are enumerable; per-IP alone fails behind proxies).
  - **Last-admin protection**: cannot delete/disable the last `system.admin` holder or remove their admin grant.
  - **Deletion semantics**: delete user → prefs file removed, refresh tokens revoked, audit entries retained (keyed by user-id).
  - **Bootstrap semantics**: `--bootstrap-admin` is an idempotent no-op when any user exists.
- **OIDC (in-app)** — discovery, authorization-code + PKCE, JWKS validation (OpenSSL), redirect handling; configurable issuer/client-id/secret/redirect; **group/claim → group → entitlements** mapping; auto-provision local user record on first login. v1 bounds: **single provider**; discovery/JWKS cached with a refresh interval; **no RP-initiated logout** (local logout only); document redirect-URI registration for LAN/self-signed deployments.
- **Proxy forward-auth** — trust `Remote-User` / configurable header (+ configurable groups header, e.g. `Remote-Groups`) **only** when the request arrives from the configured trusted-proxy CIDR; spoofed headers from outside the CIDR are rejected. Map header/group-claim → group → entitlements. The same CIDR governs `X-Forwarded-For` trust for rate-limiting and audit identity.
- **Kiosk PIN (D16, Slice 3)** — wall-tablet quick-elevation: a short admin-configured PIN elevates the anonymous Guest session to a designated group (e.g. "Household"). PIN attempts are rate-limited/locked out like passwords; PIN sessions are ordinary JWT sessions (revocable, listed in the session view).

## 7. API keys

- Generated server-side, **shown once**, stored hashed. Each key carries entitlements (directly or via group), optional expiry, and last-used timestamp. Revocable. Governs the **HTTP API only**. Legacy `--api-auth-token` = a pre-seeded bootstrap key.

### Trust boundary — integration channels

MQTT/Home-Assistant and Matter command paths **bypass the PDP by design**: they are admin-configured
machine integration channels secured at their own layer (broker credentials/TLS, Matter commissioning).
This is deliberate and documented in `docs/SECURITY.md`; anyone who can publish to the command topics or
operate the commissioned fabric has full control regardless of HTTP-side entitlements.

## 8. Preferences: global vs per-user (D7)

- **Per-user** (server-synced when logged in; localStorage fallback/first-paint for anonymous): temperature units, theme, accent, chemistry display bands.
- **System/admin** (global, `system.admin` only): alert thresholds, salt-low, comms-timeout, webhook URL, history retention, label overrides, spa-switch mapping.
- **Model**: split [`PreferencesHub`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/src/core/kernel/preferences_hub.h)/[`PreferencesService`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/src/core/preferences/preferences_service.cpp) into a **system** store (existing global file) and a **per-user** store keyed by user id. `GET/PUT /api/preferences` becomes subject-aware (returns merged global-defaults + user overrides). Theme/accent stores in `assets/web/scripts/stores/` gain a server-sync path for logged-in users.

## 9. UI surface (D6)

All screens follow the existing `assets/web` design language (Alpine.js stores + views):

| Screen | Notes |
|---|---|
| Login | Provider-aware: local form, "Sign in with <IdP>" (OIDC), auto (proxy); kiosk PIN entry when enabled |
| First-run setup | Wizard when no users exist and auth ON: create first admin |
| Account menu | Current user, change password, active sessions (per-session revoke), logout |
| Users & groups admin | User CRUD, group membership, disable/enable |
| Entitlement editor | Group ↔ entitlement assignment; per-aux picker for `equipment.control.aux:` selectors; direct grants |
| API keys admin | Create (shown-once), entitlement scope, expiry, last-used, revoke |
| Auth settings | Posture, provider enable, OIDC config, proxy config, kiosk PIN |
| Guest scope editor | What the Guest group may control (deny-by-default; per-control + per-aux) |
| Per-user prefs panel | Units, theme, accent, chemistry bands (existing settings view, made subject-aware) |
| Audit viewer | Filterable list over the local audit file |

Affordance gating is driven by `/api/auth/me` (lock icons / hidden controls for unentitled actions),
**always enforced server-side too**. The service worker must **never cache authenticated API responses**
(verify current network-first SW; add an explicit test).

## 10. Persistence, audit, config

### Persistence layout (D12)

Reuse `SecureRuntimeStateDirectories()` / `PrepareSecureDirectory()` (0700, owner-only, symlink-rejecting).
All stores are atomic-write JSON with a **schema-version field + migration hook**.

```
<state-dir>/auth/
  jwt-signing.key            # generated, 0600, kid-tagged; rotation keeps old+new
  users.json                 # id, username, argon2 hash, groups, direct entitlements, tokver, disabled
  groups.json                # group → entitlement set (+ built-in Everyone/Guest/Administrators)
  api-keys.json              # id, hash, entitlements/group, expiry, last-used
  oidc.json                  # issuer/client-id/secret/redirect/group-claim-map (0600 — D18)
  kiosk.json                 # kiosk PIN (hashed) → group mapping
  sessions/                  # refresh-token records (device/UA/last-seen) + denylist
<state-dir>/prefs/
  global.json                # migrated from current preferences-file
  users/<uid>.json           # per-user overrides
<state-dir>/audit/
  audit.jsonl                # structured audit log (rotated) — feeds the audit viewer
```

### Audit (D17)

New **Audit** channel in the logging facade. Sinks: **OS-native logging** — syslog/journald on
Linux/POSIX, **Windows Event Log** on Windows — plus the local structured JSONL file (which feeds the
in-app audit viewer and is the fallback when no OS sink is available). Audited events: **control actions**
(who did what to which resource, decision) **and auth events** — login success/failure, lockouts,
token/key issuance + revocation, entitlement/group changes, posture changes.

### Config surface (new/changed CLI + config keys)

- `--auth-mode` `disabled|enabled`
- `--auth-provider-local` / `--auth-provider-oidc` / `--auth-provider-proxy` (enable flags)
- OIDC: `--oidc-issuer`, `--oidc-client-id`, `--oidc-client-secret`, `--oidc-redirect-uri`, `--oidc-group-claim`
- Proxy: `--auth-proxy-header`, `--auth-proxy-groups-header`, `--auth-proxy-trusted-cidr`
- `--bootstrap-admin` (username) + `--bootstrap-admin-password-file` / env var — **never a bare CLI password** (process-list visible)
- `--auth-state-dir` (override), `--jwt-access-ttl`, `--jwt-refresh-ttl`
- Retain: `--api-auth-token` (bootstrap key), `--api-allowed-origin`, `--api-require-csrf-header`, `--insecure-no-auth`, TLS/bind options
- **Secret redaction**: any options-dumping surface (e.g. `/api/diagnostics/options`) must mask secret-typed options (`--oidc-client-secret`, `--api-auth-token`, bootstrap password) — Slice 1 requirement.
- Follow the options pipeline in `CLAUDE.md` (§Options) for each new flag; add tests under `test/unit/options/`.

Auth config becomes **partly runtime-managed state** (users/groups/keys/OIDC managed via admin UI), not all CLI.

## 11. Migration & back-compat

- `--api-auth-token` set + no users → keep working as a bootstrap API key; no behavior break.
- Existing global preferences file → auto-migrate into `prefs/global.json`.
- Auth OFF remains the default → existing deployments unchanged until they opt in.
- Auth state is included in backup/restore guidance (documented in `docs/configuration.md`).

## 12. Testing strategy — full-surface coverage

Testing is a first-class deliverable: **every mode, every provider, every subject type must be exercised**,
with unit + e2e coverage merged into the existing SonarCloud new-coverage gate (unit + Playwright-e2e).

### Mock IdP / provider harness (test-only)

- **Mock OIDC provider fixture** — a test-only in-process server exposing a static discovery document,
  JWKS, authorization endpoint, and token endpoint, issuing tokens with controllable claims/scopes/groups.
  Lets the in-app OIDC client (discovery → PKCE → JWKS-verify → claim-map) run deterministically with no
  network/real IdP. Signing keys are fixtures so JWKS verification is exercised end-to-end.
- **Mock trusted-proxy** — inject `Remote-User`/configured header (and spoof-attempt from an untrusted
  source, to prove the trusted-CIDR gate rejects it).
- **Local** — seeded users/groups fixtures; argon2id verify path exercised (with a fast test cost factor).
- Follow the `MockReplayHarness` precedent (drive the real stack with synthetic inputs; no hardware/network).

### Entitlement assertions

Because entitlements are embedded as claims (§5), tests assert the **resolved entitlement set** directly on
the issued token for each provider — proving `OIDC scopes/groups → group → entitlements`,
`proxy header → group → entitlements`, and `local user groups + direct grants → entitlements` all
converge on the same shape — plus **PDP decision tests** (attribute-driven Permit/Deny incl. per-aux
selectors and posture conditions) and **token-overflow-path tests** (entitlements-in-token vs server-resolved).

### Unit coverage

Password hashing/verify (off-thread) · JWT sign/verify/expiry/`kid` rotation/clock-skew/`tokver` ·
subject resolution from **each** source (local session, API key, OIDC, proxy, kiosk PIN, anonymous) ·
PDP evaluation incl. per-aux selectors (allow/deny) · schedule action-gating (D14) · group/entitlement CRUD ·
lockout + rate-limit (per-account, per-IP, XFF-trusted) · prefs merge (global defaults + per-user overrides) ·
audit sink routing · secret redaction · every new option + config key.

### E2E matrix (Playwright)

Cross-product of **global posture × provider × entitlement profile**, asserting both allow and deny:

| Posture | Providers | Subjects to cover |
|---|---|---|
| Auth OFF | — | anonymous = full access to all features |
| Auth ON | local, OIDC (mock), proxy (mock), kiosk PIN | Administrators, standard user, guest (deny-by-default + per-aux grant), anonymous→Guest, API key |

Flows: first-run setup; login/logout per provider; login **elevation** from guest (incl. kiosk PIN);
session expiry + refresh rotation; **immediate grant propagation** (revoke entitlement → next action 403);
disable-user/logout-all (revocation, incl. WS close); session list + per-session revoke; per-user prefs
persist & isolate between users; admin user/group/key management; guest per-aux grant reflected in UI
affordances **and** enforced server-side; SW never serves a cached authenticated API response.

### Full-surface control matrix

Every control route (buttons/aux, heater, setpoints, circulation, chlorinator, iaq, spaside, schedules,
diagnostics, users/groups/keys/oidc admin) tested against **every subject type** — asserting the exact
PDP gate (2xx when entitled, 401/403 when not). This is the "full surface is tested" guarantee:
a route added without a `RequiredAccess()` declaration should fail a coverage/enforcement test.

### Docs to update (per CLAUDE.md)

`docs/SECURITY.md` (auth model + trust boundary), `docs/configuration.md` (all new flags + backup),
`docs/usage-and-api.md` (auth routes + WS), `assets/web/api/swagger.yaml` (security schemes +
`/api/auth/*` incl. `/api/auth/me`, `/api/users`, `/api/groups`, `/api/entitlements`, `/api/apikeys`,
`/api/oidc`, `/api/sessions`, `/api/audit`).

## 13. Slices (delivery units — D19 order)

Work proceeds slice by slice; **each slice ships its own unit + e2e coverage** (§12) and merges into the
SonarCloud gate. No slice merges without covering its part of the full-surface matrix.

- **Slice 1 — Frameworks / patterns / structural updates.** The ABAC substrate with **no real credential
  providers yet** (behaviour-preserving: auth OFF = anonymous-all; legacy `--api-auth-token` still works).
  - Subject model + request-context resolution middleware.
  - Entitlement vocabulary + group model + PDP (`PolicyEngine.Decide`) with default-deny + posture condition.
  - **JWT framework**: signing-key management (`kid`, rotation, skew), claim schema (`ent`/`scope`, size-overflow rule), sign/verify — issuance wired in Slice 2.
  - Routing-gate refactor: single gate → per-route `RequiredAccess(action, resource-kind)` + PDP call; resource-attribute resolution (e.g. aux id) on every control route; `/api/auth/me`.
  - Rate-limiter evolution (per-account hooks, trusted-proxy XFF); secret redaction on options surfaces; Audit channel + sinks; worker-offload pattern for slow crypto.
  - Global posture wiring (auth OFF/ON), config scaffolding, and the **mock-IdP + mock-proxy test harness** so every later slice tests against it.
- **Slice 2 — User/password + API tokens.** Local accounts (argon2id off-thread), groups + entitlement
  assignment, first-run setup, form login issuing session JWTs (access+refresh+`tokver` revocation,
  immediate propagation), logout/disable-user, password lifecycle + lockout + last-admin protection,
  **session/device list**, API keys (hashed, shown-once, revocable, entitlement-scoped), legacy-token
  fold-in. Per-user preferences land here (split system/user stores, subject-aware `/api/preferences`,
  theme/accent/units/bands sync). Schedule action-gating (D14). WS lifetime policy.
- **Slice 3 — Guest mode.** ✅ *Delivered.* Admin-configured Guest group (deny-by-default), per-aux/per-control
  grants, anonymous→Guest resolution + login elevation, client- and server-side affordance gating, guest-scope
  admin UI, **kiosk PIN** elevation.
  - Guest browsing "turns on" precisely when the admin grants the Guest group at least `equipment.view`
    (`/api/auth/me` carries the Guest scope; an empty scope keeps today's login-wall behaviour). The SPA
    then boots as a guest with a persistent "Sign in" affordance; control affordances render locked
    (`auth.can(...)`) with the server PDP still the enforcement point.
  - **Kiosk PIN** (`KioskStore`/`KioskService`, `kiosk.json`): argon2id-hashed PIN → a session in the
    admin-chosen target group. PIN sessions are ordinary JWTs (`prv=KioskPin`), revocable and session-listed,
    validated by the resolver against the kiosk store's enabled flag + `TokenVersion` (no user record, no
    `prefs.self`). Endpoints: `POST /api/auth/pin` (public login), `GET|PUT|DELETE /api/kiosk` (system.admin).
- **Slice 4 — OIDC / OAuth2 / (external) JWT.** In-app OIDC client (discovery, auth-code + PKCE, JWKS
  validation, redirect), validation of external tokens, group-claim → group → entitlement mapping,
  auto-provision, OIDC settings UI.
- **Slice 5 — Forwarded auth (proxy).** Trusted reverse-proxy forward-auth: header extraction
  (`Remote-User`/configurable + groups header), trusted-CIDR gate + spoof rejection, header → group →
  entitlements, XFF-aware rate-limit/audit identity, proxy settings UI.
- **Cross-cutting (folded into the relevant slice):** audit events, TLS-required enforcement for auth
  off-loopback, doc + swagger updates.

## 14. Decision log — resolved & remaining

**Resolved 2026-07-02 (scoping):** D1–D12 (see §2).
**Resolved 2026-07-02 (completeness pass):** D13–D19 (see §2), plus: password lifecycle/lockout/last-admin
(§6), bootstrap secret sourcing (§10), audit scope incl. auth events (§10), MQTT/Matter trust boundary (§7),
JWT size-overflow rule + key rotation + clock skew (§5), WS lifetime policy (§5), schedule gating (§4),
`/api/auth/me` (§4), argon2 off-thread (§6), XFF/proxy-aware rate limiting (§4/§6), secret redaction (§10),
SW no-cache-auth rule (§9), OIDC v1 bounds (§6), backup guidance (§11).

**Remaining (open):**
- Verify during Slice 1: exact behavior of `/api/diagnostics/options` (redaction gap), current SW caching
  of API responses, availability of an async HTTP client for the OIDC token/JWKS calls (webhook sink may
  already provide one), and the threading model available for the crypto offload.
- Backlog (explicitly out of scope, not precluded): TOTP 2FA, WebAuthn/passkeys, granular view filtering,
  admin-authored ABAC condition rules, multiple OIDC providers, RP-initiated logout, encrypted-at-rest OIDC secret.
