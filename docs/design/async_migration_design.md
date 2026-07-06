<!--
  Generated 2026-06-18 by a multi-agent analysis workflow (8 subsystem surveys ->
  design synthesis -> 3 adversarial critiques -> merged roadmap). ANALYSIS ONLY:
  no production code was changed by this work (the only code change in the
  originating branch is the vcpkg submodule bump to 2026.06.01).

  RECONCILED 2026-06-30: this is an analysis SNAPSHOT, reconciled with the code on
  2026-06-30. Since the 2026-06-18 capture the MQTT client was rewritten onto the
  async-mqtt library (2026-06-19) — so §8.2's "hand-rolled MQTT / no library" premise
  is now FALSE and has been corrected — and src/aqualink-automate.cpp grew ~120 lines,
  so every absolute file:line citation had drifted. This pass corrected §8.2,
  re-anchored the Section 5 / Section 6 / Key-files line numbers to durable SYMBOL
  anchors, and fixed two path/line facts (websocket header path, replay_paced line).
  Treat any remaining bare ":NNN" as approximate.
-->

> Companion to the [async migration roadmap](async_migration_analysis.md) (the authoritative plan).
> This is the pre-critique architecture synthesis the roadmap refers to as "the design".
> See also the [per-subsystem survey](async_migration_survey.md).

# Target Async Architecture & Per-Subsystem Conversion Plan

aqualink-automate — migrating from the manual cooperative `Poll()` frame loop to a fully async boost::asio C++20-coroutine design.

> This is an analysis document — no code was edited. The `file:line` citations were verified against source as of the 2026-06-18 capture; they have since been RECONCILED with the code on 2026-06-30 (see the banner above). Treat bare `:NNN` line numbers as approximate and prefer the symbol anchors.

---

## 1. The central decision: which concurrency model

**Recommendation: Option (A) — a SINGLE `io_context` on ONE thread, driven by `io_context::run()`, with every long-running subsystem `co_spawn`'d as a coroutine.** Treat multi-threading (Options B/C) as a *separate, later, profiling-gated* effort, not part of this migration.

### Why (A), decisively

The single-thread cooperative invariant is **not incidental — it is the application's concurrency model**, and it is load-bearing in code that has *no other synchronisation whatsoever*. Verified instances:

- `HubLocator` is an unsynchronised `std::unordered_map<std::type_index, shared_ptr<void>>` whose own doc-comment states it "is constructed and consumed on the single cooperative poll() thread" (`src/core/kernel/hub_locator.h:33-34,78`).
- `WebSocket_Equipment::m_Connections` is "intentionally unsynchronised … If a multi-threaded execution model is ever reintroduced, m_Connections must be guarded before concurrent access" (`src/core/http/websocket_equipment.h:52-61`).
- `MessageGeneratorRegistry::m_Generators` is "intentionally unsynchronised" relying on the single poll() thread (`src/core/protocol/message_generator_registry.h`).
- `Restartable::s_Instances` is a process-global `std::vector<Restartable*>` mutated from every device ctor/dtor with no lock (`src/core/devices/capabilities/restartable.cpp:14,21,32`).
- DataHub/EquipmentHub/StatisticsHub/PreferencesHub mutate plain non-atomic members and fire `boost::signals2` signals **synchronously, inline, on the writer's stack** (e.g. `data_hub.cpp` PoolTemp setter → `ConfigUpdateSignal`). There is not a single `std::mutex`/strand/atomic under `src/core/kernel` — the invariant is purely structural.

The signals2 fan-out is the crux: a DataHub write on the protocol-decode path **synchronously runs** the WebSocket-broadcast slot, the MQTT publish-queue slot, the HistoryService SQLite-insert slot, and `AlertMonitor::EvaluateAll` — all inline. Default signals2 is thread-safe only for *connect/disconnect-vs-invoke*; concurrent **invoke** from two threads with slot bodies that mutate `m_Connections` / MQTT queues / SQLite buffers is not made safe by signals2. So any move to a multi-threaded `run()` introduces data races across *the entire consumer graph at once*, silently.

The workload does not justify the risk. RS-485 is inherently serial and low-rate; the hardware is modest/embedded-ish. **The win from this migration is latency and simplicity, not throughput** — removing the up-to-1ms egress/dispatch latency the frame loop adds, and replacing five hand-rolled poll state machines with linear coroutines. A single-threaded `run()` delivers *all* of that while preserving every no-lock invariant for free: handlers still serialise on one thread, exactly as today.

**What (A) changes vs. keeps:**

