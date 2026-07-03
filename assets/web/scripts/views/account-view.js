/**
 * Account view (Wave A) — the signed-in user's self-service surface.
 *
 * Rendered as a modal-style overlay (see the .account-overlay markup in
 * index.html) reachable from the nav when authenticated.  It shows the current
 * identity + groups and offers:
 *   - CHANGE PASSWORD -> PUT /api/users/{me.id}/password (new + confirm, min 12).
 *     The API does not require the current password; it kills the user's OTHER
 *     sessions on success, so we re-fetch the session list afterwards.
 *   - SESSIONS list (GET /api/sessions) with device / last-seen and a Revoke
 *     button (DELETE /api/sessions/{id}).
 *   - SIGN OUT (this session) and SIGN OUT EVERYWHERE, delegated to the auth store.
 *
 * All fetches go through the AqualinkAuth wrapper, so the bearer token (and its
 * silent refresh) are handled transparently.
 */
const ACCOUNT_PASSWORD_MIN = 12;

function accountView() {
    return {
        open: false,

        // Exposed for the password-placeholder catalog binding ({min}).
        passwordMin: ACCOUNT_PASSWORD_MIN,

        // Change-password form.
        newPassword: '',
        confirmPassword: '',
        pwError: '',
        pwSaved: false,
        pwBusy: false,

        // Sessions.
        sessions: [],
        sessionsError: '',
        sessionsLoading: false,

        show() {
            this.open = true;
            this._resetPasswordForm();
            this.fetchSessions();
        },

        hide() {
            this.open = false;
        },

        // ---- Identity (read from the auth store) ----
        // Prefer the human-readable username; the id (a stable UUID for local
        // accounts) is only the fallback for subjects with no natural name
        // (e.g. the kiosk session's synthetic "kiosk" id).
        get identity() {
            const auth = this.$store.auth;
            return auth.username || auth.id || window.AquaI18n.t('account.unknown_user');
        },
        get groups() { return this.$store.auth.groups || []; },

        _resetPasswordForm() {
            this.newPassword = '';
            this.confirmPassword = '';
            this.pwError = '';
            this.pwSaved = false;
        },

        // ---- Change password ----
        async changePassword() {
            this.pwError = '';
            this.pwSaved = false;

            if (this.newPassword.length < ACCOUNT_PASSWORD_MIN) {
                this.pwError = window.AquaI18n.t('account.password_too_short', { min: ACCOUNT_PASSWORD_MIN });
                return;
            }
            if (this.newPassword !== this.confirmPassword) {
                this.pwError = window.AquaI18n.t('account.passwords_no_match');
                return;
            }

            const id = this.$store.auth.id;
            if (!id) { this.pwError = window.AquaI18n.t('account.error_not_signed_in'); return; }

            this.pwBusy = true;
            try {
                const resp = await fetch(`/api/users/${encodeURIComponent(id)}/password`, {
                    method: 'PUT',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ password: this.newPassword }),
                });
                if (resp.status === 204) {
                    this.pwSaved = true;
                    this.newPassword = '';
                    this.confirmPassword = '';
                    // The API revokes the user's OTHER sessions on a password
                    // change — reflect that by re-fetching the list.
                    this.fetchSessions();
                    setTimeout(() => { this.pwSaved = false; }, 2500);
                } else if (resp.status === 400) {
                    this.pwError = window.AquaI18n.t('account.password_too_short', { min: ACCOUNT_PASSWORD_MIN });
                } else {
                    this.pwError = window.AquaI18n.t('account.error_change_password', { status: resp.status });
                }
            } catch (_) {
                this.pwError = window.AquaI18n.t('account.error_network');
            } finally {
                this.pwBusy = false;
            }
        },

        // ---- Sessions ----
        async fetchSessions() {
            this.sessionsError = '';
            this.sessionsLoading = true;
            try {
                const resp = await fetch('/api/sessions');
                if (!resp.ok) {
                    this.sessionsError = window.AquaI18n.t('account.error_load_sessions', { status: resp.status });
                    this.sessions = [];
                    return;
                }
                const list = await resp.json();
                this.sessions = Array.isArray(list) ? list : [];
            } catch (_) {
                this.sessionsError = window.AquaI18n.t('account.error_network_sessions');
                this.sessions = [];
            } finally {
                this.sessionsLoading = false;
            }
        },

        async revokeSession(id) {
            if (!id) return;
            try {
                const resp = await fetch(`/api/sessions/${encodeURIComponent(id)}`, { method: 'DELETE' });
                if (resp.status === 204) {
                    this.sessions = this.sessions.filter((s) => s.id !== id);
                } else {
                    this.sessionsError = window.AquaI18n.t('account.error_revoke_session', { status: resp.status });
                }
            } catch (_) {
                this.sessionsError = window.AquaI18n.t('account.error_network_revoke');
            }
        },

        // ---- Sign out ----
        async signOut() {
            this.hide();
            await this.$store.auth.logout();
        },

        async signOutEverywhere() {
            this.hide();
            await this.$store.auth.logoutEverywhere();
        },

        // ---- Display helpers ----
        // A compact "browser / OS" label from the user agent for the session row.
        // Browser / OS product names are brand tokens and stay untranslated; the
        // composition and the fallbacks go through the catalog.
        deviceLabel(ua) {
            if (!ua) return window.AquaI18n.t('account.unknown_device');
            let browser = window.AquaI18n.t('account.generic_browser');
            if (/Edg\//.test(ua)) browser = 'Edge';
            else if (/OPR\//.test(ua)) browser = 'Opera';
            else if (/Chrome\//.test(ua)) browser = 'Chrome';
            else if (/Firefox\//.test(ua)) browser = 'Firefox';
            else if (/Safari\//.test(ua)) browser = 'Safari';

            let os = '';
            if (/Windows/.test(ua)) os = 'Windows';
            else if (/Android/.test(ua)) os = 'Android';
            else if (/iPhone|iPad|iOS/.test(ua)) os = 'iOS';
            else if (/Mac OS X|Macintosh/.test(ua)) os = 'macOS';
            else if (/Linux/.test(ua)) os = 'Linux';

            return os ? window.AquaI18n.t('account.device_on', { browser, os }) : browser;
        },

        // "3 minutes ago" style relative time from a unix seconds value.
        relativeTime(unixSeconds) {
            if (!unixSeconds) return '';
            const deltaSec = Math.max(0, Math.floor(Date.now() / 1000) - unixSeconds);
            if (deltaSec < 60) return window.AquaI18n.t('time.just_now');
            const i18n = Alpine.store('i18n');
            const mins = Math.floor(deltaSec / 60);
            if (mins < 60) return i18n.tn('time.minutes_ago_long', mins);
            const hours = Math.floor(mins / 60);
            if (hours < 24) return i18n.tn('time.hours_ago_long', hours);
            const days = Math.floor(hours / 24);
            return i18n.tn('time.days_ago_long', days);
        },
    };
}
