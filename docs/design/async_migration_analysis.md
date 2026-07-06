<!--
  Generated 2026-06-18 by a multi-agent analysis workflow (8 subsystem surveys ->
  design synthesis -> 3 adversarial critiques -> merged roadmap). ANALYSIS ONLY:
  no production code was changed by this work (the only code change in the
  originating branch is the vcpkg submodule bump to 2026.06.01).

  RECONCILED 2026-06-30: this is an analysis SNAPSHOT, reconciled with the code on
  2026-06-30. Since the 2026-06-18 capture, src/aqualink-automate.cpp grew ~120 lines
  (every absolute file:line into it had drifted by ~+120) and the MQTT client was
  rewritten onto async-mqtt on 2026-06-19 (§6.2 already records this as "Landed
  v0.5.0-beta.1" and that note remains accurate). This pass re-anchored the
  frame-loop / Poll() / teardown citations to durable SYMBOL anchors and corrected
  the §2.3 ConfigUpdateSignal emit location. Treat any remaining bare ":NNN" as
  approximate.
-->

> **Async migration analysis** — this is the entry-point document (the authoritative roadmap).
> Companion docs: [Architecture design synthesis](async_migration_design.md) · [Per-subsystem survey](async_migration_survey.md).

# Async Migration Roadmap — aqualink-automate

**Status:** authoritative engineering plan. Merges the proposed design with three adversarial critiques (thread-safety, lifetime/cancellation, migration/testability). Every conflict is resolved explicitly below with the decision and rationale. The `file:line` references were verified against source as of the 2026-06-18 capture and RECONCILED with the code on 2026-06-30 (see the banner) — treat bare `:NNN` line numbers as approximate and prefer the symbol anchors. **This document plans an edit; no code is changed by it.**

---

## 1. Executive summary & recommended target model

aqualink-automate today runs a hand-rolled cooperative frame loop (the `while (!shutdown)` loop in `src/aqualink-automate.cpp`, ~lines 867-943) that, per ~1 ms frame, calls `io_context.poll()` twice (~881 & ~921) and the four subsystem `Poll()` methods (`protocol_task->Poll()` ~888, `Restartable::PollAll()` ~894 in the `main -> watchdogs` profiling zone, `http_server->Poll()` ~904 / `https_server->Poll()` ~910 / `mqtt_integration->Poll()` ~916), then idle-sleeps (`std::this_thread::sleep_until(frame_start + frame_period)` ~936). Everything runs on one thread with **zero** mutexes/strands/atomics under `src/core/kernel` — the single-thread invariant *is* the concurrency model, and it is load-bearing in code that documents it as a contract (`hub_locator.h:33-34`, `equipment_hub.h:82-87`, `device_graph.h:161-166`, `websocket_equipment.h:52-61`, `mqtt_client.h:43-55`).

**Recommended target: Model (A) — a single `io_context` on one hub-owning thread, driven by `io_context::run()`, with each long-running subsystem `co_spawn`'d as a C++20 coroutine, the manual frame loop deleted, and structured cancellation for shutdown.** (A) banks the entire win the migration exists for — removing the up-to-1ms dispatch latency and replacing five hand-rolled poll state machines with linear coroutines — while preserving every no-lock hub/signals2 invariant *for free*, because handlers still serialise on one thread exactly as today. Model (B) (multi-thread + strands) is rejected: to stay correct it must bind all hub access *and every signal-driven consumer slot* to one `hub_strand`, re-serialising the very work the threads were meant to parallelise while adding `co_await dispatch(hub_strand)` churn across ~30 web handlers, every device status processor, and every consumer slot — large, regression-prone, for throughput an inherently-serial low-rate RS-485 workload does not need. Model (C) (hybrid: primary strand + worker pool for blocking I/O) is the correct *eventual* home for the genuinely-blocking pieces (SQLite, file writes, static-file `open/stat`) and is **adopted as a bounded sliver** — a serialising worker executor reached only behind awaitables — but layered in *after* (A) lands, not before. Net: **adopt (A) now; fold in the (C) sliver for blocking I/O once the frame loop is gone; never go multi-threaded on the hubs without first converting the documented single-thread contracts into strand assertions.**

---

## 2. Target architecture

### 2.1 Executor & threading model

```
        ┌──────────────────────────────────────────────────────────────┐
        │  io_context  (ONE hub-owning thread: main → run())             │
        │                                                               │
 bytes ─▶ SerialReadLoop ─┐                                             │
                          ├─ signals2 (inline) ─▶ hubs ─▶ consumer slots│
 out  ◀── SerialWriteLoop ◀─ write channel ◀── ConnectWriteSignal slot  │
                                                                       │
 WatchdogLoop (1 steady_timer)   History/Scheduler/Cache/Startup loops  │
 MqttSupervisor → {Reader, Writer(channel), Keepalive}                  │
 HttpListener → per-session coro → {WsReader, WsWriter(channel)}        │
 AlertCommsLoop   PerDeviceNavCoro   WebhookSink (co_spawn)             │
        └───────────────┬───────────────────────────────────────────────┘
                        │  co_await post(blocking_pool)  [the (C) sliver]
                        ▼
        ┌──────────────────────────────────────────────────────────────┐
        │ blocking_pool = asio::thread_pool(1)  — SQLite + file I/O only │
        │ (serialising; subsystem state confined here, see §5.3)         │
        └──────────────────────────────────────────────────────────────┘

        ┌──────────────────────────────────────────────────────────────┐
        │ Matter sidecar poller — its OWN std::thread + OWN io_context   │
        │ (webroute_diagnostics_matter.cpp:47,91,110) — touches NO hub,  │
        │ NO signals2, NO shared io_context. PRE-EXISTING & SANCTIONED.  │
        └──────────────────────────────────────────────────────────────┘
```

**The invariant, stated precisely (resolves Critique 1 Defect 1 / Critique 3 Issue 6):** the design's "ONE thread" shorthand is *false as written* — `webroute_diagnostics_matter.cpp:110` already spawns a process-lifetime `std::thread` running `FetchSidecarStatus` on its own throwaway `io_context` (`:47`, `ioc.run_for` `:91`), correctly isolated behind `m_StatusMutex`/`m_WakeMutex` (`:117,:139`) and touching no hub. **The real invariant is: "all hub-mutating and signals2-firing work runs on the single io_context thread."** This is promoted to a project rule:

> **House Rule H-1 (worker threads).** A worker thread may exist iff it touches **no hub, no signals2 signal, and no shared `io_context`**. The Matter poller is the reference pattern and the (C) `blocking_pool` is its sanctioned sibling. Never "consolidate" the Matter thread onto the main `io_context` — its bounded `run_for` would block the serial reader.

### 2.2 Strand placement

Under (A) `make_strand` is **not used** — single-thread `run()` already serialises everything. Strands are reserved as the (B) escape hatch and are documented, not deployed, with one concrete forward-looking note: should (B) ever be pursued, the **first** things to strand-confine are the static per-message signal (`imessagesignal_recv.h:39-43`), `MqttClient::Publish` + the diagnostics read accessors (`mqtt_client.h:109-116`), and every container behind a single-thread doc-comment. Those comments become a `BOOST_ASSERT(strand.running_in_this_thread())` checklist, never silently deleted.

**Decision (resolves open question 1): build the assertion scaffolding NOW (Phase 0.5), do not defer it to a hypothetical (B).** Introduce one hub-affinity primitive up front and wire it into every documented single-thread site immediately. It earns its keep under (A): the invariant becomes *machine-checked* (the pre-existing Matter worker thread, H-1, trips it the instant it touches a hub), it catches any accidental cross-thread access introduced *during* the migration, and it turns a future (B) into a one-line body-swap instead of a code-wide hunt. Shape:
> - `Kernel::HubExecutor` — a handle captured once at startup. Today it is the `io_context` thread identity; under a future (B) it becomes the hub `asio::strand`. Call sites never learn which.
> - `AQUALINK_ASSERT_ON_HUB()` → `BOOST_ASSERT(Kernel::RunningOnHub())` in debug, compiled out in release. `RunningOnHub()` is `this_thread::get_id() == hub_thread_id` today; under (B) it becomes `hub_strand.running_in_this_thread()` — the **only** line that changes.
> - Wired at: `HubLocator::Register/Resolve` (`hub_locator.h`), every hub setter + signals2 emit, `WebSocket_Equipment::m_Connections` mutators, `MessageGeneratorRegistry` access, `Restartable::s_Instances` add/remove/sweep, the static `IMessageSignalRecv<T>::GetSignal()` connect/disconnect, and `MqttClient::Publish` + the diagnostics accessors — exactly the sites whose doc-comments assert the contract today.

### 2.3 Lock-free-hub preservation strategy

The hubs, every signals2 emit site, every slot, the static per-message dispatch (`imessagesignal_recv.h`), the `MessageGeneratorRegistry`, the circular-buffer framing, and the `scoped_connection` lifetime model are **kept verbatim**. They are passive state + synchronous notification; they have no `Poll()` to remove and must not be `co_spawn`'d. Single-thread `run()` keeps the mutate-then-emit-inline pattern correct with zero changes.

**The reentrant fan-out is deeper than "AlertMonitor re-reads the hub" (resolves Critique 1 Defects 2-4).** A single `DataHub::PoolTemp()` setter → `EmitTemperatureEvent` → `ConfigUpdateSignal` (the `ConfigUpdateSignal(update_event)` call inside `DataHub::EmitTemperatureEvent`, data_hub.cpp — the original `:19` citation pointed at the include block; the emit is in `EmitTemperatureEvent`, ~line 28) synchronously runs, on the writer's stack: `AlertMonitor::EvaluateAll` (`alert_monitor.cpp:126`) → `SetCondition` (`:324`) which fires a **second, different** signal `EquipmentHub::AlertTransitionSignal` (`aqualink-automate.cpp:702`) *and* calls `MqttClient::Publish` (`:724`, mutating `m_PublishQueue` `mqtt_client.cpp:297`); plus `HistoryService::OnConfigEvent` (`history_service.cpp:124`) → `EnsureSeries` (`:186`) runs **inline SQLite**. Three consequences are now binding rules:

> **House Rule H-2 (default signals2 policy is load-bearing).** The project relies on boost.signals2's **default** (recursive-capable, per-signal mutex) policy — there is no `dummy_mutex` override anywhere. This nested two-hub emit + cross-subsystem `Publish` is well-defined *only* because of that default. **Nobody may switch hub signals to `dummy_mutex`** (it would make the nested emit UB even single-threaded), and this nested fan-out is the decisive reason a naive `shared_mutex`-on-setters (the (C) fallback) self-deadlocks across two hubs' locks — which is why (B)/(C)-with-locks are rejected in favour of strands-if-ever.

### 2.4 House style — the asio vocabulary to adopt

Pin to **boost::asio** (already a dependency; `webhook_sink.cpp` is the in-tree reference). Standard idioms:

| Idiom | Use | In-tree precedent |
|---|---|---|
| `asio::awaitable<T>` | every async op's return type | `webhook_sink.cpp:49` |
| `co_await op(…, use_awaitable)` | default completion token | `webhook_sink.cpp:55-65` |
| `asio::co_spawn(ex, coro(), token)` | launch a root coroutine | `webhook_sink.cpp:199-202` |
| `asio::as_tuple(use_awaitable)` → `auto [ec,n] = co_await …` | error-as-value on I/O loops (serial/socket EOF/reset) | `webroute_diagnostics_matter.cpp:62,66,74,80` |
| `asio::redirect_error(use_awaitable, ec)` | single-error ops (TLS shutdown) | `webhook_sink.cpp` (redirect_error) |
| `asio::experimental::channel<void(error_code,T)>` | write queue, WS egress, MQTT publish queue, nav events | **new — see §5.7** |
| `asio::steady_timer` + `co_await async_wait` | all cadence/timeout/backoff | history/alert/startup already |
| `asio::experimental::awaitable_operators` (`\|\|`,`&&`) | "read CONNACK **or** timeout", "any child fails" | new |
| `asio::cancellation_signal` / `bind_cancellation_slot` | structured shutdown (replaces `shutdown` bool + `m_ConnectGeneration`) | new |

Conventions:
1. **I/O loops use `as_tuple` (no exceptions across the loop).** Observe `operation_aborted` (clean cancel), `eof`/`connection_reset` (reconnect), else log+continue. Every detached root `co_spawn` pairs with a completion handler that logs `eptr` — never let a throw escape a detached root.
2. **Bounded channels carry their own backpressure.** The existing **drop-oldest** semantics (`websocket_equipment.cpp:86-100`, `mqtt_client.cpp:291-297` — both verified `pop_front` then push) must be reproduced explicitly: a full channel `try_send` fails the *new* item, so to drop *oldest* you `try_receive` one then `try_send`.
3. **A coroutine owns the buffers it `co_await`s across.** No borrowed pointers held across a suspension (the `m_pCurrentContent` hazard, §6.4; the beast `message_generator`, §5.4).
4. **One writer per stream, always.** Each beast WS stream and the serial port has exactly one writer coroutine draining a channel; signals2 slots only `try_send`, never write the stream (two writers on one beast stream is UB).
5. **Every long-running `co_spawn` is cancellation-wired** (§5). Detached-without-cancellation is only for genuine one-shots (the webhook POST, and even that gains a slot — §5.6).

---

## 3. Per-subsystem conversion table

| Subsystem | Current driver | Target coroutine shape | `Poll()` removal | Effort | Risk |
|---|---|---|---|---|---|
| **main frame loop** (the `while (!shutdown)` loop, `aqualink-automate.cpp` ~867-943) | `while(!shutdown)` + 2×`poll()` (~881 & ~921) + idle `sleep_until` (~936) | `co_spawn` each subsystem, then `io_context.run()` | whole loop deleted | large | **high** (teardown ordering §5) |
| **Restartable watchdog** (`Restartable::PollAll`, restartable.cpp) | per-frame `PollAll()` sweep of `s_Instances` (called ~894, the `main -> watchdogs` zone) | one `WatchdogLoop` `steady_timer` (1 s) → `CheckWatchdog(now)` over a **snapshot copy** of `s_Instances` (§6.6) | `PollAll()` + main call deleted | small | **medium** (not "mechanical" — H5) |
| **serial WRITE** `protocol_thread.h:48-64` | `ConnectWriteSignal` slot → `EnqueueWrite`→`m_WriteQueue` | slot `try_send`s a `channel`; `SerialWriteLoop` drains → `async_write` | none yet (read `Poll()` untouched) | medium | low (true drop-in) |
| **serial READ + ProtocolTask** `protocol_thread.cpp:184` | `protocol_task->Poll()` per frame | `SerialReadLoop`: `auto[ec,n]=co_await port.async_read_some(buf); frame+dispatch` | `Poll()` deleted; **but** `StepOnce()` retained for tests (§4, §6.1) | large | **high** (the irreducible cut, §7) |
| **HTTP/WS** `http_server.cpp:680` | accept/read already async; `Poll()` kicks WS egress + erases sessions | per-connection WS **writer** coroutine draining a channel; `try_send` from broadcast | `HttpServer::Poll`+`HttpSessionState::Poll`+`TryWsWrite`-kick deleted | large | medium (single-writer + close-order, §6.6/H6) |
| **MQTT** (`mqtt_client.cpp`, **async-mqtt-backed since 2026-06-19**, §6.2) | `mqtt_integration->Poll()` (~916) → `MqttClient::Poll()` → `Impl::FlushIfConnected()`; the library owns connect/read/write/keepalive | replace the per-frame `FlushIfConnected()` drain with a co_spawned writer; keep the `CalculateReconnectDelay` steady_timer reconnect | per-frame `Poll()` call removed; library already drives the endpoint under `run()` | small (RECONCILED — was "large"; the hand-rolled state machine is gone) | medium (shutdown §5/H4; preserve drop-oldest queue) |
| **navigation + spider** `navigator.cpp`, `onetouch_device.cpp` | signals2 slots re-enter `OnPageUpdate`; pending-status **counter** | per-device `NavCoro`: page/status via `channel`; `co_await AwaitPage(target,deadline)` | no `Poll()` exists; re-entrant dispatch replaced by awaiting events | large | high (re-entrancy + borrow, §6.4) |
| **devices + dispatch** `command_dispatcher.cpp`, `serial_adapter_device.cpp:175` | slots inline; commands return optimistic `Success` | **see §6.3 decision** — keep sync `CommandResult`, add *separate* `AwaitOutcome` awaitable | n/a | medium | high (breaks committed tests if done naively — §6.3) |
| **history** `history_service.cpp:512` | self-rearming `steady_timer` (already async) | timers already correct; **all mutable state** + SQLite confined to `blocking_pool` (§5.3) | n/a | medium | medium (whole-state confinement, not "hop the call" — Defects 3/4) |
| **scheduler / equipment_cache / preferences** | timers (sched/cache); preferences has no executor | blocking file `Load/Save` → `blocking_pool`; inject executor into preferences | n/a | medium | low |
| **alert_monitor** `alert_monitor.cpp:161` | self-rearming comms `steady_timer`; pure compute | `co_spawn` comms loop; no I/O | n/a | small | low |
| **jandy startup** `jandy_startup_service.cpp:37` | self-rearming 1 s `Tick`; pure state machine | `co_spawn` the Advance loop | n/a | small | low |
| **webhook_sink** `webhook_sink.cpp:199` | already `co_spawn` + full awaitable chain | swap `detached` for cancellation-slot-bound spawn | already async | trivial | low |

---

## 4. Test-harness adaptation — the early enabling step (resolves Critique 3 Issues 2 & 5)

This is an **enabling step, scheduled early (Phase 1)**, not a side-effect of the production work — because three committed tests are structurally coupled to the synchronous `Poll()` model and the migration is blocked until they are addressed:

- `test/unit/protocol/test_replay_pacing.cpp` constructs `MockReplayHarness(/*single_read_per_poll=*/true)` and asserts the **per-`Poll()` read bound** + the no-`BufferOverflows` guarantee (`:55-72`, `:102-124`). It hard-depends on the `single_read_per_poll` ctor arg, the `ProtocolTask::Poll()` `bool`, and `m_SingleReadPerPoll` (`protocol_thread.h:82`).
- `test/integration/flows/test_flow_command_to_wire.cpp` asserts `result == Success` **synchronously, before** `ReplaySerialAdapterStatus()` drives the wire (`:223-230`, `:235-242`, …) and asserts `NoSerialAdapter` synchronously (`:548-571`).
- `MockReplayHarness::ReplayOne` drives `ProtocolTask::Poll()` directly and synchronously (`unit_test_mockreplayharness.cpp:61-72`); the whole nav/decode suite assumes a step-driven model.

**Decisions:**

1. **Keep a synchronous `ProtocolTask::StepOnce()` (the body of today's `Poll()`), retaining `m_SingleReadPerPoll`, for the harness.** Production drives the new `SerialReadLoop`; the harness keeps calling a step method. Both feed the same `ProcessMessages` → signals2 path, so the entire decode/replay assertion surface survives with near-zero churn. **`m_SingleReadPerPoll` is NOT deleted** (this overrides the design's §5 "disappears"): it costs nothing, and deleting it would (a) fail `test_replay_pacing.cpp` to compile and (b) destroy the documented no-overflow guarantee that a synchronous harness cannot otherwise exercise. Per the project rule "never remove tests to work around a model change," any future relocation of that guarantee requires a *named replacement* test against the mock-producer timer path before the old test is dropped.
2. **The "faithful async-path" harness (own `io_context` + `run_one()` + `co_spawn` the real `SerialReadLoop`) is a *prerequisite subsystem*, not a casual fallback.** `TestSerialPortImpl` has **no async surface** today — only `QueueReadData`/`EnableTestMode` (verified). Scope adding `async_read_some`/`async_write_some` to `ISerialPortImpl` + the mock (with deterministic `would_block`/`eof` completion so `run_one()` returns) as an explicit deliverable of the read-path phase (§7 Phase 6) — it is the *same* `ISerialPortImpl` change production needs. Keep the **default** harness on `StepOnce()`; add a small set of async-path tests only after the mock gains that surface.
3. **Nav/command decision functions stay synchronous and are tested as-is.** Keep `Navigator`/`SpiderEngine`/`MenuModel` pure (do not rewrite as coroutines — §6.4); only the *driver* becomes a coroutine. The integration flow tests gain an `io_context` + `co_spawn` of the nav coroutine and `co_await` the terminal result.
4. **Fixture record↔replay symmetry survives.** `LoadFixture`/`ReplayFixture` and the `.cap` format are execution-model-agnostic; only replay *pacing* moves (frame loop → mock producer, §5.1). Keep running the test exe **from `build/<preset>/test/`** so relative `fixtures/...` paths resolve.

---

## 5. Replacing the frame loop, shutdown & blocking I/O

### 5.1 The new `main()` and replay pacing

Delete the `while (!shutdown)` frame loop (`aqualink-automate.cpp` ~867-943) and replace with `co_spawn` of each subsystem + `io_context.run()`:

```cpp
// after subsystems constructed
auto root_cancel = std::make_shared<asio::cancellation_signal>();   // §5.5: heap, outlives coroutines
std::vector<std::future<void>> roots;                               // join latch, §5.4 / H1

auto spawn = [&](auto coro) {
    std::promise<void> done; roots.push_back(done.get_future());
    asio::co_spawn(io_context, std::move(coro),
        asio::bind_cancellation_slot(root_cancel->slot(),
            [d=std::move(done)](std::exception_ptr e) mutable {
                if (e) LogError(...); d.set_value();      // never let a throw escape a detached root
            }));
};

spawn(SerialReadLoop(serial_port, ...));   spawn(SerialWriteLoop(serial_port, write_channel));
spawn(WatchdogLoop(1s));                    spawn(HttpListener(*http_server)); /* +https */
spawn(MqttSupervisor(*mqtt_client));        spawn(MqttHubPublishLoop(*mqtt_hub));
// history/scheduler/cache/alert/startup already co_spawn-able timer loops

shutdown_signals.async_wait([root_cancel](auto,int){ root_cancel->emit(asio::cancellation_type::all); });
io_context.run();   // single thread; sleeps in epoll/IOCP until real I/O or a timer
```

- **Idle `sleep_until` deleted outright** — `run()` blocks in the reactor; the ~1 ms worst-case dispatch latency vanishes and idle CPU genuinely improves (the busy-pace `std::this_thread::sleep_until(frame_start + frame_period)` at ~936 is gone).
- **Replay pacing moves into the producer.** Today it lives in *two* places: the loop `sleep_until(frame_start + frame_period)` (~936, governed by the `replay_paced` flag — `const bool replay_paced = ...` at ~476 — and `replay_frame_period`) **and** `ProtocolTask::m_SingleReadPerPoll`. Under `run()` the loop pacing disappears, so the **`MockSerialPortImpl` producer must self-pace** with `co_await steady_timer(replay_frame_period)` between emitted chunks — the only surviving frame clock, owned by the replay source. `m_SingleReadPerPoll` itself stays for the harness (§4). The fixture-replay tests gate this.
- **Tracy frame marks lose their cadence.** `profiler.Get()->EmitFrameMark("MainLoop")` (the initial emit before the loop ~865 and the per-frame emit ~942) fired per frame; under `run()` there is no frame. Re-anchor to one mark per `SerialReadLoop` iteration (per RS-485 chunk) — the closest analogue to one unit of protocol work.
- **The mock's real `std::this_thread::sleep_for(send_duration)` (`mock_serial_port_impl.cpp:326`) must become `co_await steady_timer(send_duration)` or be dropped** — under `run()` it would stall the io_context thread (§6.5).

### 5.2 Shutdown is a contract, not a "risk" (resolves Critique 2 H1-H4, H6, H8)

The design's §6 prescription "emit cancellation → `run()` returns → reset" is **wrong about asio semantics** and is promoted here into an explicit ordered contract. `io_context::run()` returns when there is no outstanding work, but `emit(cancellation_type::all)` does **not** synchronously complete in-flight `async_read`/`async_write`/`async_wait` — they complete with `operation_aborted` on a *later* reactor turn. If the emit is the last handler, `run()` can return while coroutine frames are still suspended holding `shared_ptr`s to `protocol_task`/`serial_port`/the client — and the subsequent `reset()` is then a use-after-free.

**Shutdown contract (binding):**

1. **SIGINT/SIGTERM handler emits `root_cancel->emit(cancellation_type::all)`** — replacing the `shutdown` bool (the `boost::asio::signal_set shutdown_signals(...)` async_wait, ~858).
2. **Drive `run()` until every root completion handler has fired** (the `roots` futures in §5.1 / a child-coroutine join). The abort completions *are* outstanding work, so `run()` keeps going; only when all roots have completed are the coroutine frames — and the buffers/`shared_ptr`s they own — provably destroyed. **Do not model shutdown as "run() returns then reset."** Do **not** use bare `io_context.stop()` (it drops pending handlers without running their cancellation, stranding a half-written WS close or MQTT DISCONNECT).
3. **Channels are closed, not just operations cancelled (H2/H6/H8).** A `SerialWriteLoop` suspended in `channel.async_receive()` (empty queue) is **not** waiting on the port — `serial_port->cancel()` does nothing to it. The write channel must be `.close()`d (→ `async_receive` returns `operation_aborted`) or the writer never wakes, its frame never drops, and shutdown hangs. **Order: disconnect the producer-side write slots first** (the `m_WriteSignalConnections` `scoped_connection`s, `protocol_thread.h:85`) so no slot `try_send`s onto a closing channel, *then* close the channel, *then* the read side. Same for each WS connection: close the egress channel → cancel the reader's `async_read` → only then `async_close`/destroy the `websocket::stream` (cancelling the stream mid-`async_write` is beast UB). A broadcast slot firing during teardown must tolerate a closed channel (`try_send` returns error → drop), never deref a destroyed connection. The replay mock's pacing timer (§5.1) must also be on the cancellation tree or replay-mode shutdown (e2e/Playwright) hangs (H8).
4. **MQTT: `co_await` the supervisor's completion before any synchronous DISCONNECT (H4).** `MqttClient` is `enable_shared_from_this` (`mqtt_client.h:56`); Reader/Writer/Keepalive capture `self`, so `~MqttClient` *cannot* run while any child is suspended. Move the synchronous DISCONNECT (`mqtt_client.cpp:215-241`, called from the `noexcept` `Stop()` and the dtor) into an explicit `co_await Shutdown()`: cancel the children, `co_await (async_write(DISCONNECT) || steady_timer(short))`, then close. `mqtt_integration->Stop()` (in the teardown after the STOP APPLICATION banner, the MQTT stop block ~995-1000) emits the client's cancellation and `co_await`s the supervisor's completion so that by `mqtt_integration.reset()` no frame holds `self`. Never run a synchronous socket write while an async op on that socket may still be in flight. Keep `Stop()` non-throwing. **(RECONCILED 2026-06-30 — note: since MQTT is now async-mqtt-backed, the old `m_ConnectGeneration` staleness guard no longer exists; the library owns connection lifecycle, so this concern is moot for the current client.)**
5. **After all roots have joined, perform the *existing* explicit reset order** (the teardown after the "STOP APPLICATION" banner, `aqualink-automate.cpp` ~945-1034), which cancellation alone does not guarantee: `serial_port->cancel()` (~953-957) → `protocol_task.reset()` (~960) → `alert_monitor.Stop()` (~964, before the MQTT it references) → history/jandy/scheduler/cache stop → mqtt stop (~995-1000) → http stop (~1003-1012) → `Routing::Clear()` → **`hub_locator.Unregister<IRecordingController>()` (~1025) then `serial_port.reset()` (~1027)** (the HubLocator holds a null-deleter `shared_ptr` *aliasing* a SerialPort-owned `RecordingSerialPortImpl`; resetting the port first dangles the alias) → `MessageGeneratorRegistry::Clear()` → `StopProfiling()` (~1034) last. Cancellation unwinds the *coroutines*; this explicit order still owns *object destruction*.

### 5.3 Blocking I/O — the (C) sliver, stated correctly (resolves Critique 1 Defects 3 & 4)

The design's "`co_await post(db_pool)` hops only the blocking call; the hop reads a snapshot, the blocking work touches only the DB" is **factually wrong for HistoryService** and is corrected here. `HistoryService::OnConfigEvent` (`history_service.cpp:124`) runs inline on the `ConfigUpdateSignal` stack and `RecordNumeric`→`EnsureSeries` (`:186`) both runs blocking SQLite **and** mutates service-owned `m_SeriesIds`/`m_SeriesLabels`/`m_Buffer` (`:193-209`) — which are *also* touched by the flush/heartbeat/purge timers and the HTTP read API (`ListSeries`/`QuerySeries`/`SeriesExists`). The racing surface is the **service's own state**, not "only the DB."

**Decision:** confine **all** of HistoryService's mutable state (`m_Db`, `m_SeriesIds`, `m_SeriesLabels`, `m_Buffer`, `m_LastSampleTs`) to a **size-1 serialising `blocking_pool`** executor. `OnConfigEvent`/`SampleCurrentState`/`ListSeries`/`QuerySeries`/`Flush`/`PurgeOld` all run on it; the read API returns `awaitable<…>` resolved there. **Hub snapshots are captured on the io_context thread before any hop** (Defect 4): `SampleCurrentState` (`:160-184`) reads `PoolTemp()/SaltLevel()/Chlorinators()` *live* today — those getters reach into `BodyOfWater`/`DevicesGraph` and **must not** run on the `blocking_pool` thread concurrently with the protocol thread mutating the same `DataHub`. Only already-extracted POD values (or the heap `shared_ptr` event payload, which is already safe to cross) may be handed to the pool. The same whole-state confinement applies to scheduler/equipment_cache/preferences file state. This is added as a project rule:

> **House Rule H-3 (executor discipline).** No hub getter may be called from any executor other than the main io_context thread. Cross to the `blocking_pool` only with already-extracted values or a `shared_ptr` event payload. A subsystem that moves blocking work off-thread confines its *entire* mutable state to that executor, not just the blocking call.

---

## 6. The hard problems & chosen resolutions

### 6.1 Serial write path crosses producers via signals2
`ConnectWriteSignal` (`protocol_thread.h:48-64`) connects a slot that `Serialize`s and `EnqueueWrite`s onto `m_WriteQueue` (`:46`,`.cpp:234`), fired from device ACK signals inside the decode path. **Resolution:** the slot `try_send`s onto an `experimental::channel`; `SerialWriteLoop` drains it with `asio::async_write` (replacing the manual `m_WriteOffset` partial-write bookkeeping and guaranteeing the whole buffer). Single-writer rule holds. A write error surfaces as the writer's `ec` → the same reconnect path the sync code got from would_block→hard-error. This is the **most isolatable change** (the enqueue seam already exists) and ships *before* the read path (§7 Phase 3).

### 6.2 MQTT is hand-rolled — but a vcpkg library can now replace most of it
> **RECONCILED 2026-06-30:** the opening premise below ("`vcpkg.json` has no MQTT dependency") describes the 2026-06-18 state and is now historical — see the "✅ Landed" note at the end of this section, which is the current truth (`vcpkg.json` now declares `async-mqtt`+`tls`; `mqtt_client.cpp` is library-backed). The narrative is kept to record how the decision was reached.

(As of the 2026-06-18 capture) `vcpkg.json` had no MQTT dependency; the entire 3.1.1 state machine, framing, partial I/O, keepalive, reconnect backoff+jitter (`CalculateReconnectDelay`, mqtt_client.cpp), LWT, and the security limits (`MAX_PUBLISH_QUEUE_SIZE=1000` drop-oldest, `MAX_READ_BUFFER_SIZE=1MB` tear-down) would have to be re-expressed without regression. **Resolution:** the connect chain (`async_resolve`/`async_connect`/`async_handshake`, `:464,492,554`) converts to `co_await` cleanly; model steady state as `Reader` (loop `async_read_some`→frame-parse→`OnMessageReceived`), `Writer` (drain publish channel→`async_write`), `Keepalive` (`steady_timer`), joined by `awaitable_operators::operator||` so any child failing tears down and re-enters backoff. CONNACK timeout = `async_read(...) || steady_timer(...)`. Reproduce drop-oldest with `try_receive`-then-`try_send`. **`Publish` is the cross-thread funnel point** (`mqtt_client.cpp:288-298`) reachable today from the AlertMonitor sink on *both* the `ConfigUpdateSignal` stack **and** the `m_CommsTimer` handler (`aqualink-automate.cpp:722-728`, `alert_monitor.cpp:126,180`) plus the startup seed (`:728`) — safe under (A); the moment (B) is considered, `Publish` + the diagnostics read accessors (`mqtt_client.h:113-116`) head the strand-confine checklist (Critique 1 Defect 5).

**Update — a library is available (overturns the "no library" premise).** The bumped vcpkg ships two mature Asio-native MQTT libraries that can *replace* most of this hand-rolled machine rather than port it coroutine-by-coroutine: **`async-mqtt`** (redboltz; BSL-1.0, header-only, MQTT **v3.1.1 + v5.0**, client + broker, v10.3.0) and **`boost-mqtt5`** (official Boost.MQTT5; MQTT **v5.0 only**, client-only, completion-token-native with built-in auto-reconnect/retransmission). Either subsumes the riskiest part of this subsystem — framing, partial I/O, keepalive, reconnect backoff, CONNACK/timeout — leaving only the app glue (topic building, HA discovery payloads, hub wiring, the drop-oldest publish policy, the diagnostics status read). The decisive trade-off is **wire protocol version**: `async-mqtt` preserves today's v3.1.1 behaviour (zero broker / Home-Assistant reconfiguration); Boost.MQTT5 forces a move to v5.0. Recommendation `async-mqtt` unless a deliberate move to MQTT v5 is wanted; the choice reshapes Phase 5 (see open question 6).

**✅ Landed (v0.5.0-beta.1).** `async-mqtt` was adopted (with the `tls` feature) and `MqttClient` was rewritten on its **runtime-`protocol_version` endpoint**, so MQTT 3.1.1 **and** 5.0 are both functional and admin-selectable via `--mqtt-protocol-version` (default 3.1.1). The library now owns framing / partial I/O / keep-alive / packet coding; the app keeps reconnect backoff+jitter, the drop-oldest queue, LWT, TLS + hostname verification, the signals, and the diagnostics counters. async-mqtt's heavy templates are confined to one TU via a pimpl. **Scope note:** this is the *library-adoption* slice of Phase 5 — the client is still driven by the existing `io_context.poll()` frame loop; removing the MQTT `Poll()` (and the rest of the poll-loop removal) remains part of the broader migration below. Verified live against a real broker for both versions over plain TCP.

### 6.3 Honest command completion — decision: do NOT change the dispatcher signature (resolves Critique 3 Issue 3, refines design §8.3)
Commands return `CommandResult::Success` the instant they queue, before any byte hits the wire (`serial_adapter_device.cpp:175`). The design proposes converting `ToggleByUuid` etc. to `awaitable<CommandResult>` that completes on actual ACK emission. **This is rejected as scoped here** because it is a breaking API change that invalidates committed, passing tests: `test_flow_command_to_wire.cpp` asserts `Success` **synchronously before** any replay (`:223-230`) — an await would *never complete* there — and `NoSerialAdapter` is *already* surfaced synchronously via the deliverability gate (`:548-571`), so honesty does not *require* an awaitable. **Decision:** keep the synchronous `ToggleByUuid → CommandResult` ("Accepted/queued") that existing optimistic web/MQTT callers and tests depend on, and add a **separate** `awaitable<ActuationResult> AwaitOutcome(handle)` that new callers opt into. This preserves the committed suite and the optimistic-200 contract while making honest completion *available*. Note: an honest await would *newly surface* the RSSA aux-toggle no-op that the optimistic 200 currently masks (MEMORY), so any `AwaitOutcome` **must** carry a deadline (`co_await (AwaitAck() || steady_timer(cmd_timeout))`) — the deadline is load-bearing for correctness, not UX, because emulation-suppression (`emulated.h:41-48`, verified: silently no-ops the ACK) and the "command only emits in response to a Status/master poll" constraint (`onetouch_device.cpp:221-229`, `serial_adapter_device.cpp:86`) mean the awaitable can otherwise hang forever. This step is **de-scoped from the frame-loop migration** and tracked as a separate behaviour-change RFC.

### 6.4 Navigation re-entrancy + the dangling page-snapshot borrow
`Navigator::OnPageUpdate` runs synchronously and stashes `m_pCurrentContent` as a **raw pointer to a stack `ScreenDataPage`** (`navigator.h:191`), read later in the same call. **A `co_await` mid-flow would dangle it.** **Resolution:** the coroutine **owns/moves** the page snapshot into its frame, never borrows. The pending-status counter ordering is load-bearing — `OnStatusMessageReceived` runs *before* the step, and `SpiderEngine::OnStatusReceived` deliberately does **not** double-decrement (`spider_engine.cpp:209-215`); a channel feeding the nav coroutine must preserve **exactly one logical Status event per inbound frame**. Keep `Navigator`/`SpiderEngine`/`MenuModel` pure and synchronous; only the *driver* becomes a coroutine. The frame-count timeouts (`MAX_WAIT_CYCLES=100`, `ONETOUCH_*_STEP_LIMIT=500`) scale with *bus traffic rate*, not wall-clock — converting them to `steady_timer` deadlines changes timeout behaviour and **must be re-tuned against live captures**. A device-owned `cancellation_signal` emitted in `~Device` *before* members are destroyed resolves any suspended command/nav awaitable (which captured `this`) — without it, a watchdog-triggered destruction (§6.6) mid-wait is a UAF (Critique 2 H7).

### 6.5 The mock's blocking `sleep_for`
`MockSerialPortImpl::HandleMockWrite` does a real `std::this_thread::sleep_for(send_duration)` (`mock_serial_port_impl.cpp:326`). Tolerable today; under `run()` it stalls the io_context thread. **Resolution:** `co_await steady_timer(send_duration)` or drop it.

### 6.6 Restartable watchdog — NOT mechanical (resolves Critique 2 H5)
`PollAll()` deliberately does **not** copy `s_Instances` ("CheckWatchdog() never adds or removes instances", verified `restartable.cpp:41`) and calls the virtual `WatchdogTimeoutOccurred()` mid-iteration; for OneTouch that enters `AttemptFaultRecovery`/state transitions. **Resolution:** convert to a single `WatchdogLoop` `steady_timer` (1 s) calling `CheckWatchdog(now)` — but **iterate a snapshot copy** of `s_Instances`, not the live registry, because the timeout callback path can (now or with future async fault-recovery/discovery) construct or destroy a device, invalidating the no-copy iteration → UAF. This **regresses the deliberate no-copy optimisation** and that trade is made consciously (correctness over a micro-optimisation on a 1 s tick). Prefer the single-timer sweep over per-device timers (Restartable is mixed into 7+ device types; per-device timers widen the `Kick()`/cancel blast radius). The HTTP/WS single-writer + close-ordering rule (§5.2/H6) and the device-dtor cancellation rule (§6.4/H7) are the companion lifetime fixes.

### 6.7 The static per-message-type signal
`IMessageSignalRecv<T>::GetSignal()` is a function-local-static `shared_ptr<signal>` shared across all devices (`imessagesignal_recv.h:39-43`), connect/disconnect from device ctors/dtors. Safe under (A). It is the *first* thing to strand-confine under any future (B) and the single natural place to `post`/`co_spawn` handler coroutines if dispatch ever needs to leave the decode stack.

---

## 7. Phased roadmap (independently-mergeable; manual loop removed LAST)

Re-binned from the design's order to resolve Critique 3 Issue 1: **the irreversible cut is the serial READ path, not "step 5"** — a `SerialReadLoop` doing `async_read_some` and a synchronous `Poll()` doing non-blocking `read_some` cannot both own one `boost::asio::serial_port` (they'd steal each other's bytes). The moment the read coroutine exists, `protocol_task->Poll()` *must* leave `:764` and the `had_work` pacing signal dies. So the read-path conversion + frame-loop removal are **one atomic commit (Phase 6)**; everything before it ships alongside the still-running loop.

Each phase states: **goal · concrete changes · verification · invariant it must not break.**

**Phase 0 — Boost spike & version pin (enabling, no behaviour change).**
*Goal:* prove the load-bearing primitives compile under the real toolchain. *Changes:* one throwaway TU using `experimental::channel` + `awaitable_operators` + `as_tuple` (none exist in `src/` today — verified; `co_spawn` exists only in `webhook_sink.cpp`, `webroute_diagnostics_matter.cpp`, and `test_mqtt_performance.cpp`); pin a Boost floor and record it in `vcpkg.json`/baseline (`boost-asio` is currently unpinned). *Verify:* configures + builds clean on the CI toolchain (per CLAUDE.md, no platform conditionals — shared CMakeLists). *Invariant:* none touched.

**Phase 0.5 — Hub-affinity assertion scaffolding (enabling; debug-only, no release behaviour change).**
*Goal:* make the single-thread invariant machine-checked before any driver changes, and pre-stage the future-(B) strand checklist (resolves open question 1; §2.2). *Changes:* add `Kernel::HubExecutor` + `Kernel::RunningOnHub()` + `AQUALINK_ASSERT_ON_HUB()` (debug-only); register the hub thread at startup; wire the assert into the documented single-thread sites (HubLocator, hub setters/emits, `WebSocket_Equipment::m_Connections`, `MessageGeneratorRegistry`, `Restartable::s_Instances`, the static per-message signal, `MqttClient::Publish` + diagnostics accessors). *Verify:* full suite green in **debug** (asserts hold on the one thread); release binary unchanged (asserts compiled out); the pre-existing Matter worker thread (H-1) does not trip it. *Invariant:* none changed — this only *observes* the existing one.

**Phase 1 — Test-harness enabling step (§4).**
*Goal:* decouple the harness from the production driver before any driver changes. *Changes:* introduce `ProtocolTask::StepOnce()` (today's `Poll()` body, retaining `m_SingleReadPerPoll`); point `MockReplayHarness::ReplayOne` at it. *Verify:* `test_replay_pacing.cpp`, `test_flow_command_to_wire.cpp`, `test_flow_fixture_replay.cpp`, and the nav suite all stay green unchanged. *Invariant:* the per-`Poll()` read bound + no-overflow guarantee (`test_replay_pacing.cpp`) and the synchronous decode/replay assertion surface.

**Phase 2 — Restartable → `WatchdogLoop` (§6.6).**
*Goal:* remove `PollAll()`. *Changes:* one `co_spawn`'d 1 s `steady_timer` sweeping a **snapshot copy** of `s_Instances`; delete `PollAll()` + the `:767` call. *Verify:* `test_devices_capabilities_restartable.cpp` adapts to a timer tick; watchdog fault-recovery behaviour unchanged. *Invariant:* registry not mutated mid-sweep (now via snapshot).

**Phase 3 — Serial WRITE → channel + `SerialWriteLoop` (§6.1).**
*Goal:* convert the write path in isolation (read `Poll()` untouched). *Changes:* `ConnectWriteSignal` slot `try_send`s a channel; `SerialWriteLoop` drains via `async_write`; the channel drains on the existing `io_context.poll()` so the loop still runs. *Verify:* `test_flow_command_to_wire.cpp` write-byte assertions still pass (the channel delivers the same bytes). *Invariant:* single-writer on the port; drop-oldest backpressure preserved.

**Phase 4 — WS egress → per-connection writer coroutine + channel (§5.2/H6).**
*Goal:* remove `HttpServer::Poll`/`HttpSessionState::Poll`/`TryWsWrite`-kick. *Changes:* per-connection writer drains an egress channel; broadcast slots `try_send`; accept/read already async. *Verify:* WS e2e/Playwright still receives broadcasts; no double-writer. *Invariant:* exactly one writer per beast stream; close-channel-before-stream on teardown.

**Phase 5 — MQTT → supervisor (§6.2, §5.2/H4).**
*RECONCILED 2026-06-30 — the library-adoption pre-decision is DONE:* `async-mqtt` was adopted (v0.5.0-beta.1, §6.2), so the "hand-roll the supervisor" alternative below is moot — the library owns framing/keepalive/reconnect. What remains of this phase is the small app-glue step: replace the per-frame `MqttClient::Poll()`→`FlushIfConnected()` drain with a co_spawned writer and drop the per-frame `mqtt_integration->Poll()` call. *Goal:* remove the remaining MQTT `Poll()` plumbing from the frame loop. *Changes (hand-rolled path):* `MqttSupervisor` + Reader/Writer/Keepalive; retire `m_ConnectGeneration`; DISCONNECT via `co_await Shutdown()`. *Verify:* `test_mqtt_integration.cpp`/`test_mqtt_hub.cpp` adapt to a `run_one()`-style driver; reconnect/keepalive/drop-oldest covered. *Invariant:* publish-queue drop-oldest; no synchronous socket write while an async op is in flight.

**Phase 6 — THE CUT: serial READ + delete frame loop + `run()` + structured shutdown (§5, Issue 1).**
*Goal:* the headline win. *Changes (one atomic commit):* `SerialReadLoop` (`async_read_some`); add the async `ISerialPortImpl` surface + deterministic mock completion (§4 deliverable 2); delete the `while (!shutdown)` loop (~867-943), both `poll()` drains, `had_work`, and the idle `sleep_until`; `co_spawn` all subsystems; `io_context.run()`; replay pacing → mock producer; Tracy frame mark → per-read-iteration; cancellation-signal shutdown with the §5.2 join + the exact teardown reset order (post-"STOP APPLICATION" banner, ~945-1034). `StepOnce()` retained for the harness. *Verify:* full unit + integration + fixture-replay + e2e suites green; replay-mode shutdown does not hang (H8); IRecordingController-before-serial-reset preserved. *Invariant:* single-thread no-lock hub model; teardown object-destruction order.

**Phase 7 — Blocking I/O → `blocking_pool` (§5.3, the (C) sliver).**
*Goal:* take SQLite/file writes off the io_context thread. *Changes:* size-1 serialising pool; confine HistoryService/scheduler/cache/preferences *whole mutable state* to it; hub snapshots taken on the io_context thread before any hop. *Verify:* history/scheduler/preferences tests green; no hub getter called off-thread. *Invariant:* H-3 executor discipline; no race on service-owned maps.

**Phase 8 — (separate RFC) honest `AwaitOutcome` (§6.3).**
*Goal:* opt-in real command completion **without** changing `ICommandDispatcher` signatures. *Changes:* add `AwaitOutcome(handle)` alongside the synchronous return; deadline-bounded. *Verify:* existing `command_to_wire` suite untouched + new await-path tests; the previously-masked RSSA no-op now observable under `AwaitOutcome`. *Invariant:* optimistic-200 contract and committed tests unchanged.

Phases 0-6 deliver the frame loop removal, latency win, and lock-free model intact. The nav-driver coroutine rewrite (§6.4) and Phase 8 are deferrable follow-ups.

---

## 8. Risks, rollback, and open questions

**Top risks.** (1) **The single-thread no-lock invariant** is structural and pervasive — any slip to a multi-threaded `run()` silently races the entire hub/signals2/consumer graph (Critique 1). Mitigation: (A) stays single-thread; the documented contracts become assert checklists, never deletions. (2) **Shutdown UAF/hang** if §5.2 is not honoured exactly (Critique 2 H1-H4, H6, H8) — the single biggest correctness hazard, now a binding contract, not a "risk." (3) **The Phase 6 cut is irreducibly big-bang** (one serial-port owner) — concentrate review and fixture/e2e coverage there. (4) **Test breakage from honesty** (Issue 3) — neutralised by the §6.3 additive-`AwaitOutcome` decision. (5) **Boost `experimental` API drift** — neutralised by Phase 0's spike + pin.

**Rollback strategy.** Phases 0-5 and 7-8 are independently revertable (each removes one `Poll()` consumer or adds an isolated facility; the manual loop still drives anything not yet converted). **Phase 6 is the only non-revertable-in-isolation step** — its rollback is a full revert of that commit. Therefore Phase 6 lands only after 0-5 are merged and green, behind a feature checkout/branch with the full suite (unit + integration + fixture-replay + Playwright e2e) passing, so the diff to bisect is bounded. Keep `StepOnce()` permanently (it is the harness contract *and* a cheap manual-step diagnostic), so a regression in the coroutine read path can always be A/B'd against the synchronous step.

**Open questions needing a human decision.**
1. **Single-thread forever, or a future (B)? — RESOLVED: pre-invest.** This plan commits to (A) now and keeps the (B) door open by building the hub-affinity assertion scaffolding up front (Phase 0.5; §2.2), so the strand-confine checklist is enforced from the start rather than reconstructed later. The remaining sub-decision — actually *running* a second `io_context` thread — stays a separate, profiling-gated effort.
2. **Nav timeout semantics (§6.4).** Frame-count limits (`MAX_WAIT_CYCLES`, `ONETOUCH_*_STEP_LIMIT`) scale with bus rate today; real `steady_timer` deadlines change behaviour. Pick the wall-clock values and re-tune against live captures — a hardware-gated decision.
3. **Phase 8 honesty rollout.** Do we want `AwaitOutcome` to *surface* the currently-masked RSSA aux-toggle no-op to users (a visible failure/timeout where today there is a silent optimistic 200)? That is a product decision, not just an engineering one.
4. **PDA `Scrapeable` convergence.** A second scraping engine (`Capabilities::Scrapeable`) duplicates the Navigator wait-stack/recovery handshake. Converge both nav engines during the coroutine rewrite, or let them drift (deferred today per `scrapeable.h`)? Affects Phase-6+ scope.
5. **`blocking_pool` sizing (§5.3).** Size-1 (serialising, simplest, preserves per-subsystem ordering) vs. size-N (parallel SQLite + file I/O, needs per-subsystem strands). Recommend size-1 first; revisit only if profiling shows DB latency matters.
6. **MQTT: hand-roll the coroutine client, or adopt a vcpkg library — and which (§6.2)? — RESOLVED (v0.5.0-beta.1): adopt `async-mqtt`.** Rather than choose between keeping v3.1.1 and moving to v5, the rewrite uses async-mqtt's runtime-`protocol_version` endpoint and exposes the choice as the `--mqtt-protocol-version` option — 3.1.1 stays the default and v5 becomes opt-in, getting both without a forced wire-protocol change. The highest-risk hand-rolled code (framing/keepalive/reconnect/CONNACK) is gone.

**Key files (all absolute; line numbers RECONCILED 2026-06-30 — symbol-anchored, treat as approximate):** `R:\aqualink-automate\src\aqualink-automate.cpp` (the `while (!shutdown)` frame loop ~867-943; teardown after the "STOP APPLICATION" banner ~945-1034), `R:\aqualink-automate\src\core\protocol\protocol_thread.{h,cpp}` (`ProtocolTask::Poll`, `ConnectWriteSignal`, `m_SingleReadPerPoll`), `R:\aqualink-automate\src\core\devices\capabilities\restartable.cpp` (`Restartable::PollAll`, no-copy sweep), `R:\aqualink-automate\src\core\http\server\http_server.cpp` (`HttpServer::Poll`), `R:\aqualink-automate\src\core\http\webroute_diagnostics_matter.cpp` (the Matter sidecar worker thread), `R:\aqualink-automate\src\core\mqtt\mqtt_client.cpp` (now async-mqtt-backed: `MqttClient::Poll`→`Impl::FlushIfConnected`, `Publish`, `CalculateReconnectDelay`) and `R:\aqualink-automate\src\core\mqtt\mqtt_client.h` (`MAX_PUBLISH_QUEUE_SIZE`), `R:\aqualink-automate\src\core\alerting\{webhook_sink,alert_monitor}.cpp`, `R:\aqualink-automate\src\core\history\history_service.cpp`, `R:\aqualink-automate\src\core\kernel\{data_hub,hub_locator}.{h,cpp}` (the `ConfigUpdateSignal` emit is in `DataHub::EmitTemperatureEvent`, data_hub.cpp ~28 — NOT the `:19` include block; `hub_locator.h` single-thread contract), `R:\aqualink-automate\src\jandy\navigation\{navigator,spider_engine}.cpp` (`navigator.h` `m_pCurrentContent`, `SpiderEngine::OnStatusReceived` no-double-decrement), `R:\aqualink-automate\src\jandy\devices\capabilities\emulated.h`, `R:\aqualink-automate\src\core\developer\mock_serial_port_impl.cpp` (`HandleMockWrite` `sleep_for`), `R:\aqualink-automate\test\unit\protocol\test_replay_pacing.cpp`, `R:\aqualink-automate\test\integration\flows\test_flow_command_to_wire.cpp`, `R:\aqualink-automate\test\unit\support\unit_test_mockreplayharness.{h,cpp}`, `R:\aqualink-automate\vcpkg.json` (declares `async-mqtt`+`tls`; `boost-asio` unpinned).

No code was modified.
