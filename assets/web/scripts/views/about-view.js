/**
 * About View — Version info component
 *
 * The licence / copyright line and the "Licences" section render from
 * `window.AquaNotices` (scripts/config/third-party-notices.js), the one place
 * the project's own notice and everything it ships from others is written down.
 */
function aboutView() {
    // Group order and heading keys for the third-party list. A literal map (not
    // string concatenation) so scripts/check-i18n-keys.ps1 sees the keys.
    const NOTICE_GROUPS = [
        { kind: 'font', labelKey: 'about.licences_fonts' },
        { kind: 'script', labelKey: 'about.licences_scripts' },
        { kind: 'icon', labelKey: 'about.licences_icons' }
    ];

    // Same http(s)-only rule as the homepage link: notices are static data, but
    // every href this view emits goes through one gate.
    const httpUrl = (url) => {
        if (!url) return '';
        try {
            const parsed = new URL(url);
            if (parsed.protocol === 'http:' || parsed.protocol === 'https:') return url;
        } catch { /* invalid URL */ }
        return '';
    };

    const notices = () => window.AquaNotices || { app: {}, licences: {}, components: [] };

    return {
        get shortCommit() {
            const sha = Alpine.store('pool').gitCommit;
            return sha ? sha.substring(0, 7) : '--';
        },

        get safeHomepage() {
            return httpUrl(Alpine.store('pool').projectHomepage);
        },

        /** Application's own notice: licence name, licence file, copyright line. */
        get appNotice() {
            return notices().app || {};
        },

        /** Third-party components grouped by kind (fonts / scripts / icons). */
        get noticeGroups() {
            const { components, licences } = notices();
            return NOTICE_GROUPS
                .map((group) => ({
                    kind: group.kind,
                    labelKey: group.labelKey,
                    items: components
                        .filter((c) => c.kind === group.kind)
                        .map((c) => {
                            const lic = licences[c.licence] || {};
                            return {
                                name: c.name,
                                version: c.version || '',
                                copyright: c.copyright || [],
                                notes: c.notes || [],
                                url: httpUrl(c.url),
                                files: c.files || [],
                                licenceName: lic.name || c.licence,
                                licenceUrl: httpUrl(lic.url)
                            };
                        })
                }))
                .filter((group) => group.items.length > 0);
        },

        /** Only the licences some listed component actually uses, full text included. */
        get noticeLicences() {
            const { components, licences } = notices();
            const used = new Set(components.map((c) => c.licence));
            return Object.entries(licences)
                .filter(([id]) => used.has(id))
                .map(([id, lic]) => ({ id, name: lic.name, url: httpUrl(lic.url), text: lic.text }));
        }
    };
}
