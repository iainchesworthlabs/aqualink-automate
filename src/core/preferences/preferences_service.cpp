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
	}
	// unnamed namespace

	PreferencesService::PreferencesService(Kernel::HubLocator& hub_locator, const Options::Preferences::PreferencesSettings& settings) :
		m_Settings(settings)
	{
		m_Hub = hub_locator.Find<Kernel::PreferencesHub>();
	}

	void PreferencesService::Seed(std::uint32_t salt_low_ppm, std::uint32_t comms_timeout_seconds, const std::string& webhook_url, std::uint32_t retention_days)
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
		json["show_aux_id_in_label"] = m_Hub->ShowAuxIdInLabel;
		json["ui"] = m_Hub->UiPreferences;
		json["spa_switch_buttons"] = m_Hub->SpaSwitchButtons;
		return json;
	}

	bool PreferencesService::ApplyJson(const nlohmann::json& json, std::string& error)
	{
		if (!m_Hub)
		{
			error = "preferences hub unavailable";
			return false;
		}
		if (!json.is_object())
		{
			error = "preferences must be a JSON object";
			return false;
		}

		// Validate into locals first so a single bad field never half-applies.
		auto units = m_Hub->Temperature_DisplayUnits;
		auto salt = m_Hub->AlertSaltLowPpm;
		auto comms = m_Hub->AlertCommsTimeoutSeconds;
		auto webhook = m_Hub->AlertWebhookUrl;
		auto retention = m_Hub->HistoryRetentionDays;
		auto label_overrides = m_Hub->LabelOverrides;
		auto show_aux_id = m_Hub->ShowAuxIdInLabel;
		auto ui = m_Hub->UiPreferences;
		auto spa_switch_buttons = m_Hub->SpaSwitchButtons;

		if (json.contains("temperature_units"))
		{
			if (!json["temperature_units"].is_string())
			{
				error = "temperature_units must be 'Celsius' or 'Fahrenheit'";
				return false;
			}
			auto parsed = magic_enum::enum_cast<Kernel::TemperatureUnits>(json["temperature_units"].get<std::string>());
			if (!parsed.has_value())
			{
				error = "temperature_units must be 'Celsius' or 'Fahrenheit'";
				return false;
			}
			units = parsed.value();
		}

		if (json.contains("alert") && json["alert"].is_object())
		{
			const auto& a = json["alert"];
			if (a.contains("salt_low_ppm"))
			{
				if (!a["salt_low_ppm"].is_number_integer() || a["salt_low_ppm"].get<std::int64_t>() < 0 || a["salt_low_ppm"].get<std::int64_t>() > 6000)
				{
					error = "alert.salt_low_ppm must be 0..6000";
					return false;
				}
				salt = a["salt_low_ppm"].get<std::uint32_t>();
			}
			if (a.contains("comms_timeout_seconds"))
			{
				if (!a["comms_timeout_seconds"].is_number_integer() || a["comms_timeout_seconds"].get<std::int64_t>() <= 0)
				{
					error = "alert.comms_timeout_seconds must be greater than 0";
					return false;
				}
				comms = a["comms_timeout_seconds"].get<std::uint32_t>();
			}
			if (a.contains("webhook_url"))
			{
				if (!a["webhook_url"].is_string())
				{
					error = "alert.webhook_url must be empty or an absolute http/https URL";
					return false;
				}
				const auto url = a["webhook_url"].get<std::string>();
				if (!url.empty() && !IsValidWebhookUrl(url))
				{
					error = "alert.webhook_url must be empty or an absolute http/https URL";
					return false;
				}
				webhook = url;
			}
		}

		if (json.contains("history") && json["history"].is_object())
		{
			const auto& h = json["history"];
			if (h.contains("retention_days"))
			{
				if (!h["retention_days"].is_number_integer() || h["retention_days"].get<std::int64_t>() <= 0)
				{
					error = "history.retention_days must be greater than 0";
					return false;
				}
				retention = h["retention_days"].get<std::uint32_t>();
			}
		}

		if (json.contains("label_overrides"))
		{
			if (!json["label_overrides"].is_object())
			{
				error = "label_overrides must be an object of canonical->display strings";
				return false;
			}
			for (const auto& [canonical, display] : json["label_overrides"].items())
			{
				if (!display.is_string())
				{
					error = "label_overrides values must be strings";
					return false;
				}
			}
			label_overrides = json["label_overrides"];
		}

		if (json.contains("show_aux_id_in_label"))
		{
			if (!json["show_aux_id_in_label"].is_boolean())
			{
				error = "show_aux_id_in_label must be a boolean";
				return false;
			}
			show_aux_id = json["show_aux_id_in_label"].get<bool>();
		}

		if (json.contains("ui"))
		{
			if (!json["ui"].is_object())
			{
				error = "ui must be an object";
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
		}

		if (json.contains("spa_switch_buttons"))
		{
			if (!json["spa_switch_buttons"].is_object())
			{
				error = "spa_switch_buttons must be an object of \"switch:button\"->function strings";
				return false;
			}
			for (const auto& [key, function] : json["spa_switch_buttons"].items())
			{
				if (!function.is_string())
				{
					error = "spa_switch_buttons values must be strings";
					return false;
				}
			}
			spa_switch_buttons = json["spa_switch_buttons"];
		}

		// Commit + persist.
		m_Hub->Temperature_DisplayUnits = units;
		m_Hub->AlertSaltLowPpm = salt;
		m_Hub->AlertCommsTimeoutSeconds = comms;
		m_Hub->AlertWebhookUrl = std::move(webhook);
		m_Hub->HistoryRetentionDays = retention;
		m_Hub->LabelOverrides = std::move(label_overrides);
		m_Hub->ShowAuxIdInLabel = show_aux_id;
		m_Hub->UiPreferences = std::move(ui);
		m_Hub->SpaSwitchButtons = std::move(spa_switch_buttons);

		Save();
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
				LogWarning(Channel::Main, [&] { return std::format("Preferences file rejected ({}); using seeded/default values", error); });
			}
		}
		catch (const std::exception& ex)
		{
			LogError(Channel::Main, [&] { return std::format("Failed to load preferences file: {}", ex.what()); });
		}
	}

	void PreferencesService::Save()
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
		catch (const std::exception& ex)
		{
			LogError(Channel::Main, [&] { return std::format("Failed to save preferences file: {}", ex.what()); });
		}
	}

}
// namespace AqualinkAutomate::Preferences