- **Removes:** the `while (!shutdown)` frame loop (`src/aqualink-automate.cpp`, ~lines 867-943), the two per-frame `io_context.poll()` drains (~881 & ~921), the idle `sleep_until(frame_start + frame_period)` pacing (~936), and the four `Poll()` methods (`ProtocolTask::Poll`, `HttpServer::Poll`, `MqttClient/MqttHub/MqttIntegration::Poll`, `Restartable::PollAll`).
- **Keeps unchanged:** the hubs, every signals2 emit site, every slot, the static per-message-type signal dispatch, the `MessageGeneratorRegistry`, the circular-buffer framing, the `scoped_connection` lifetime model — none of it needs touching, because the executor stays single-threaded.

### Why not (B) multi-threaded + strands

To keep correctness you would bind *all* hub access **and every signal-driven consumer slot** to one shared `hub_strand`. That re-serialises the exact work the threads were meant to parallelise, negating the benefit, while adding `co_await dispatch(hub_strand)` churn to ~30 webroute handlers, every device status processor, and every consumer slot — a large, regression-prone change for a throughput win the workload does not need. The reentrancy hazard is real: `AlertMonitor::EvaluateAll` re-reads the DataHub *from inside* the `ConfigUpdateSignal` slot it is connected to; a naive `shared_mutex` on setters would self-deadlock (strands avoid the deadlock but only because slots never block).

### Why not (C) hybrid

(C) — primary strand for hub+protocol, worker pool for blocking I/O — is the *correct eventual destination* for the genuinely-blocking pieces (SQLite, file writes, static-file `open/stat`). But adopting it now forces cross-executor hops for those subsystems before the simpler win is banked. **Adopt (A) first; layer in (C)'s worker pool only for the handful of blocking-I/O call sites, behind awaitables, once the frame loop is gone.** See §9.

> **House rule going forward:** a doc-comment that asserts single-thread safety (`hub_locator.h:33`, `equipment_hub.h:82`, `device_graph.h:161`, `websocket_equipment.h:52`, `mqtt_client.h:43-55`) is a **contract**. Under (A) it stays true. If anyone ever introduces a second `run()` thread, these comments become the checklist of things that must first be strand-confined — they must be converted to a `BOOST_ASSERT(strand.running_in_this_thread())`, never silently deleted.

---

## 2. Executor & threading model

```
                       ┌─────────────────────────────────────────────┐
                       │  io_context (ONE thread: main → run())       │
                       │                                              │
  serial bytes ───▶ SerialReadLoop ─┐                                 │
                                     ├─ signals2 (inline) ─▶ hubs ─▶ consumer slots
  outbound ◀── SerialWriteLoop ◀─ channel ◀── ConnectWriteSignal      │
                                                                      │
  WatchdogLoop (1 steady_timer)   HistoryLoop  SchedulerLoop          │
  MqttSupervisor → {Reader,Writer,Keepalive}   EquipmentCacheLoop     │
  HttpListener → per-session coro → {WsReader, WsWriter(channel)}     │
  JandyStartupLoop  AlertCommsLoop  WebhookSink(co_spawn detached)    │
  PerDeviceNavCoro (OneTouch/PDA)                                     │
                       └─────────────────────────────────────────────┘
```

- **One executor, one thread.** `make_strand` is **not** needed in (A) — everything already serialises on the io_context thread. (Strands are the (B) escape hatch, documented but not used.)
- **Hubs & signals2 stay exactly as they are.** The mutate-then-emit pattern, inline slot execution, and `scoped_connection` lifetime model are all preserved verbatim because there is still only one thread.
- **Blocking I/O is the one place (C) leaks in:** SQLite and file writes get a dedicated `asio::thread_pool` (size 1 = a serialising DB/file executor); the owning coroutine does `co_await asio::post(db_pool, asio::use_awaitable)` to hop over, does the blocking call, then hops back. This keeps the io_context thread free without exposing the hubs to a second thread (the hop reads a snapshot, the blocking work touches only the DB/file).

---

## 3. House style: the asio primitives & idioms to adopt

A small, consistent vocabulary. Pin to **boost::asio** (already the dependency; webhook_sink already uses it).

