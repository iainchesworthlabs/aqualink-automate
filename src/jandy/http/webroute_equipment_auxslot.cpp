#include "http/webroute_equipment_auxslot.h"

#include <format>
#include <optional>
#include <string>

#include <boost/url/parse.hpp>
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

#include "auxillaries/jandy_auxillary_id.h"
#include "auxillaries/jandy_auxillary_presence_override.h"
#include "http/json/json_equipment_auxslots.h"
#include "http/server/make_response.h"
#include "http/server/responses/response_405.h"
#include "logging/logging.h"
#include "preferences/preferences_service.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace
{
	// Extract aux_id from the URL path (last segment of "/api/equipment/aux-slots/{aux_id}"),
	// URL-decoded by boost.url so "Aux%20B1" round-trips to "Aux B1".
	std::optional<std::string> ExtractAuxId(const AqualinkAutomate::HTTP::Request& req)
	{
		if (auto url_result = boost::urls::parse_origin_form(req.target()); url_result.has_value())
		{
			auto segments = url_result->segments();
			if (segments.size() >= 4)
			{
				auto it = segments.end();
				--it;
				std::string last_segment(*it);
				if (!last_segment.empty())
				{
					return last_segment;
				}
			}
		}

		return std::nullopt;
	}
}
// anonymous namespace

namespace AqualinkAutomate::HTTP
{

	WebRoute_Equipment_AuxSlot::WebRoute_Equipment_AuxSlot(Kernel::HubLocator& hub_locator, std::shared_ptr<Preferences::PreferencesService> preferences_service) :
		m_DataHub(hub_locator.Find<Kernel::DataHub>()),
		m_PreferencesHub(hub_locator.TryFind<Kernel::PreferencesHub>()),
		m_PreferencesService(std::move(preferences_service))
	{
	}

	HTTP::Response WebRoute_Equipment_AuxSlot::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Equipment_AuxSlot::OnRequest", std::source_location::current());

		if (HTTP::Verbs::put != req.method())
		{
			return HTTP::Responses::Response_405(req);
		}

		return HandlePut(req);
	}

	HTTP::Response WebRoute_Equipment_AuxSlot::HandlePut(const HTTP::Request& req)
	{
		if (!m_PreferencesHub || !m_PreferencesService)
		{
			return MakeErrorResponse(req, HTTP::Status::service_unavailable, "prefs_hub_unavailable", "Preferences are not available in this mode");
		}

		const auto raw_aux_id = ExtractAuxId(req);
		const auto aux_id = raw_aux_id.has_value() ? Auxillaries::ParseAuxId(raw_aux_id.value()) : std::nullopt;
		if (!aux_id.has_value())
		{
			const auto id_text = raw_aux_id.value_or(std::string{});
			return MakeErrorResponse(req, HTTP::Status::not_found, "invalid_aux_id", std::format("'{}' is not a valid auxillary id", id_text), {{"aux_id", id_text}});
		}

		nlohmann::json body;
		try
		{
			body = nlohmann::json::parse(req.body());
		}
		catch (const nlohmann::json::exception&)
		{
			return MakeErrorResponse(req, HTTP::Status::bad_request, "invalid_json", "Invalid JSON in request body");
		}

		if (!body.contains("presence_override") || !body["presence_override"].is_string())
		{
			return MakeErrorResponse(req, HTTP::Status::bad_request, "presence_override_required", "Request must contain a string 'presence_override' of 'auto', 'present' or 'absent'");
		}

		const auto requested = body["presence_override"].get<std::string>();
		if (("auto" != requested) && ("present" != requested) && ("absent" != requested))
		{
			return MakeErrorResponse(req, HTTP::Status::bad_request, "invalid_presence_override", "'presence_override' must be 'auto', 'present' or 'absent'");
		}

		// Canonicalise the storage key via enum_name (not the raw URL text) so "Aux5"/"Aux 5"
		// and "AuxB1"/"Aux B1" all collapse onto the one key GetPresenceOverride/GenerateJson_
		// Equipment_AuxSlots look up.
		const std::string key{ magic_enum::enum_name(aux_id.value()) };

		nlohmann::json merged = m_PreferencesHub->AuxPresenceOverrides;
		if (!merged.is_object())
		{
			merged = nlohmann::json::object();
		}

		if ("auto" == requested)
		{
			merged.erase(key);
		}
		else
		{
			merged[key] = requested;
		}

		std::string error, error_code;
		if (!m_PreferencesService->ApplyJson({ { "aux_presence_overrides", merged } }, error, error_code))
		{
			LogWarning(Channel::Web, std::format("Rejected aux presence override for '{}': {}", key, error));
			return MakeErrorResponse(req, HTTP::Status::bad_request, error_code, error);
		}

		// PreferencesService (core) only persisted the override -- it cannot reconcile the device
		// graph itself (that's Jandy-protocol-specific). Do it here, in the same request, so the
		// response and every other channel (REST/WS/MQTT/HA) reflect the change immediately.
		Auxillaries::ApplyPresenceOverrides(m_DataHub->Devices, m_PreferencesHub->AuxPresenceOverrides);

		const nlohmann::json label_overrides = m_PreferencesHub->LabelOverrides;
		const bool show_aux_id = m_PreferencesHub->ShowAuxIdInLabel;
		const nlohmann::json presence_overrides = m_PreferencesHub->AuxPresenceOverrides;

		for (auto& slot : JSON::GenerateJson_Equipment_AuxSlots(m_DataHub, label_overrides, show_aux_id, presence_overrides))
		{
			if (slot.contains("aux_id") && (slot["aux_id"].get<std::string>() == key))
			{
				return MakeJsonResponse(req, HTTP::Status::ok, slot.dump());
			}
		}

		// Unreachable: every enum value produces exactly one row.
		return MakeErrorResponse(req, HTTP::Status::internal_server_error, "aux_slot_not_found", "Updated slot could not be located");
	}

}
// namespace AqualinkAutomate::HTTP
