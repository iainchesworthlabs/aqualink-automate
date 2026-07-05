#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "options/options_preferences_options.h"

namespace AqualinkAutomate::Kernel { class PreferencesHub; class HubLocator; }

namespace AqualinkAutomate::Preferences
{

	//=========================================================================
	// PreferencesService — load/save user-and-admin preferences and keep them in
	// the PreferencesHub (which the relevant services read live).
	//
	// Precedence at boot: persisted file (if any) overrides the CLI seed, which
	// overrides the built-in defaults.  Mutations via ApplyJson() persist
	// immediately (atomic write-temp-then-rename), like the schedules file.
	// Empty --preferences-file => no persistence (preferences still work for the
	// session, but reset on restart).
	//=========================================================================
	class PreferencesService
	{
	public:
		PreferencesService(Kernel::HubLocator& hub_locator, const Options::Preferences::PreferencesSettings& settings);

		// Seed the runtime-editable fields from the effective CLI/config values so
		// existing deployments behave identically until a preference is changed.
		void Seed(std::uint32_t salt_low_ppm, std::uint32_t comms_timeout_seconds, const std::string& webhook_url, std::uint32_t retention_days) const;

		// Load the persisted file (overriding the seed) when a path is configured.
		void Start();

		// Serialise the current preferences (used by GET /api/preferences).
		nlohmann::json ToJson() const;

		// Validate + apply a (partial) preferences document, then persist. On any
		// invalid field nothing is applied and `error` describes the problem.
		bool ApplyJson(const nlohmann::json& json, std::string& error);

		// As above, additionally surfacing a stable snake_case `error_code` the
		// web UI maps to a translated message (catalog key `error.<code>`,
		// docs/i18n.md).
		bool ApplyJson(const nlohmann::json& json, std::string& error, std::string& error_code);

		// Record a single REQUESTED spa-switch button->function assignment (keyed
		// "<switch>:<button>") into PreferencesHub::SpaSwitchButtons and persist. Called by the
		// spaside assign route when a controller accepts a programming request, so the desired
		// mapping is remembered (the live decoded map remains authoritative).
		void RecordSpaSwitchAssignment(std::uint8_t switch_number, std::uint8_t button_number, const std::string& function);

	private:
		void Load();
		void Save() const;

	private:
		std::shared_ptr<Kernel::PreferencesHub> m_Hub;
		Options::Preferences::PreferencesSettings m_Settings;
	};

}
// namespace AqualinkAutomate::Preferences