| Idiom | Use | Project example today |
|---|---|---|
| `asio::awaitable<T>` | every long-running/async operation's return type | `webhook_sink.cpp:49` `AttemptHttp` |
| `co_await op(..., asio::use_awaitable)` | the default completion token | `webhook_sink.cpp:55-65` |
| `asio::co_spawn(ex, coro(), token)` | launch a detached/root coroutine | `webhook_sink.cpp:199-202` |
| `asio::as_tuple(asio::use_awaitable)` | `auto [ec, n] = co_await ...` — handle errors without exceptions on hot paths (serial read EOF, socket reset) | new |
| `asio::redirect_error(use_awaitable, ec)` | single-error ops where a tuple is overkill (TLS shutdown) | `webhook_sink.cpp:103` already uses this |
| `asio::experimental::channel<void(error_code, T)>` | producer/consumer hand-off: write queue, WS egress, MQTT publish queue, nav page/status events | new (replaces the deques) |
| `asio::steady_timer` + `co_await t.async_wait` | all cadence/timeout/backoff | already in HistoryService/AlertMonitor/JandyStartup |
| `asio::experimental::awaitable_operators` (`||`, `&&`) | "read CONNACK **or** timeout" races, "any child coroutine fails" | new |
| `asio::cancellation_signal` / `bind_cancellation_slot` | structured shutdown; replaces the `shutdown` bool & generation-counter staleness guards | new |
| `asio::this_coro::executor` | get the current executor inside a coroutine | `webhook_sink.cpp:51` |

**House conventions:**

1. **Error handling on I/O loops uses `as_tuple` (no exceptions across the read loop).** Throwing coroutines are fine for the one-shot webhook (where the catch is the retry), but the serial/MQTT/WS *loops* must observe `ec == operation_aborted` (clean cancel), `ec == eof`/`connection_reset` (reconnect), else log+continue. A thrown exception escaping a `co_spawn`'d root with `detached` calls `std::terminate` indirectly via the completion handler — always pair a root `co_spawn` with a completion handler that logs `eptr`, or use `as_tuple` internally.
2. **Bounded channels carry their own backpressure policy.** `experimental::channel<void(error_code, T)>(ex, N)`. `try_send` for drop-on-full producers (WS broadcast, MQTT publish — both already drop-oldest today); `async_send` for must-not-drop hand-offs. The bound replaces the manual `MAX_MESSAGE_QUEUE_SIZE` / `MAX_PUBLISH_QUEUE_SIZE` checks — but note the *drop-oldest* semantics (pop_front then push, `websocket_equipment.cpp:86-100`, `mqtt_client.cpp:291-297`) must be reproduced explicitly (a full channel `try_send` *fails the new item* — to drop *oldest* you `try_receive` one then `try_send`).
3. **A coroutine owns the buffers it `co_await`s across suspension.** No raw borrowed pointers held across a `co_await` (this is the `m_pCurrentContent` hazard, §8.4). Move the page snapshot / the beast `message_generator` into the coroutine frame.
4. **One writer per stream, always.** A beast WS stream and the serial port each have exactly one writer coroutine draining a channel. Producers (signals2 slots) only `try_send`/`async_send`; they never write the stream. Two coroutines writing one beast WS stream is UB.
5. **Every `co_spawn` for a long-running subsystem gets a cancellation slot wired to the root.** Detached-without-cancellation is only for genuinely fire-and-forget one-shots (webhook POST).

---

## 4. Per-subsystem conversion table

