/**
 * Equipment Button — toggle component logic with command feedback.
 *
 * Button status values from the API (magic_enum string representations):
 *   Auxillary: On, Off, Enabled, Pending, Unknown
 *              (Cleaner, Light, Spillover and Sprinkler share this vocabulary)
 *   Pump:      Off, Running, NotInstalled, Unknown
 *   Heater:    Off, Heating, Enabled, NotInstalled, Unknown
 *
 * iconKey() maps a device to one of the line-art icons defined in the SVG sprite
 * in index.html (id="icon-<key>"), preferring the backend device_type and falling
 * back to label keywords. The template renders <svg><use href="#icon-<key>"></svg>.
 */
function equipmentButton() {
    const ui = (typeof window !== 'undefined' && window.AquaUI) || {};
    const isActiveStatus = ui.isActiveStatus || ((status) => {
        const s = String(status == null ? '' : status).toLowerCase();
        return s === 'on' || s === 'running' || s === 'heating' || s === 'enabled';
    });

    const TYPE_ICON = {
        light: 'light',
        pump: 'pump',
        cleaner: 'clean',
        heater: 'heater',
        spillover: 'waves',
        sprinkler: 'droplet',
        auxillary: 'power',
    };

    return {
        isActive(button) {
            return isActiveStatus(button.status);
        },

        statusLabel(button) {
            return ui.buttonStatusLabel ? ui.buttonStatusLabel(button.status || 'Unknown') : String(button.status || 'Unknown');
        },

        cmdState(button) {
            return Alpine.store('pool').getCommandState(button.id);
        },

        // Returns an SVG sprite key (see #icon-* symbols in index.html).
        iconKey(button) {
            const dt = String(button.device_type || '').toLowerCase();
            if (TYPE_ICON[dt]) { return TYPE_ICON[dt]; }

            const label = String(button.label || '').toLowerCase();
            if (label.includes('light')) { return 'light'; }
            if (label.includes('filter')) { return 'filter'; }
            if (label.includes('pump')) { return 'pump'; }
            if (label.includes('heat')) { return 'heater'; }
            if (label.includes('spa')) { return 'waves'; }
            if (label.includes('clean') || label.includes('vac')) { return 'clean'; }
            if (label.includes('jet') || label.includes('blow')) { return 'wind'; }
            if (label.includes('spill') || label.includes('fall') || label.includes('pool')) { return 'waves'; }
            if (label.includes('sprink') || label.includes('water')) { return 'droplet'; }
            if (label.includes('valve')) { return 'valve'; }
            return 'power';
        },

        async toggle(button) {
            await Alpine.store('pool').toggleButton(button.id);
        },
    };
}
