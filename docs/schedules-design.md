# Schedules — design philosophy (two tiers, one view)

Aqualink-Automate manages schedules at **two tiers** that appear together in one unified
Schedules view. The tiers exist because they make different trade-offs; the UI's job is to
present them as one coherent timeline and to move a schedule between tiers when it makes sense.

## Tier 1 — Controller schedules (the resilient baseline)

The controller's own built-in program timers (Program Groups A/B). These are the "must always
work" essentials — the filter-pump loop, heating windows — and they keep running even if the
app host, network, or MQTT broker falls over. The app **reads** them off the wire (IAQ /
OneTouch Program pages; see [iaq_schedule_protocol.md](iaq_schedule_protocol.md)), and will
**edit / manage** them and help **promote** app schedules down into them.

They are deliberately limited by the hardware:

| Aspect | Controller capability (owner manual §6.4, §6.11–6.13) |
|---|---|
| Action | on/off span for one equipment item only (no setpoints/%/mode) |
| Days | **all / weekdays / weekends / a single day** — no arbitrary multi-day |
| Time | 12-hour (AM/PM) on the panel; whole-minute on/off |
| Per device | multiple programs allowed ("PGM 1 of 3") |
| Program Groups | two sets **A / B**, only one active; per-group **custom label** |
| Auto-switch | optional: a start date `mm/dd` for A and for B flips the active group by season |
| VSP pumps | speed sub-programs nested inside the filter-pump cycle (advanced; deferred) |

Only the **active** group's programs are observable passively; reading the other group would
require switching the active group (a write that changes controller behaviour), so a read
exposes the active group and reports that the other exists.

## Tier 2 — App schedules (rich / exotic)

The app's own `SchedulerService` (`--schedules-file`, `/api/schedules`) fires
`ICommandDispatcher` actions on a wall clock. This is where flexibility lives: one-off "party"
schedules, specific dates, arbitrary day combinations, reactive/conditional triggers, and
actions beyond on/off — setpoints, chlorinator %, circulation mode (already in
`Scheduling::ActionType`). App schedules are **not** constrained by the controller's limits;
only *promotion* is.

## Moving between tiers

- **Promote (app → controller):** push an app schedule down so it survives an app outage.
  Allowed only when it fits the controller's constraints (single equipment on/off, an
  expressible day selection, a free program slot / group). The UI marks which app schedules
  are promotable and surfaces exactly what is lost or constrained when they are not (e.g.
  "Mon+Wed+Fri isn't representable — the controller allows a single day or weekdays").
- **Adopt (controller → app):** pull a controller program up into the app to edit it with the
  richer app model (then optionally re-promote).

## The unified view

The existing Schedules view (list + 24-hour timeline) renders both tiers, each row labelled by
source/tier, with conflict flagging across the merged set. Controller rows come from
`/api/controller/schedules` (`{status, schedules[]}`); app rows from `/api/schedules`.

## Roadmap

1. **Read controller schedules** (IAQ first, then OneTouch) → `ControllerScheduleStore`
   `available`; model program group (A/B + active) and the day constraint. *(in progress)*
2. Surface tier + source + group in the unified view; conflict flagging across tiers.
3. **Promote** app→controller (constraint checker + the write handshake, once the create/edit/
   delete button-press flow is fully decoded).
4. **Adopt** controller→app; auto-switch + custom-label read/display; VSP speed sub-programs.