| Subsystem | Current driver | Target shape (coroutine[s] to `co_spawn`) | How `Poll()` is removed | Effort |
|---|---|---|---|---|
| **main frame loop** (the `while (!shutdown)` loop, `aqualink-automate.cpp` ~867-943) | hand-rolled `while(!shutdown)` + double `io_context.poll()` + idle `sleep_until` | `co_spawn` each subsystem (below), then `io_context.run()` on the main thread | The whole loop deleted; `run()` blocks on the reactor | large |
| **serial + ProtocolTask** (`protocol_thread.cpp:184`) | `protocol_task->Poll()` per frame; sync `read_some`/`write_some` in would_block loops | `SerialReadLoop`: `for(;;){ auto [ec,n] = co_await port.async_read_some(buf); frame+dispatch; }`. `SerialWriteLoop`: drains an outbound `channel`, `co_await async_write` | `Poll()` + `bool` return + `m_SingleReadPerPoll` + non-blocking-read shims all deleted | large |
| **HTTP/WS server** (`http_server.cpp:680`) | accept/read/write already async; `Poll()` only kicks WS egress + erases done sessions | accept loop stays (already self-rearming); add a **per-connection WS writer coroutine** draining a channel; `try_send` from broadcast | `HttpServer::Poll` + `HttpSessionState::Poll` + `TryWsWrite` egress-kick deleted; session erase moves to coroutine exit | large |
| **MQTT** (`mqtt_client.cpp`, async-mqtt-backed) | `mqtt_integration->Poll()` → `MqttClient::Poll()` → `Impl::FlushIfConnected()` (drains the publish queue; async_mqtt owns connect/read/write/keepalive) | Replace the per-frame `FlushIfConnected()` drain with a co_spawned writer woken by `Publish()`; keep the existing `CalculateReconnectDelay` steady_timer reconnect | Per-frame `mqtt_integration->Poll()` call removed; the library already drives the endpoint under `run()` | small (RECONCILED 2026-06-30 — was "large") |
| **navigation + spider** (`onetouch_device.cpp`, `navigator.cpp`) | signals2 slots re-enter `OnPageUpdate` per inbound frame; pending-status **counter** simulates "await page" | per-device `NavCoro`: page/status events arrive via a `channel`; `co_await AwaitPage(target, deadline)`; cursor/recovery become `while(...) co_await SendKeyAwaitStatus()` | No `Poll()` exists to remove; the per-frame re-entrant dispatch is replaced by the coroutine awaiting page/status events | large |
| **devices + dispatch** (`command_dispatcher.cpp`, `serial_adapter_device.cpp`) | slots fire inline; commands queued, return optimistic `Success` | command path → `awaitable<CommandResult>` that completes when the ACK is actually emitted; pending-command queue → `channel` | n/a (no Poll); fire-and-forget queue becomes awaitable | large |
| **Restartable watchdog** (`restartable.cpp:35`) | `PollAll()` per-frame sweep of `s_Instances` | one repeating `WatchdogLoop` `steady_timer` (e.g. 1s tick) calling `CheckWatchdog(now)` across `s_Instances` — minimal change | `PollAll()` deleted; the per-frame call in main removed | small |
| **history** (`history_service.cpp:512`) | self-rearming `steady_timer` (already async) | already correct; only **blocking SQLite** moves to the DB pool via `co_await post(db_pool)` | n/a — no Poll; timers already async | medium |
| **scheduler / equipment_cache** | self-rearming `steady_timer` | already correct; blocking file `Load/Save` → file pool | n/a | medium |
| **alert_monitor** (`alert_monitor.cpp:161`) | self-rearming comms `steady_timer`; pure compute | already correct; `co_spawn` the comms loop; no I/O | n/a | small |
| **preferences** (`preferences_service.cpp`) | **no io_context, no timer** — HTTP-/event-driven | inject an executor; blocking `Save/Load` → file pool, routes `co_await` it | n/a — never had a Poll | medium |
| **jandy startup** (`jandy_startup_service.cpp:37`) | self-rearming 1s `Tick` timer; pure state machine | already correct; `co_spawn` the Advance loop | n/a | small |
| **webhook_sink** (`webhook_sink.cpp:199`) | already `co_spawn` + full `awaitable` chain | **no change** except swap `detached` for a cancellation-slot-bound spawn | already async | trivial |

---

## 5. Replacing the manual frame loop in `main()`

Today (the `while (!shutdown)` frame loop in `src/aqualink-automate.cpp`, ~lines 867-943): `while(!shutdown){ poll(); protocol->Poll(); Restartable::PollAll(); http/https/mqtt->Poll(); poll(); pace; }`.

**Target:**

```cpp
// after all subsystems are constructed (serial / protocol / http / mqtt setup, all
// before the `while (!shutdown)` loop at ~line 867 in src/aqualink-automate.cpp)
asio::cancellation_signal root_cancel;
auto spawn = [&](auto coro) {
    asio::co_spawn(io_context, std::move(coro),
        asio::bind_cancellation_slot(root_cancel.slot(),
            [](std::exception_ptr e){ if (e) LogError(...); }));   // never let a throw escape detached
};

spawn(SerialReadLoop(serial_port, ...));      // replaces protocol_task->Poll() read side
spawn(SerialWriteLoop(serial_port, write_channel));
spawn(WatchdogLoop(std::chrono::seconds(1))); // replaces Restartable::PollAll()
spawn(HttpListener(*http_server));            // accept loop (was already async; just owns its lifetime)
spawn(HttpListener(*https_server));
spawn(MqttSupervisor(*mqtt_client));          // replaces the three ::Poll() methods
spawn(MqttHubPublishLoop(*mqtt_hub));         // periodic/debounce timers

// signal_set: instead of flipping a bool, request cancellation of the whole tree
shutdown_signals.async_wait([&](auto, int){ root_cancel.emit(asio::cancellation_type::all); });

io_context.run();   // single thread; sleeps in epoll/IOCP until real I/O or a timer fires
```

