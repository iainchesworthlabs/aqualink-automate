#include <filesystem>
#include <format>
#include <fstream>

#include <boost/url/parse.hpp>
#include <boost/url/url_view.hpp>
#include <magic_enum/magic_enum.hpp>

#include "kernel/hub_locator.h"
#include "kernel/preferences_hub.h"
#include "logging/logging.h"
#include "preferences/preferences_service.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Preferences
{
	namespace
	{
		bool IsValidWebhookUrl(const std::string& url)
		{
			auto parsed = boost::urls::parse_uri(url);
			if (!parsed) { return false; }
			const auto scheme = parsed->scheme();
			return (scheme == "http" || scheme == "https") && parsed->has_authority();
		}

		// Each ValidateXxx helper mirrors one field block of ApplyJson: it either
		// leaves the out-param untouched (field absent) or, after validating, writes
		// the accepted value into it. On any invalid field it sets error/error_code
		// and returns false so the caller can abort before committing anything.

		bool ValidateTemperatureUnits(const nlohmann::json& json, Kernel::TemperatureUnits& units, std::string& error, std::string& error_code)
		{
			if (!json.contains("temperature_units"))
			{
				return true;
			}
			if (!json["temperature_units"].is_string())
			{
				error = "temperature_units must be 'Celsius' or 'Fahrenheit'";
				error_code = "invalid_temperature_units";
				return false;
			}
			auto parsed = magic_enum::enum_cast<Kernel::TemperatureUnits>(json["temperature_units"].get<std::string>());
			if (!parsed.has_value())
			{
				error = "temperature_units must be 'Celsius' or 'Fahrenheit'";
				error_code = "invalid_temperature_units";
				return false;
			}
			units = parsed.value();
			return true;
		}

		bool ValidateAlert(const nlohmann::json& json, std::uint32_t& salt, std::uint32_t& comms, std::string& webhook, std::string& error, std::string& error_code)
		{
			if (!(json.contains("alert") && json["alert"].is_object()))
			{
				return true;
			}
			const auto& a = json["alert"];
			if (a.contains("salt_low_ppm"))
			{
				if (!a["salt_low_ppm"].is_number_integer() || a["salt_low_ppm"].get<std::int64_t>() < 0 || a["salt_low_ppm"].get<std::int64_t>() > 6000)
				{
					error = "alert.salt_low_ppm must be 0..6000";
					error_code = "invalid_salt_low_ppm";
					return false;
				}
				salt = a["salt_low_ppm"].get<std::uint32_t>();
			}
			if (a.contains("comms_timeout_seconds"))
			{
				if (!a["comms_timeout_seconds"].is_number_integer() || a["comms_timeout_seconds"].get<std::int64_t>() <= 0)
				{
					error = "alert.comms_timeout_seconds must be greater than 0";
					error_code = "invalid_comms_timeout";
					return false;
				}
				comms = a["comms_timeout_seconds"].get<std::uint32_t>();
			}
			if (a.contains("webhook_url"))
			{
				if (!a["webhook_url"].is_string())
				{
					error = "alert.webhook_url must be empty or an absolute http/https URL";
					error_code = "invalid_webhook_url";
					return false;
				}
				const auto url = a["webhook_url"].get<std::string>();
				if (!url.empty() && !IsValidWebhookUrl(url))
				{
					error = "alert.webhook_url must be empty or an absolute http/https URL";
					error_code = "invalid_webhook_url";
					return false;
				}
				webhook = url;
			}
			return true;
		}

		bool ValidateHistory(const nlohmann::json& json, std::uint32_t& retention, std::string& error, std::string& error_code)
		{
			if (!(json.contains("history") && json["history"].is_object()))
			{
				return true;
			}
			if (const auto& h = json["history"]; h.contains("retention_days"))
			{
				if (!h["retention_days"].is_number_integer() || h["retention_days"].get<std::int64_t>() <= 0)
				{
					error = "history.retention_days must be greater than 0";
					error_code = "invalid_retention_days";
					return false;
				}
				retention = h["retention_days"].get<std::uint32_t>();
			}
			return true;
		}

		bool ValidateLabelOverrides(const nlohmann::json& json, nlohmann::json& label_overrides, std::string& error, std::string& error_code)
		{
			if (!json.contains("label_overrides"))
			{
				return true;
			}
			if (!json["label_overrides"].is_object())
			{
				error = "label_overrides must be an object of canonical->display strings";
				error_code = "invalid_label_overrides";
				return false;
			}
			for (const auto& [canonical, display] : json["label_overrides"].items())
			{
				if (!display.is_string())
				{
					error = "label_overrides values must be strings";
					error_code = "invalid_label_overrides";
					return false;
				}
			}
			label_overrides = json["label_overrides"];
			return true;
		}

		bool ValidateAuxPresenceOverrides(const nlohmann::json& json, nlohmann::json& aux_presence_overrides, std::string& error, std::string& error_code)
		{
			if (!json.contains("aux_presence_overrides"))
			{
				return true;
			}
			if (!json["aux_presence_overrides"].is_object())
			{
				error = "aux_presence_overrides must be an object of aux-id->\"present\"/\"absent\" strings";
				error_code = "invalid_aux_presence_overrides";
				return false;
			}
			for (const auto& [aux_id, value] : json["aux_presence_overrides"].items())
			{
				if (!value.is_string() || (("present" != value.get_ref<const std::string&>()) && ("absent" != value.get_ref<const std::string&>())))
				{
					error = "aux_presence_overrides values must be \"present\" or \"absent\"";
					error_code = "invalid_aux_presence_overrides";
					return false;
				}
			}
			aux_presence_overrides = json["aux_presence_overrides"];
			return true;
		}

		bool ValidateShowAuxId(const nlohmann::json& json, bool& show_aux_id, std::string& error, std::string& error_code)
		{
			if (!json.contains("show_aux_id_in_label"))
			{
				return true;
			}
			if (!json["show_aux_id_in_label"].is_boolean())
			{
				error = "show_aux_id_in_label must be a boolean";
				error_code = "invalid_show_aux_id";
				return false;
			}
			show_aux_id = json["show_aux_id_in_label"].get<bool>();
			return true;
		}

		bool ValidateUi(const nlohmann::json& json, nlohmann::json& ui, std::string& error, std::string& error_code)
		{
			if (!json.contains("ui"))
			{
				return true;
			}
			if (!json["ui"].is_object())
			{
				error = "ui must be an object";
				error_code = "invalid_ui";
				return false;
			}
			// Shallow-merge at the top level so independent UI features (e.g.
			// ui.chemistryBands and ui.locale) can each PUT their own key
			// without clobbering the others; a null value deletes the key.
			if (!ui.is_object())
			{
				ui = nlohmann::json::object();
			}
			for (const auto& [key, value] : json["ui"].items())
			{
				if (value.is_null())
				{
					ui.erase(key);
				}
				else
				{
					ui[key] = value;
				}
			}
			return true;
		}

		bool ValidateSpaSwitchButtons(const nlohmann::json& json, nlohmann::json& spa_switch_buttons, std::string& error, std::string& error_code)
		{
			if (!json.contains("spa_switch_buttons"))
			{
				return true;
			}
			if (!json["spa_switch_buttons"].is_object())
			{
				error = "spa_switch_buttons must be an object of \"switch:button\"->function strings";
				error_code = "invalid_spa_switch_buttons";
				return false;
			}
			for (const auto& [key, function] : json["spa_switch_buttons"].items())
			{
				if (!function.is_string())
				{
					error = "spa_switch_buttons values must be strings";
					error_code = "invalid_spa_switch_buttons";
					return false;
				}
			}
			spa_switch_buttons = json["spa_switch_buttons"];
			return true;
		}
	}
	// unnamed namespace

	PreferencesService::PreferencesService(Kernel::HubLocator& hub_locator, const Options::Preferences::PreferencesSettings& settings) :
		m_Hub(hub_locator.Find<Kernel::PreferencesHub>()),
		m_Settings(settings)
	{
	}

	void PreferencesService::Seed(std::uint32_t salt_low_ppm, std::uint32_t comms_timeout_seconds, const std::string& webhook_url, std::uint32_t retention_days) const
	{
		if (!m_Hub) { return; }
		m_Hub->AlertSaltLowPpm = salt_low_ppm;
		m_Hub->AlertCommsTimeoutSeconds = comms_timeout_seconds;
		m_Hub->AlertWebhookUrl = webhook_url;
		m_Hub->HistoryRetentionDays = retention_days;
	}

	void PreferencesService::Start()
	{
		if (m_Settings.preferences_file.empty())
		{
			return;
		}
		Load();
	}

	nlohmann::json PreferencesService::ToJson() const
	{
		nlohmann::json json;
		if (!m_Hub) { return json; }

		json["temperature_units"] = std::string{ magic_enum::enum_name(m_Hub->Temperature_DisplayUnits) };
		json["alert"] = {
			{ "salt_low_ppm", m_Hub->AlertSaltLowPpm },
			{ "comms_timeout_seconds", m_Hub->AlertCommsTimeoutSeconds },
			{ "webhook_url", m_Hub->AlertWebhookUrl },
		};
		json["history"] = { { "retention_days", m_Hub->HistoryRetentionDays } };
		json["label_overrides"] = m_Hub->LabelOverrides;
		json["aux_presence_overrides"] = m_Hub->AuxPresenceOverrides;
		json["show_aux_id_in_label"] = m_Hub->ShowAuxIdInLabel;
		json["ui"] = m_Hub->UiPreferences;
		json["spa_switch_buttons"] = m_Hub->SpaSwitchButtons;
		return json;
	}

	bool PreferencesService::ApplyJson(const nlohmann::json& json, std::string& error)
	{
		std::string ignored_code;
		return ApplyJson(json, error, ignored_code);
	}

	bool PreferencesService::ApplyJson(const nlohmann::json& json, std::string& error, std::string& error_code)
	{
		if (!m_Hub)
		{
			error = "preferences hub unavailable";
			error_code = "prefs_hub_unavailable";
			return false;
		}
		if (!json.is_object())
		{
			error = "preferences must be a JSON object";
			error_code = "prefs_not_object";
			return false;
		}

		// Validate into locals first so a single bad field never half-applies.
		auto units = m_Hub->Temperature_DisplayUnits;
		auto salt = m_Hub->AlertSaltLowPpm;
		auto comms = m_Hub->AlertCommsTimeoutSeconds;
		auto webhook = m_Hub->AlertWebhookUrl;
		auto retention = m_Hub->HistoryRetentionDays;
		auto label_overrides = m_Hub->LabelOverrides;
		auto aux_presence_overrides = m_Hub->AuxPresenceOverrides;
		auto show_aux_id = m_Hub->ShowAuxIdInLabel;
		auto ui = m_Hub->UiPreferences;
		auto spa_switch_buttons = m_Hub->SpaSwitchButtons;

		if (!ValidateTemperatureUnits(json, units, error, error_code)) { return false; }
		if (!ValidateAlert(json, salt, comms, webhook, error, error_code)) { return false; }
		if (!ValidateHistory(json, retention, error, error_code)) { return false; }
		if (!ValidateLabelOverrides(json, label_overrides, error, error_code)) { return false; }
		if (!ValidateAuxPresenceOverrides(json, aux_presence_overrides, error, error_code)) { return false; }
		if (!ValidateShowAuxId(json, show_aux_id, error, error_code)) { return false; }
		if (!ValidateUi(json, ui, error, error_code)) { return false; }
		if (!ValidateSpaSwitchButtons(json, spa_switch_buttons, error, error_code)) { return false; }

		// Commit + persist.
		const bool units_changed = (m_Hub->Temperature_DisplayUnits != units);
		m_Hub->Temperature_DisplayUnits = units;
		m_Hub->AlertSaltLowPpm = salt;
		m_Hub->AlertCommsTimeoutSeconds = comms;
		m_Hub->AlertWebhookUrl = std::move(webhook);
		m_Hub->HistoryRetentionDays = retention;
		m_Hub->LabelOverrides = std::move(label_overrides);
		m_Hub->AuxPresenceOverrides = std::move(aux_presence_overrides);
		m_Hub->ShowAuxIdInLabel = show_aux_id;
		m_Hub->UiPreferences = std::move(ui);
		m_Hub->SpaSwitchButtons = std::move(spa_switch_buttons);

		Save();

		// After commit+persist so subscribers reading the hub see the new value.
		if (units_changed)
		{
			m_Hub->DisplayUnitsChangedSignal();
		}

		// NOTE: this only persists aux_presence_overrides -- it does NOT reconcile the live
		// device graph (that's a protocol-specific concern PreferencesService, being part of
		// libaqualink-automate, must not reach into; libaqualink-jandy depends on this library,
		// never the reverse). Reconciliation happens in JandyController's constructor (covers
		// boot-time restore) and explicitly in WebRoute_Equipment_AuxSlot (the dedicated,
		// UI-facing route) right after a successful apply. A direct PUT to /api/preferences that
		// bypasses that route persists the override but only takes visible effect at the next
		// panel-model (re)detection or process restart.

		return true;
	}

	void PreferencesService::RecordSpaSwitchAssignment(std::uint8_t switch_number, std::uint8_t button_number, const std::string& function)
	{
		if (!m_Hub)
		{
			return;
		}

		// Record the user's REQUESTED mapping (desired state) keyed "<switch>:<button>" and persist.
		// The controller's live decoded map (DataHub) remains the source of truth; this just lets the
		// UI reflect intent and survives a restart.
		const auto key = std::format("{}:{}", switch_number, button_number);
		m_Hub->SpaSwitchButtons[key] = function;
		Save();
	}

	void PreferencesService::Load()
	{
		if (!std::filesystem::exists(m_Settings.preferences_file))
		{
			return;
		}

		try
		{
			std::ifstream in(m_Settings.preferences_file, std::ios::binary);
			nlohmann::json json = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);

			std::string error;
			// Reuse the validating apply, but it would re-save; loading is read-only,
			// so suppress the write by temporarily clearing the path... simpler: apply
			// then accept the harmless re-write of identical content.
			if (!ApplyJson(json, error))
			{
				LogWarning(Channel::Main, [&error] { return std::format("Preferences file rejected ({}); using seeded/default values", error); });
			}
		}
		catch (const nlohmann::json::exception& ex)
		{
			// nlohmann::json::parse (malformed JSON) and the typed accessors reached
			// through ApplyJson are the only domain throwers on this path.
			LogError(Channel::Main, [&ex] { return std::format("Failed to load preferences file: {}", ex.what()); });
		}
	}

	void PreferencesService::Save() const
	{
		if (m_Settings.preferences_file.empty() || !m_Hub)
		{
			return;
		}

		try
		{
			const std::filesystem::path target(m_Settings.preferences_file);
			const std::filesystem::path tmp = target.string() + ".tmp";
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				out << ToJson().dump(2);
			}
			std::filesystem::rename(tmp, target);
		}
		catch (const std::filesystem::filesystem_error& ex)
		{
			// std::filesystem::rename fails (target locked, cross-device, missing dir).
			LogError(Channel::Main, [&ex] { return std::format("Failed to save preferences file: {}", ex.what()); });
		}
		catch (const nlohmann::json::exception& ex)
		{
			// ToJson().dump() rejects e.g. invalid UTF-8 in a stored string value.
			LogError(Channel::Main, [&ex] { return std::format("Failed to save preferences file: {}", ex.what()); });
		}
	}

}
// namespace AqualinkAutomate::Preferences
