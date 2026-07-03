/**
 * WebSocket Store — Manages both WS connections with auto-reconnect
 */

// Reconnect backoff bounds (ms). Initial delay doubles up to the max on each
// failed (re)connection attempt and resets to the initial delay on a clean open.
const WS_RECONNECT_INITIAL_DELAY_MS = 1000;
const WS_RECONNECT_MAX_DELAY_MS = 30000;

// Per-message payload logging is opt-in (it logs every decoded WS frame). Enable
// at runtime with `localStorage.setItem('wsDebug', 'true')` then reload.
function _wsDebugEnabled() {
    try { return localStorage.getItem('wsDebug') === 'true'; } catch (_) { return false; }
}
const _WS_DEBUG = _wsDebugEnabled();

document.addEventListener('alpine:init', () => {
    Alpine.store('ws', {
        connected: false,
        statsConnected: false,

        // Per-connection runtime state. Each connection owns its socket, its
        // current backoff delay, and a single pending reconnect-timer handle so
        // reconnect attempts can never stack.
        _conns: {
            equipment: { socket: null, delay: WS_RECONNECT_INITIAL_DELAY_MS, reconnectTimer: null },
            stats:     { socket: null, delay: WS_RECONNECT_INITIAL_DELAY_MS, reconnectTimer: null }
        },

        wsUrl(path) {
            const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
            return `${protocol}//${window.location.host}${path}`;
        },

        connectEquipment() {
            this._connect({
                name: 'equipment',
                path: '/ws/equipment',
                onMessage: (msg) => {
                    if (_WS_DEBUG) console.debug('[WS /equipment]', msg.type, msg.payload);
                    Alpine.store('pool').handleEvent(msg);
                    Alpine.store('system').handleEvent(msg);
                    Alpine.store('alerts').handleEvent(msg);
                },
                onOpen: () => { this.connected = true; },
                onDown: () => { this.connected = false; }
            });
        },

        connectStats() {
            this._connect({
                name: 'stats',
                path: '/ws/equipment/stats',
                onMessage: (msg) => {
                    Alpine.store('stats').handleEvent(msg);
                },
                onOpen: () => {
                    this.statsConnected = true;
                    // A fresh socket may be a restarted server with reset
                    // counters — drop stale frequency baselines.
                    Alpine.store('stats').resetFrequency();
                },
                onDown: () => { this.statsConnected = false; }
            });
        },

        /**
         * Open (or re-open) a named WebSocket connection. Cancels any pending
         * reconnect timer and closes/detaches any prior socket before creating a
         * new one, so timers and sockets can never accumulate.
         */
        _connect({ name, path, onMessage, onOpen, onDown }) {
            const conn = this._conns[name];

            // A reconnect attempt fired (or an explicit reconnect) — clear the
            // pending timer so it cannot fire again on top of this attempt.
            if (conn.reconnectTimer) {
                clearTimeout(conn.reconnectTimer);
                conn.reconnectTimer = null;
            }

            // Already connecting/open — nothing to do.
            if (conn.socket && conn.socket.readyState <= WebSocket.OPEN) return;

            // Detach and discard any stale (closing/closed) socket first.
            this._closeSocket(conn);

            try {
                // Attach the bearer token (when present) as a WebSocket subprotocol;
                // browsers cannot set an Authorization header on the upgrade. Returns
                // undefined when no token is stored, i.e. an unauthenticated connect.
                const subprotocols = (window.AqualinkAuth && window.AqualinkAuth.wsSubprotocols())
                    ? window.AqualinkAuth.wsSubprotocols()
                    : undefined;
                const ws = subprotocols ? new WebSocket(this.wsUrl(path), subprotocols) : new WebSocket(this.wsUrl(path));
                conn.socket = ws;

                ws.onopen = () => {
                    conn.delay = WS_RECONNECT_INITIAL_DELAY_MS;
                    onOpen();
                };

                ws.onmessage = (event) => {
                    try {
                        const msg = JSON.parse(event.data);
                        onMessage(msg);
                    } catch (e) {
                        console.error(`WS /${name} parse error:`, e);
                    }
                };

                ws.onclose = (event) => {
                    onDown();
                    if (name === 'equipment') {
                        Alpine.store('toast').show(window.AquaI18n.t('toast.conn_lost_retrying'), 'warn');
                    }
                    // #20: the server closes sockets when a session is revoked or
                    // its access token expires. A browser can't read the upgrade's
                    // 401/403 status, so treat a close while we hold a token (under
                    // the identity system) as a possible auth rejection: refresh the
                    // access token BEFORE reconnecting so the new socket carries a
                    // valid bearer subprotocol. If the refresh fails the session is
                    // dead — surface the login card instead of looping.
                    this._reconnectWithAuth(name, event);
                };

                ws.onerror = (event) => {
                    console.warn(`WS /${name} error:`, event);
                    onDown();
                    // onclose follows onerror for failed connections and drives
                    // the reconnect; do not schedule here to avoid stacking.
                };
            } catch (e) {
                console.error(`WS /${name} connect failed:`, e);
                onDown();
                this._scheduleReconnect(name);
            }
        },

        // Detach handlers and close a connection's socket without triggering its
        // onclose-driven reconnect.
        _closeSocket(conn) {
            const ws = conn.socket;
            if (!ws) return;
            ws.onopen = null;
            ws.onmessage = null;
            ws.onclose = null;
            ws.onerror = null;
            try { ws.close(); } catch (_) { /* already closing/closed */ }
            conn.socket = null;
        },

        disconnectEquipment() {
            const conn = this._conns.equipment;
            if (conn.reconnectTimer) {
                clearTimeout(conn.reconnectTimer);
                conn.reconnectTimer = null;
            }
            this._closeSocket(conn);
            this.connected = false;
        },

        disconnectStats() {
            const conn = this._conns.stats;
            if (conn.reconnectTimer) {
                clearTimeout(conn.reconnectTimer);
                conn.reconnectTimer = null;
            }
            this._closeSocket(conn);
            this.statsConnected = false;
        },

        /**
         * Reconnect after a close, refreshing the access token first when the
         * close might be an auth rejection (#20). No token / posture-off => the
         * ordinary backoff reconnect. Holding a token => try one silent refresh;
         * on success the scheduled reconnect naturally uses the rotated token, on
         * failure the auth store shows the login card and we stop retrying.
         */
        _reconnectWithAuth(name, event) {
            const auth = window.AqualinkAuth;
            const authStore = (typeof Alpine !== 'undefined') ? Alpine.store('auth') : null;
            const identityOn = authStore && authStore.posture === 'enabled';
            const haveToken = auth && auth.token && auth.token();

            // Browsers report a rejected/aborted upgrade as an abnormal close
            // (1006) and a policy/auth close as 1008/1011; a clean 1000/1001 is a
            // normal teardown (e.g. server restart) that just needs a plain
            // reconnect. Only spend a single-use refresh token on the abnormal
            // codes, and only under the identity system with a token in hand.
            const code = event && typeof event.code === 'number' ? event.code : 1006;
            const maybeAuthClose = code !== 1000 && code !== 1001;

            if (!identityOn || !haveToken || !maybeAuthClose || typeof auth.tryRefresh !== 'function') {
                this._scheduleReconnect(name);
                return;
            }

            auth.tryRefresh().then((ok) => {
                if (ok) {
                    // Fresh token in hand — reconnect promptly with it.
                    this._scheduleReconnect(name);
                } else {
                    // Session is dead: drop tokens and surface login; do not loop.
                    if (typeof authStore._localSignOut === 'function') {
                        authStore._localSignOut();
                    } else {
                        window.dispatchEvent(new CustomEvent('auth:unauthorized'));
                    }
                }
            }).catch(() => this._scheduleReconnect(name));
        },

        _scheduleReconnect(name) {
            const conn = this._conns[name];
            // A reconnect is already pending — do not stack another timer.
            if (conn.reconnectTimer) return;

            const reconnect = name === 'equipment'
                ? () => this.connectEquipment()
                : () => this.connectStats();

            conn.reconnectTimer = setTimeout(() => {
                conn.reconnectTimer = null;
                reconnect();
            }, conn.delay);
            conn.delay = Math.min(conn.delay * 2, WS_RECONNECT_MAX_DELAY_MS);
        }
    });

    // Re-open both sockets whenever auth state settles — not just at boot.
    // `auth:ready` fires again after a successful login/logout (auth.js's
    // check()), and browsers can't attach a bearer token to an in-flight
    // WebSocket, so a socket opened before login stays anonymous forever
    // unless it's explicitly recreated here (mirrors prefs-sync.js's
    // un-gated `auth:ready` listener). _connect() safely replaces any
    // existing socket, so a redundant call right after app.js's own
    // first-boot connect is a harmless no-op.
    window.addEventListener('auth:ready', () => {
        const ws = Alpine.store('ws');
        if (!ws) return;
        ws.connectEquipment();
        ws.connectStats();
    });
});