- **The idle `sleep_until` pacing is deleted outright.** `run()` blocks in the reactor; there are no wasted wakeups and the ~1ms worst-case dispatch latency vanishes.
- **Capture-replay pacing must move into the producer.** Today replay timing lives in *two* places: the frame-loop `sleep_until(frame_start + frame_period)` (the `std::this_thread::sleep_until(...)` at ~line 936, governed by the `replay_paced` flag — `const bool replay_paced = ...` at ~line 476 — and `replay_frame_period`) **and** `ProtocolTask::m_SingleReadPerPoll` (one chunk per Poll). Under `run()` both disappear, so the **`MockSerialPortImpl`/replay producer must self-pace**: between emitting chunks it does `co_await steady_timer(replay_frame_period)`. This is the *only* surviving use of a frame clock, and it belongs to the replay source, not the application. The fixture-replay tests (`test/integration/flows/`) gate this — they must keep passing.
- **Tracy frame marks lose their natural cadence.** `profiler.Get()->EmitFrameMark("MainLoop")` (the initial emit before the loop at ~line 865 and the per-frame emit at ~line 942) fired once per frame. Under `run()` there is no frame. Re-anchor it to a meaningful boundary — emit a frame mark per serial read-loop iteration (per RS-485 chunk), which is the closest analogue to "one unit of protocol work."

---

## 6. Shutdown / cancellation strategy

The existing ordered teardown (the **"STOP APPLICATION"** banner through the final `StopProfiling()`, `aqualink-automate.cpp` ~lines 945-1034) is **correct and subtle**, and the new design must reproduce its ordering exactly — it is not free to reshuffle.

The verified critical constraints (symbol-anchored; approximate line numbers in parentheses):
1. **Serial cancel must precede protocol teardown** — `serial_port->cancel()` (~953-957) then `protocol_task.reset()` (~960) — to unblock the read coroutine before its owner is destroyed.
2. **`IRecordingController` must be `Unregister`'d before `serial_port.reset()`** — `hub_locator.Unregister<Interfaces::IRecordingController>()` (~1025) then `serial_port.reset()` (~1027) — the HubLocator holds a null-deleter `shared_ptr` *aliasing* a SerialPort-owned `RecordingSerialPortImpl`; resetting the port first leaves a dangling alias.
3. Alert monitor stops (`alert_monitor.Stop()`, ~964) before the MQTT client its sink references (MQTT stop ~995-1000).

**Target sequence:**

```
SIGINT/SIGTERM → shutdown_signals handler → root_cancel.emit(cancellation_type::all)
```

- The emit propagates the cancellation slot into every in-flight `async_read`/`async_write`/`async_wait`, which complete with `operation_aborted`. Each loop coroutine observes `ec == operation_aborted` and returns — **structured unwinding** drops the coroutine frames (and the buffers they own) in reverse spawn order automatically.
- **Ordering that cancellation alone does not guarantee** (the IRecordingController alias, the serial-before-protocol rule, alert-before-mqtt) must be enforced by **awaiting** the relevant coroutines' completion in sequence rather than just `io_context.stop()`. Concretely: after `run()` returns (all roots cancelled), perform the *same* explicit reset order as the teardown after the STOP APPLICATION banner (~945-1034) — `serial_port->cancel()` (~953-957), `protocol_task.reset()` (~960), `alert_monitor.Stop()` (~964), history/jandy/scheduler/cache stop, mqtt stop (~995-1000), http stop (~1003-1012), `Routing::Clear()`, **`hub_locator.Unregister<IRecordingController>()` (~1025) then `serial_port.reset()` (~1027)**, `MessageGeneratorRegistry::Clear()`, `StopProfiling()` (~1034) last. The cancellation makes the *coroutines* exit; the explicit teardown still owns the *object-destruction order*.
- Do **not** use a bare `io_context.stop()` for shutdown — it drops pending handlers without running their cancellation, which can strand a half-written WS close or a DISCONNECT packet. Prefer `root_cancel.emit(all)` and let `run()` drain to natural completion.
- The `HttpServer::Stop()` method that synchronously closes sessions becomes: cancel the listener/sessions, draining any half-written WS close. For MQTT (RECONCILED 2026-06-30 — now async-mqtt-backed) `Stop()` should drive the **library's** close/disconnect rather than a hand-rolled DISCONNECT write; the old `m_ConnectGeneration` staleness guard no longer exists (async-mqtt owns connection lifecycle), so there is nothing left to retire there.

