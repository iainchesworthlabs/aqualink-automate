#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <optional>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/webroute_equipment_spaside_remotes.h"
#include "http/server/make_response.h"
#include "http/server/server_fields.h"
#include "logging/logging.h"
#include "preferences/preferences_service.h"
#include "profiling/factories/profiling_unit_factory.h"
#include "utility/to_number.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::HTTP
{

	// TryFind (not Find): a spa-side controller is only present when the Jandy stack is running.
	// In dev-mode/replay there is none, and the route should still construct and report an empty
	// list rather than throw.
	WebRoute_Equipment_SpasideRemotes::WebRoute_Equipment_SpasideRemotes(Kernel::HubLocator& hub_locator, std::shared_ptr<Preferences::PreferencesService> preferences_service) :
		m_Controller(hub_locator.TryFind<Interfaces::ISpasideRemoteController>()),
		m_DataHub(hub_locator.TryFind<Kernel::DataHub>()),
		m_PreferencesHub(hub_locator.TryFind<Kernel::PreferencesHub>()),
		m_PreferencesService(std::move(preferences_service))
	{
	}

	HTTP::Response WebRoute_Equipment_SpasideRemotes::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Equipment_SpasideRemotes::OnRequest", std::source_location::current());

		switch (req.method())
		{
		case Verbs::get:
			return HandleGet(req);

		case Verbs::post:
			return HandlePost(req);

		default:
			return MakeErrorResponse(req, HTTP::Status::method_not_allowed, "method_not_allowed", "Method not allowed. Use GET or POST.", {{"allowed", "GET, POST"}});
		}
	}

	HTTP::Response WebRoute_Equipment_SpasideRemotes::HandleGet(const HTTP::Request& req) const
	{
		// With no controller the empty (but well-formed) envelope is the correct picture -- no
		// spa-side stack is running (e.g. dev-mode/replay).
		return MakeJsonResponse(req, HTTP::Status::ok, BuildEnvelope().dump());
	}

	std::map<std::pair<std::uint8_t, std::uint8_t>, std::string> WebRoute_Equipment_SpasideRemotes::ParseRequestedAssignments() const
	{
		// Non-throwing parse: std::stoul throws std::out_of_range for an all-digit
		// string wider than unsigned long (the digit check above does NOT bound the
		// magnitude), and these keys are attacker-influenced (persisted via PUT
		// /api/preferences / the config file).  An uncaught throw here unwinds into
		// the router's exception barrier and degrades to a blanket HTTP 500; ToNumber
		// returns std::nullopt instead, so a malformed key is simply skipped below.
		auto parse_uint = [](const std::string& s)
		{
			return Utility::ToNumber<unsigned long>(s);
		};

		std::map<std::pair<uint8_t, uint8_t>, std::string> requested_map;
		if (m_PreferencesHub)
		{
			for (const auto& [key, function] : m_PreferencesHub->SpaSwitchButtons.items())
			{
				const auto colon = key.find(':');
				if (colon == std::string::npos || !function.is_string()) { continue; }
				const auto sw = parse_uint(key.substr(0, colon));
				const auto btn = parse_uint(key.substr(colon + 1));
				if (!sw.has_value() || !btn.has_value()) { continue; }
				if (sw.value() > 0xFF || btn.value() > 0xFF) { continue; }
				requested_map[{ static_cast<uint8_t>(sw.value()), static_cast<uint8_t>(btn.value()) }] = function.get<std::string>();
			}
		}

		return requested_map;
	}

	nlohmann::json WebRoute_Equipment_SpasideRemotes::BuildRemotesJson(
		const std::map<std::pair<std::uint8_t, std::uint8_t>, std::string>& live,
		const std::map<std::pair<std::uint8_t, std::uint8_t>, std::string>& requested_map) const
	{
		nlohmann::json remotes_json = nlohmann::json::array();
		if (m_Controller)
		{
			for (const auto& remote : m_Controller->Remotes())
			{
				nlohmann::json j;
				j["address"] = std::format("0x{:02x}", remote.address);
				j["device_class"] = remote.device_class;
				j["emulated"] = remote.emulated;
				j["button_count"] = remote.button_count;
				j["poll_count"] = remote.poll_count;
				j["last_button"] = remote.last_button;
				j["led_image_seen"] = remote.led_image_seen;
				j["leds"] = remote.leds;
				j["led_image"] = remote.led_image;

				nlohmann::json buttons = nlohmann::json::array();
				for (const auto& button : remote.buttons)
				{
					nlohmann::json b;
					b["index"] = button.index;
					b["switch"] = button.switch_number;
					b["button"] = button.button_number;
					b["assignable"] = button.assignable;
					b["pressable"] = remote.emulated;

					const std::pair<uint8_t, uint8_t> key{ button.switch_number, button.button_number };
					const auto live_it = button.assignable ? live.find(key) : live.end();
					const auto req_it = button.assignable ? requested_map.find(key) : requested_map.end();
					b["function"] = (live_it != live.end()) ? live_it->second : std::string{};
					b["requested"] = (req_it != requested_map.end()) ? req_it->second : std::string{};
					// Pending iff a request exists and the live map does not (yet) confirm the same function.
					b["pending"] = (req_it != requested_map.end()) &&
						((live_it == live.end()) || (live_it->second != req_it->second));

					buttons.push_back(std::move(b));
				}
				j["buttons"] = std::move(buttons);

				remotes_json.push_back(std::move(j));
			}
		}

		return remotes_json;
	}

	nlohmann::json WebRoute_Equipment_SpasideRemotes::BuildEnvelope() const
	{
		// --- Live decoded assignments (iAQ "Spa Remotes" / OneTouch "Spa Switch" config), keyed by
		// the controller's switch:button numbering. Used both for the flat `assignments` list and to
		// label each remote key with its real function.
		std::map<std::pair<uint8_t, uint8_t>, std::string> live;
		if (m_DataHub)
		{
			live = m_DataHub->SpaSwitchAssignments();
		}

		// --- User-requested (desired-state) assignments persisted in PreferencesHub, keyed
		// "<switch>:<button>". Surfaced so the UI shows intent even before the controller re-reports
		// it (or, on a read-only/iAQ-only system, as a pending annotation).
		const std::map<std::pair<uint8_t, uint8_t>, std::string> requested_map = ParseRequestedAssignments();

		// --- Remotes, each enriched with a per-key `buttons` list joining the wire press index, the
		// controller config switch:button coordinate, the live function, the requested function and a
		// pending flag (requested but not yet confirmed by the live map). `pressable` mirrors the
		// remote's emulated flag (only an emulated remote can have a press injected).
		nlohmann::json remotes_json = BuildRemotesJson(live, requested_map);

		nlohmann::json envelope;
		envelope["remotes"] = std::move(remotes_json);

		// Flat back-compat lists.
		nlohmann::json assignments = nlohmann::json::array();
		for (const auto& [key, function] : live)
		{
			nlohmann::json entry;
			entry["switch"] = key.first;
			entry["button"] = key.second;
			entry["function"] = function;
			assignments.push_back(std::move(entry));
		}
		envelope["assignments"] = std::move(assignments);

		nlohmann::json requested = nlohmann::json::array();
		for (const auto& [key, function] : requested_map)
		{
			nlohmann::json entry;
			entry["switch"] = key.first;
			entry["button"] = key.second;
			entry["function"] = function;
			requested.push_back(std::move(entry));
		}
		envelope["requested"] = std::move(requested);

		// --- Assignable functions: the controller's picker set unioned with any function already in
		// use (so a strict UI chooser can never hide a currently-assigned function). First-seen order
		// is preserved: controller order first, then any extra in-use functions.
		nlohmann::json available = nlohmann::json::array();
		std::vector<std::string> available_list;
		if (m_Controller)
		{
			available_list = m_Controller->AvailableFunctions();
		}
		for (const auto& [key, function] : live)
		{
			if (std::ranges::find(available_list, function) == available_list.end())
			{
				available_list.push_back(function);
			}
		}
		for (const auto& function : available_list)
		{
			available.push_back(function);
		}
		envelope["available_functions"] = std::move(available);

		return envelope;
	}

	HTTP::Response WebRoute_Equipment_SpasideRemotes::HandlePost(const HTTP::Request& req) const
	{
		if (!m_Controller)
		{
			LogWarning(Channel::Web, "Spa-side remote command requested but no controller is available (dev-mode/replay?)");
			return MakeErrorResponse(req, HTTP::Status::service_unavailable, "spaside_unavailable", "Spa-side remote control is not available in this mode");
		}

		try
		{
			auto body = nlohmann::json::parse(req.body());

			if (!body.contains("action") || !body["action"].is_string())
			{
				return MakeErrorResponse(req, HTTP::Status::bad_request, "spaside_action_required", "Request must contain a string 'action' ('press' or 'assign')");
			}

			const auto action = body["action"].get<std::string>();

			if ("press" == action)
			{
				return HandlePressAction(req, body);
			}

			if ("assign" == action)
			{
				return HandleAssignAction(req, body);
			}

			return MakeErrorResponse(req, HTTP::Status::bad_request, "spaside_invalid_action", "'action' must be 'press' or 'assign'");
		}
		catch (const nlohmann::json::exception&)
		{
			return MakeErrorResponse(req, HTTP::Status::bad_request, "invalid_json", "Invalid JSON in request body");
		}
	}

	HTTP::Response WebRoute_Equipment_SpasideRemotes::HandlePressAction(const HTTP::Request& req, const nlohmann::json& body) const
	{
		if (!body.contains("address") || !body["address"].is_number_unsigned())
		{
			return MakeErrorResponse(req, HTTP::Status::bad_request, "spaside_press_requires_address", "'press' requires an unsigned integer 'address'");
		}
		if (!body.contains("button") || !body["button"].is_number_unsigned())
		{
			return MakeErrorResponse(req, HTTP::Status::bad_request, "spaside_press_requires_button", "'press' requires an unsigned integer 'button'");
		}

		const auto address_value = body["address"].get<std::uint64_t>();
		const auto button_value = body["button"].get<std::uint64_t>();
		if ((address_value > 0xFF) || (button_value > 0xFF))
		{
			return MakeErrorResponse(req, HTTP::Status::bad_request, "spaside_byte_range", "'address' and 'button' must each fit in a single byte");
		}

		const auto address = static_cast<uint8_t>(address_value);
		const auto button = static_cast<uint8_t>(button_value);

		using enum Interfaces::ISpasideRemoteController::PressResult;
		switch (m_Controller->PressButton(address, button))
		{
		case Success:
			LogInfo(Channel::Web, std::format("Spa-side remote 0x{:02x}: queued press of button {} via web UI", address, button));
			return MakeJsonResponse(req, HTTP::Status::ok, BuildEnvelope().dump());

		case RemoteNotFound:
			return MakeErrorResponse(req, HTTP::Status::not_found, "spaside_remote_not_found", "No spa-side remote at that address");

		case NotEmulated:
			return MakeErrorResponse(req, HTTP::Status::conflict, "spaside_remote_observed_only", "That spa-side remote is a real device we only observe; it cannot be actuated");

		case InvalidButton:
		default:
			return MakeErrorResponse(req, HTTP::Status::bad_request, "spaside_button_out_of_range", "'button' is out of range for that remote (1..button_count)");
		}
	}

	HTTP::Response WebRoute_Equipment_SpasideRemotes::HandleAssignAction(const HTTP::Request& req, const nlohmann::json& body) const
	{
		// Program switch:button -> function over the bus (drives the controller's config menu).
		if (!body.contains("switch") || !body["switch"].is_number_unsigned() ||
			!body.contains("button") || !body["button"].is_number_unsigned() ||
			!body.contains("function") || !body["function"].is_string())
		{
			return MakeErrorResponse(req, HTTP::Status::bad_request, "spaside_assign_requires_fields", "'assign' requires unsigned 'switch', unsigned 'button', and string 'function'");
		}

		const auto switch_value = body["switch"].get<std::uint64_t>();
		const auto button_value = body["button"].get<std::uint64_t>();
		const auto function = body["function"].get<std::string>();
		if ((switch_value > 0xFF) || (button_value > 0xFF))
		{
			return MakeErrorResponse(req, HTTP::Status::bad_request, "spaside_byte_range", "'switch' and 'button' must each fit in a single byte");
		}

		const auto sw = static_cast<uint8_t>(switch_value);
		const auto btn = static_cast<uint8_t>(button_value);

		using enum Interfaces::ISpasideRemoteController::AssignResult;
		switch (m_Controller->SetButtonAssignment(sw, btn, function))
		{
		case Accepted:
			LogInfo(Channel::Web, std::format("Spa-switch assign: switch {} button {} -> '{}' queued via web UI", sw, btn, function));
			// Remember the user's request (desired state) so the UI reflects it and it
			// survives a restart; the controller's live decoded map remains authoritative.
			if (m_PreferencesService)
			{
				m_PreferencesService->RecordSpaSwitchAssignment(sw, btn, function);
			}
			return MakeJsonResponse(req, HTTP::Status::ok, BuildEnvelope().dump());

		case InvalidRequest:
			return MakeErrorResponse(req, HTTP::Status::bad_request, "spaside_assign_invalid", "Invalid switch/button/function for assignment");

		case Busy:
			return MakeErrorResponse(req, HTTP::Status::conflict, "spaside_controller_busy", "A controller operation is in progress; retry shortly");

		case NotAvailable:
		default:
			return MakeErrorResponse(req, HTTP::Status::service_unavailable, "spaside_no_programmer", "No controller can program spa-switch assignments on this system");
		}
	}

}
// namespace AqualinkAutomate::HTTP
