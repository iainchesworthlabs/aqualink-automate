# consteval opportunities — aqualink-automate C++23 codebase

_Branch `refactor/looking-for-constexpr` · multi-agent find→verify workflow over all 1,025 `.cpp/.h/.hpp` files in `src/`, using a stricter lens than the constexpr audit._

## Why this list is short (by design)

`consteval` (an *immediate function*) can **only** be called in a constant expression — every call site must supply constant arguments. That makes it correct for a symbol **only when it has no runtime call sites and none could reasonably exist**. Most pure functions in this codebase are legitimately called with runtime values (checksums over live packets, enum→label mappers on device data, conversions on incoming temperatures) — those are `constexpr` opportunities, not `consteval`, and are covered by the companion [constexpr-opportunities.md](constexpr-opportunities.md).

Every finder was told that **zero results per batch is the expected normal case**, and the verifier was told to REJECT anything with a plausible runtime call site. Across all 81 batches, exactly one symbol survived.

## Confirmed (1) · Rejected (0)

### `AqualinkAutomate::Interfaces::IsSameStatusType`

- **File:** `src/core/interfaces/istatuspublisher.h`:79
- **Current form:** constexpr free function template ([[nodiscard]] constexpr bool ... noexcept)
- **Category:** compile-time-only helper -> consteval (never called at runtime)
- **Effort / confidence:** minor / high  ·  verdict: CONFIRMED
- **Why compile-time-only is correct:** IsSameStatusType is a pure type-level predicate taking NO runtime parameters; its result is std::same_as<remove_cvref_t<THIS_STATUS>, remove_cvref_t<THAT_STATUS>>, fully determined by the two template type arguments. There is no runtime input it could depend on, so compile-time-only evaluation is always correct. It is documented as 'a compile-time, type-only relation' that 'inspects no runtime field'; consteval makes that intent enforceable.
- **No runtime callers (evidence):** Grep across the whole worktree shows the decl at istatuspublisher.h:79 and call sites ONLY in test/unit/statuses/test_statusupdate_equipmentanddevices.cpp: lines 26-27 (inside BOOST_CHECK), 107-108 and 111 (inside BOOST_CHECK), 114-115 (inside static_assert). No production call sites anywhere. Every call passes only template type arguments (e.g. IsSameStatusType<DeviceStatus_Normal, DeviceStatus_Normal>()); the function has zero runtime parameters so no runtime-valued call is expressible. BOOST_CHECK receives an already-evaluated bool from a constant-expression argument, and static_assert is already a constant context — consteval compiles all sites unchanged.
- **Benefit:** Guarantees zero runtime cost and turns any accidental non-constant use into a compile error, hardening the documented type-only contract. Cannot break callers because a call with only template arguments is inherently a constant expression.
- **Verifier note:** Verified istatuspublisher.h:77-82: no runtime parameters, body is a pure type predicate (std::same_as over the two template args). Worktree-wide grep found call sites only in the test file, all invoked as IsSameStatusType<TypeA,TypeB>() with template arguments only. No production or runtime-valued call site exists or is expressible; consteval is correct and worthwhile, and existing BOOST_CHECK/static_assert sites remain valid constant expressions.

## What was searched but did NOT qualify

The finders specifically considered — and correctly did not surface as consteval — the following common shapes, because each has (or plausibly needs) runtime call sites: Jandy/Pentair checksum & DLE-escape helpers, enum→string label mappers, temperature/unit conversions, HTTP status mappers, and factory dispatch predicates. These remain valid **constexpr** targets in the companion report.

## Suggested follow-ups (not auto-surfaced, judgment calls)

If you want to *introduce* consteval rather than just relabel existing code, the natural seams are:
- A `consteval` validator for wire opcodes / device IDs so an out-of-range literal `MessageType`/`DeviceId` becomes a build error.
- A `consteval` builder for any `static constexpr std::array` lookup table (opcode→handler, capability tables) to guarantee the table never materialises at runtime.
- `consteval` UDLs for strong types (e.g. a `_celsius` / `_devid` literal) if such literals get introduced.

These are green-field opportunities, so they are listed as design suggestions rather than verified in-place findings.