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

                ws.onclose = () => {
                    onDown();
                    if (name === 'equipment') {
                        Alpine.store('toast').show(window.AquaI18n.t('toast.conn_lost_retrying'), 'warn');
                    }
                    this._scheduleReconnect(name);
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
});