---

## 7. Impact on the test harness

This is the migration's most pervasive non-production change, and it must be handled deliberately.

**Today:** `MockReplayHarness` (`test/unit/support/unit_test_mockreplayharness.h`) drives `Protocol::ProtocolTask::Poll()` *directly and synchronously*: `Replay()` queues bytes into `TestSerialPortImpl`, then "Poll until the protocol task reports no remaining work" (`ReplayOne`, `:112-114`). It holds the `ProtocolTask` by `shared_ptr` and calls its `bool Poll()`. The `single_read_per_poll` flag is forwarded to exercise the replay per-frame read bound. The whole harness — and `test/unit/navigation/*`, `test/integration/flows/test_flow_onetouch_*` which drive `OnPageUpdate`/`ProcessStep` directly and assert on returned `NavKeyCommand` — assumes a **synchronous, step-driven** model.

**Two-tier adaptation, lowest-disruption first:**

1. **Keep `ProtocolTask` step-drivable for tests.** The cleanest path is to keep a thin **synchronous `StepOnce()`** entry on `ProtocolTask` (the body of today's `Poll()`) used *only by the harness*, while production drives the new `SerialReadLoop` coroutine. Both feed the same `ProcessMessages` → signals2 path, so device decode assertions are unchanged. This preserves the entire existing decode/replay test surface with near-zero churn — the harness keeps calling a step method, the bytes still flow through the real generator and real device slots.

   - Alternatively (more faithful to production), give the harness its own `io_context` and drive it with `run_one()` / `poll()` in `ReplayOne`, `co_spawn`-ing the real `SerialReadLoop` over the `TestSerialPortImpl`. This exercises the *actual* async path but requires the mock to surface a would_block/EOF that ends the spawned read so `run_one` returns. Reserve this for a focused set of "async-path" tests; do not force every existing decode test through it.

2. **Navigation/command tests that today assert "returns this `NavKeyCommand`"** keep the pure `Navigator`/`SpiderEngine`/`MenuModel` *decision functions* synchronous and test them exactly as now (these are pure and must **not** be rewritten as coroutines — see §8.4). Only the *driver* becomes a coroutine, and the driver is what the integration flow tests exercise; those gain an `io_context` + `co_spawn` of the nav coroutine, feeding page/status events through the channel and awaiting the terminal `ActuationResult`.

3. **The fixture record↔replay symmetry must survive.** `LoadFixture`/`ReplayFixture` and the `.cap` format are execution-model-agnostic; the only coupling is replay *pacing*, which moves from the frame loop into the mock producer (§5). Keep running the test exe **from `build/<preset>/test/`** so the relative `fixtures/...` paths resolve (a documented gotcha).

Net: the **decode/hub assertion surface is preserved by keeping a synchronous step entry**; only the navigation *flow* tests and any test that asserts on `Poll()`'s `bool`/pacing semantics need real adaptation.

---

## 8. The genuinely hard problems

### 8.1 The serial write path crosses producer "threads" via signals2

`ProtocolTask::ConnectWriteSignal` (`protocol_thread.h:48-64`) connects a signals2 slot that `Serialize`s and calls `EnqueueWrite` → `m_WriteQueue.push_back` (`:234-236`). Producers are device ACK signals fired from *inside* the decode path. Under (A) this is still one thread, so the literal port is safe — but to remove `Poll()` the write queue must become a `channel` drained by `SerialWriteLoop`. **The slot must `try_send`/`async_send` onto the channel, never write the port.** This keeps the single-writer rule. `asio::async_write` replaces the manual `m_WriteOffset` partial-write bookkeeping (`:55-93`) and guarantees the whole buffer — verify equivalence for half-open TCP (a write error must surface as the writer coroutine's `ec`, triggering the same reconnect path the sync code got from would_block→hard-error).

### 8.2 MQTT is library-backed — only app-level glue remains

> **RECONCILED 2026-06-30.** This section's original premise — "vcpkg.json has no
> MQTT dependency; the entire 3.1.1 state machine, framing, keepalive must be
> re-expressed" — is **FALSE as of 2026-06-19**, when the MQTT client was rewritten
> onto the **async-mqtt** library. The corrected reality follows.

MQTT is now **library-backed**: `vcpkg.json` declares `{ "name": "async-mqtt",
"features": ["tls"] }`, and `mqtt_client.cpp` includes `<async_mqtt/all.hpp>` and
owns an `async_mqtt::endpoint` (`MqttClient::Impl`, a TCP or TLS
`am::endpoint<am::role::client, am::protocol::mqtt|mqtts>`). The library owns the
MQTT 3.1.1 wire concerns that this section originally scoped as migration work:
framing/encode/decode, keepalive, the CONNECT/CONNACK handshake, and the async
read/write completions — all driven by the host `io_context`. There is **no
hand-rolled state machine, partial read/write bookkeeping, or `m_ConnectGeneration`
staleness guard left to re-express**.

What actually remains is thin app-level glue: (1) the bounded **drop-oldest** publish
queue (`MqttClient::MAX_PUBLISH_QUEUE_SIZE = 1000`, mqtt_client.h) drained by
`Impl::FlushIfConnected()`, which `MqttClient::Poll()` calls once per frame; and
(2) the reconnect **backoff + jitter** (`MqttClient::CalculateReconnectDelay()`)
applied via an `async_mqtt`/asio `steady_timer` (`m_ReconnectTimer.expires_after(...)`).
Under `io_context::run()` the only genuinely poll-coupled piece is
`FlushIfConnected()`: replace the per-frame call with a co_spawned writer woken when
`Publish()` enqueues (or feed a bounded `experimental::channel`, reproducing
drop-oldest via `try_receive`-then-`try_send`). The reconnect path is already
timer-based and needs no change. This subsystem's effort drops from "large" to
"small" — most of what this section originally described is now obviated.

### 8.3 Honest command completion surfaces a latent bug

Commands return `CommandResult::Success` the instant they are *queued*, before any byte hits the wire (`command_dispatcher.cpp`, `serial_adapter_device.cpp:175`). Converting `ToggleByUuid` etc. to `awaitable<CommandResult>` that completes only when the ACK is **actually emitted** is the right design — but MEMORY records that an optimistic 200-apply already *masks a real aux-toggle no-op* (the RSSA byte-order bug). An honest await will **newly surface** that as a user-visible failure/timeout. Therefore: every awaitable command **must** carry a cancellation+deadline (`co_await (AwaitAck() || steady_timer(cmd_timeout))`), and the "command only emits in response to a Status/master poll" hard protocol constraint (`onetouch_device.cpp:221-229`, `serial_adapter_device.cpp:86`) means the coroutine genuinely suspends across multiple frames. Lifetime is the sharp edge: the device may be destroyed, or **emulation-suppressed** mid-wait (`Capabilities::Emulated`, `emulated.h:39-63` silently drops queued commands) — the awaitable must then resolve waiters with `NotSupported`, not hang.

### 8.4 Navigation re-entrancy + the dangling page-snapshot borrow

`Navigator::OnPageUpdate` runs to completion synchronously today and stashes `m_pCurrentContent` as a **raw pointer to a stack `ScreenDataPage`** (`navigator.h:191`), read later in the same call by `FindLineByLabel`/`AttemptWrapRecovery`. **Inserting a `co_await` mid-flow would dangle it.** Rule: the coroutine must **own/move** the page snapshot into its frame, never borrow. Equally subtle: the pending-status counter ordering is load-bearing — `OnStatusMessageReceived` must run *before* the step function, and `SpiderEngine::OnStatusReceived` deliberately does **not** double-decrement (`spider_engine.cpp:209-215`). A channel feeding the nav coroutine must preserve **exactly one logical Status event per inbound frame**, or page transitions fire early/late. Keep the pure `Navigator`/`SpiderEngine`/`MenuModel` *decision functions* synchronous — only the *driver* becomes a coroutine. Also note the frame-count timeouts (`MAX_WAIT_CYCLES=100`, `ONETOUCH_*_STEP_LIMIT=500`) scale with *bus traffic rate*, not wall-clock; converting them to real `steady_timer` deadlines **changes timeout behaviour** and must be re-tuned against live captures.

### 8.5 The mock's real blocking `sleep_for`

`MockSerialPortImpl::HandleMockWrite` does a genuine `std::this_thread::sleep_for(send_duration)` (`mock_serial_port_impl.cpp:326`) to simulate baud-rate TX time. Tolerable today (single thread, mock-only, driven by an explicit Poll). Under `io_context::run()` it would **stall the io_context thread**. Must become `co_await steady_timer(send_duration)` or be dropped.

### 8.6 Restartable's process-global registry

`s_Instances` is mutated from device ctors/dtors (`restartable.cpp:21,32`) and swept by `PollAll`. Under (A)/single-thread the conversion is trivial and *low-risk*: one `WatchdogLoop` `steady_timer` tick calling `CheckWatchdog(now)` across `s_Instances` — the cheapest win in the whole migration (effort: small). It only becomes a hazard if device *construction* ever moves off the single thread (async discovery), which (A) does not introduce. Note the alternative (per-device timer rearmed in `Kick()`) has a wider blast radius (Restartable is mixed into 7+ device types) and introduces `Kick()`/timer-cancel interplay — prefer the single-timer sweep.

### 8.7 The static per-message-type signal is process-global

`IMessageSignalRecv<T>::GetSignal()` returns a function-local-static `shared_ptr<signal>` (`imessagesignal_recv.h:39-43`), shared across all devices. Connect/disconnect happens from device ctors/dtors. Under (A) this is safe (one thread). It is called out here only as the *first* thing that would need strand-confinement under any future (B) — it is the single natural place to `post`/`co_spawn` handler coroutines if dispatch ever needs to leave the decode stack.

---

## 9. Sequencing the migration (low-risk order)

1. **Restartable → single `WatchdogLoop` timer** (§8.6) — small, isolated, removes one `Poll()`.
2. **Serial read/write → `SerialReadLoop` + `SerialWriteLoop` + write channel**, keeping a synchronous `StepOnce()` for the harness (§7) — removes the protocol `Poll()` and the largest chunk of the frame loop's reason to exist; replay pacing moves to the mock.
3. **WS egress → per-connection writer coroutine + channel** — removes `HttpServer::Poll`; accept/read already async.
4. **MQTT → supervisor + Reader/Writer/Keepalive** (§8.2) — removes the three `Poll()` methods.
5. **Delete the frame loop; `co_spawn` + `run()`; cancellation-based shutdown** (§5, §6).
6. **Blocking I/O (history SQLite, scheduler/cache/preferences files) → DB/file thread-pool behind awaitables** (the (C) sliver, §2) — independent, can land anytime after step 5; turns the remaining synchronous-on-the-loop blocking calls into `co_await post(pool)`.
7. **Honest awaitable command completion** (§8.3) — last and most behaviour-changing; gated on good timeout/cancellation and on consciously deciding to surface the previously-masked failures.

Steps 1–5 deliver the headline win (frame loop gone, latency removed, lock-free model intact) with the navigation flow rewrite (§8.4) and command-completion rewrite (§8.3) deferrable as follow-ups.

---

## Key files (all absolute)

- Frame loop / lifecycle / teardown: `R:\aqualink-automate\src\aqualink-automate.cpp` (io_context `boost::asio::io_context io_context;` ~249, `signal_set` ~858, the `while (!shutdown)` loop ~867-943, teardown after the "STOP APPLICATION" banner ~945-1034). Citations RECONCILED 2026-06-30 — symbol-anchored; treat line numbers as approximate.
- Serial engine: `R:\aqualink-automate\src\core\protocol\protocol_thread.cpp` (`ProtocolTask::Poll`), `R:\aqualink-automate\src\core\protocol\protocol_thread.h` (`ConnectWriteSignal`)
- Watchdog: `R:\aqualink-automate\src\core\devices\capabilities\restartable.cpp` (`Restartable::PollAll`, the `s_Instances` global)
- HTTP/WS: `R:\aqualink-automate\src\core\http\server\http_server.cpp` (`HttpServer::Poll`); the WS code is under `http\` (NOT `http\server\`): `R:\aqualink-automate\src\core\http\websocket_equipment.h` (the "intentionally unsynchronised" note) and `R:\aqualink-automate\src\core\http\websocket_equipment.cpp` (`Broadcast`/`DequeueMessage`)
- MQTT (async-mqtt-backed): `R:\aqualink-automate\src\core\mqtt\mqtt_client.cpp` (`MqttClient::Poll` → `Impl::FlushIfConnected`, `Publish`, `CalculateReconnectDelay`), `R:\aqualink-automate\src\core\mqtt\mqtt_client.h` (`MAX_PUBLISH_QUEUE_SIZE`), `R:\aqualink-automate\src\core\mqtt\mqtt_integration.cpp` (`MqttIntegration::Poll`), `R:\aqualink-automate\vcpkg.json` (`async-mqtt` + `tls`)
- Canonical coroutine example: `R:\aqualink-automate\src\core\alerting\webhook_sink.cpp` (`:49-104` awaitable chain, `:199-202` co_spawn)
- Hubs/DI invariant: `R:\aqualink-automate\src\core\kernel\hub_locator.h` (`:33-34,78`)
- Test harness: `R:\aqualink-automate\test\unit\support\unit_test_mockreplayharness.h`

No code was modified.
