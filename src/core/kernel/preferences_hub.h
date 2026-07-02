#pragma once

#include <cstdint>
#include <string>

#include <boost/signals2.hpp>
#include <nlohmann/json.hpp>

#include "interfaces/ihub.h"
#include "kernel/temperature.h"

namespace AqualinkAutomate::Kernel
{

	// User/admin preferences (as opposed to boot-only CLI infrastructure).
	//
	// The typed fields below are READ LIVE by the relevant services
	// (AlertMonitor, HistoryService, the webhook sink), so a change via the
	// preferences API takes effect without a restart.  They are seeded from the
	// CLI/config at boot (so existing deployments are unchanged), then overridden
	// by the persisted preferences file if present.
	//
	// UiPreferences is an opaque JSON blob the backend never interprets (e.g. the
	// chemistry gauge bands); it simply round-trips through the API + file so the
	// UI has a shared, server-persisted home for its display preferences.
	class PreferencesHub : public Interfaces::IHub
	{
	public:
		PreferencesHub();
		~PreferencesHub() override = default;

	public:
		TemperatureUnits Temperature_DisplayUnits{ TemperatureUnits::Celsius };

		// Fired by PreferencesService when Temperature_DisplayUnits changes.
		// The typed fields are read live, so most consumers need nothing —
		// this exists for consumers with PUBLISHED artefacts derived from the
		// preference (e.g. the HA discovery setpoint number entities), which
		// must refresh when it flips.
		mutable boost::signals2::signal<void()> DisplayUnitsChangedSignal;

		std::uint32_t AlertSaltLowPpm{ 2600 };
		std::uint32_t AlertCommsTimeoutSeconds{ 60 };
		std::string AlertWebhookUrl;
		std::uint32_t HistoryRetentionDays{ 90 };

		// When true, the backend appends the protocol-native id to a device's display_label,
		// e.g. "Pool Light (Aux5)". Default off (plain friendly name). Affects UI/display only
		// (the canonical label still drives dispatch / MQTT / Home Assistant).
		bool ShowAuxIdInLabel{ false };

		// User-friendly display names keyed by the device's canonical label
		// (e.g. {"Aux1": "Pool Light"}). The canonical label is unchanged (it
		// still drives dispatch / MQTT / HA); this only feeds a display_label.
		//
		// NOTE: copy-initialise with '=', NOT 'json x{ object() }'. Under
		// gcc/libstdc++ the brace form selects nlohmann's initializer_list
		// constructor and wraps the empty object in a 1-element array ([{}]),
		// which then fails the "must be an object" validation on reload and
		// silently discards the whole persisted preferences file. (MSVC treated
		// it as copy-init, so this only bit the Linux/gcc build.)
		nlohmann::json LabelOverrides = nlohmann::json::object();

		nlohmann::json UiPreferences = nlohmann::json::object();

		// Spa-side switch button -> function assignments REQUESTED by the user (via the spaside
		// "Set" control / assign API), keyed "<switch>:<button>" e.g. {"1:2": "Pool Light"}. This
		// is the desired-state record: the controller's live decoded map (DataHub) remains the
		// source of truth, but a OneTouch programs over the bus asynchronously and an iAQ-only
		// system cannot program at all, so persisting the request lets the UI reflect intent and
		// gives read-only systems an annotation. Same opaque-blob discipline as LabelOverrides
		// (copy-init with '=', see the note above).
		nlohmann::json SpaSwitchButtons = nlohmann::json::object();
	};

}
// namespace AqualinkAutomate::Kernel
