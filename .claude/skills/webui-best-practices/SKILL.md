---
name: cpp23-webui
description: >
  Conventions and quality rules for a C++23 backend application with an Alpine.js web UI
  communicating via WebSocket and REST APIs. Use when writing, reviewing, or modifying C++
  backend code (API handlers, WebSocket handlers, business logic, JSON serialisation), Alpine.js
  stores, HTML templates with Alpine directives, CSS, vanilla JavaScript, or the WebSocket
  message protocol that connects the two layers. Also use when adding new message types,
  endpoints, or frontend features, or when reviewing cross-boundary changes.
---

# C++23 Backend + Alpine.js Web UI Skill

## Architecture Overview

- **C++ backend** (`src/`) — sole authority for game state, monetary calculations, and compliance logic.
- **Alpine.js frontend** (`web/`) — display and input layer only; sends user intents, never computed results.
- **Communication** — WebSocket for real-time state push; REST API for request/response operations.
- **Message contract** — typed messages (`{ "type": "...", "payload": {} }`) defined as C++ enum classes and mirrored as JS constants.

The frontend must never perform authoritative calculations. All validation happens server-side.

## Quick Rules

### C++ Backend

1. Use `std::expected` for fallible operations — prefer over exceptions in handlers.
2. Return consistent JSON envelopes: `{ "ok": bool, "data": {}, "error": "" }`.
3. Validate and sanitise ALL incoming data (REST body, query params, WebSocket payloads) before processing.
4. Define serialisation adjacent to data models, not scattered across handlers.
5. Rate-limit WebSocket messages per session. Implement heartbeat/ping-pong.
6. Clean up session state (timers, subscriptions) on disconnect.
7. Run `clang-format` and `clang-tidy` after every C++ change.

### WebSocket Protocol

8. Message types defined in `src/ws/messages.hpp` (C++ enum class) AND `web/js/message-types.js` (JS constants). **Always update both sides together.**
9. Document every message type with payload shape in `PROTOCOL.md`.
10. Use sequence numbers or timestamps to handle out-of-order messages.

### Alpine.js / HTML / CSS / JS

11. One Alpine.js store per bounded concern (e.g., `connection`, `game`, `user`).
12. Stores dispatch intents via WebSocket — never compute authoritative logic.
13. Use `x-cloak` + `[x-cloak] { display: none }` to prevent FOUC.
14. Use `x-text` for dynamic content (auto-escapes). Never use `x-html` with server data.
15. Use semantic HTML elements. All interactive elements must be keyboard-navigable.
16. Add `aria-live` regions for values updated via WebSocket (balances, game state, errors).

### Cross-Boundary Changes

17. After adding/modifying a message type: update C++ enum, JS constants, TypeScript types (if any), PROTOCOL.md, and write a test.
18. After modifying an API endpoint: rebuild backend, run integration tests, update any consuming JS code.
19. Run E2E tests (Playwright) before marking any cross-stack feature complete.
20. After a visible UI change (layout, theme, navigation, new/renamed views or controls, auth screens, language list): regenerate the affected documentation screenshots in `docs/assets/` with `scripts/capture-doc-screenshots.js` and commit them with the change (see "Documentation screenshots" in CLAUDE.md).

## Verification Commands

Adapt these to match the project's actual build system:

```
Build backend:     cmake --build build/ --target app
C++ tests:         ctest --test-dir build/ --output-on-failure
Lint C++:          clang-tidy src/**/*.cpp
Format C++:        clang-format -i src/**/*.cpp src/**/*.hpp
Lint JS:           eslint web/js/
E2E tests:         npx playwright test
Dev server:        ./build/app --dev --port 8080
```

After **every** change:
1. C++ modified → build + ctest.
2. API/WS handler modified → rebuild + integration tests.
3. JS/HTML/CSS modified → eslint + Playwright.
4. Message schema changed → update both sides + E2E.
5. Always run formatters/linters before considering the task complete.

## Reference Files

For detailed guidance on specific areas, read these files in this skill's directory:

| File | When to read |
|---|---|
| `references/cpp23-backend.md` | Writing or reviewing C++ handlers, JSON serialisation, C++23 feature usage |
| `references/websocket-protocol.md` | Adding message types, connection management, reconnection logic |
| `references/alpinejs-frontend.md` | Working on stores, HTML templates, CSS, JS file organisation |
| `references/ux-patterns.md` | Loading states, real-time updates, error handling, accessibility, responsive design |
| `references/security.md` | Security rules for both server-side C++ and client-side JS/HTML |
| `references/testing.md` | Testing pyramid, what to test at each layer |
| `references/observability.md` | Logging, debugging, metrics, and monitoring |
| `references/workflow.md` | Claude Code workflow patterns: planning, hooks, subagents, context management |
