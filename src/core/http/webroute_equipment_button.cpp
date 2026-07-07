#include <format>

#include <boost/url/parse.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <magic_enum/magic_enum.hpp>

#include "http/webroute_equipment_button.h"
#include "http/server/make_response.h"
#include "http/server/parse_query_string.h"
#include "http/server/server_fields.h"
#include "http/server/responses/response_405.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_helpers.h"
#include "interfaces/icommanddispatcher.h"
#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace
{
	/// Extract button_id from URL path (last segment of "/api/equipment/buttons/{button_id}").
	/// Falls back to query string parameter for backwards compatibility.
	std::optional<std::string> ExtractButtonId(const AqualinkAutomate::HTTP::Request& req)
	{
		// Try extracting from URL path first (e.g. /api/equipment/buttons/some-uuid)
		if (auto url_result = boost::urls::parse_origin_form(req.target()); url_result.has_value())
		{
			auto segments = url_result->segments();
			// Path: /api/equipment/buttons/{button_id} -> 4 segments
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

		// Fall back to query string
		if (auto qs = AqualinkAutomate::HTTP::ParseQueryString(req, "button_id"); qs.has_value())
		{
			return qs.value();
		}

		return std::nullopt;
	}
}

namespace AqualinkAutomate::HTTP
{

	WebRoute_Equipment_Button::WebRoute_Equipment_Button(Kernel::HubLocator& hub_locator) :
		m_DataHub(hub_locator.Find<Kernel::DataHub>()),
		m_CommandDispatcher(hub_locator.TryFind<Interfaces::ICommandDispatcher>())
	{
	}

	HTTP::Response WebRoute_Equipment_Button::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_EquipmentButton::OnRequest", std::source_location::current());

		switch (req.method())
		{
		case HTTP::Verbs::get:
			return ButtonIndividual_GetHandler(req);

		case HTTP::Verbs::post:
			return ButtonIndividual_PostHandler(req);

		default:
			return HTTP::Responses::Response_405(req);
		}
	}

	HTTP::Response WebRoute_Equipment_Button::ButtonIndividual_GetHandler(const HTTP::Request& req)
	{
		const std::string UNKNOWN_BUTTON_ID{ "Unknown Or Missing Button Id" };

		try
		{
			if (Kernel::PoolConfigurations::Unknown == m_DataHub->PoolConfiguration)
			{
				return Report_SystemIsInactive(req);
			}
			else if (auto button_id = ExtractButtonId(req); !button_id.has_value())
			{
				return Report_ButtonDoesntExist(req, UNKNOWN_BUTTON_ID);
			}
			else if (const auto device{ m_DataHub->Devices.FindById(boost::uuids::string_generator()(button_id.value())) }; nullptr == device)
			{
				// Invalid device pointer...return a bad status.
				return Report_ButtonDoesntExist(req, button_id.value());
			}
			else
			{
				nlohmann::json button;

				button["id"] = button_id.value();

				if (device->AuxillaryTraits.Has(Kernel::AuxillaryTraitsTypes::LabelTrait{}))
				{
					button["label"] = *(device->AuxillaryTraits[Kernel::AuxillaryTraitsTypes::LabelTrait{}]);
				}

				if (Kernel::AuxillaryTraitsTypes::HasStatus(device))
				{
					button["status"] = Kernel::AuxillaryTraitsTypes::ConvertStatusToString(device);
				}

				return MakeJsonResponse(req, HTTP::Status::ok, button.dump());
			}
		}
		catch (const std::runtime_error& ex_re)
		{
			// Raised if the UUID is malformed and cannot be parsed.
			return Report_ButtonDoesntExist(req, UNKNOWN_BUTTON_ID);
		}
	}

	HTTP::Response WebRoute_Equipment_Button::ButtonIndividual_PostHandler(const HTTP::Request& req)
	{
		const std::string UNKNOWN_BUTTON_ID{ "Unknown Or Missing Button Id" };

		if (Kernel::PoolConfigurations::Unknown == m_DataHub->PoolConfiguration)
		{
			return Report_SystemIsInactive(req);
		}
		else
		{
			try
			{
				if (auto button_id = ExtractButtonId(req); !button_id.has_value())
				{
					return Report_ButtonDoesntExist(req, UNKNOWN_BUTTON_ID);
				}
				else if (const auto button_device{ m_DataHub->Devices.FindById(boost::uuids::string_generator()(button_id.value())) }; nullptr == button_device)
				{
					return Report_ButtonDoesntExist(req, button_id.value());
				}
				else
				{
					if (!m_CommandDispatcher)
					{
						return Report_SystemIsInactive(req);
					}

					auto parsed_uuid = boost::uuids::string_generator()(button_id.value());
					auto result = m_CommandDispatcher->ToggleByUuid(parsed_uuid);

					return ButtonToggle_MapResultToResponse(req, button_id.value(), button_device, result);
				}
			}
			catch (const std::runtime_error& ex_re)
			{
				// The Boost UUID generator failed as the string was invalid / incorrectly formatted.
				return Report_ButtonDoesntExist(req, UNKNOWN_BUTTON_ID);
			}
		}
	}

	HTTP::Response WebRoute_Equipment_Button::ButtonToggle_MapResultToResponse(const HTTP::Request& req, const std::string& button_id, const std::shared_ptr<Kernel::AuxillaryDevice>& button_device, Interfaces::ICommandDispatcher::CommandResult result)
	{
		switch (result)
		{
		case Interfaces::ICommandDispatcher::CommandResult::Success:
		{
			nlohmann::json button;
			button["id"] = button_id;

			if (button_device->AuxillaryTraits.Has(Kernel::AuxillaryTraitsTypes::LabelTrait{}))
			{
				button["label"] = *(button_device->AuxillaryTraits[Kernel::AuxillaryTraitsTypes::LabelTrait{}]);
			}

			if (Kernel::AuxillaryTraitsTypes::HasStatus(button_device))
			{
				button["status"] = Kernel::AuxillaryTraitsTypes::ConvertStatusToString(button_device);
			}

			button["command"] = "toggled";

			return MakeJsonResponse(req, HTTP::Status::ok, button.dump());
		}

		case Interfaces::ICommandDispatcher::CommandResult::NoSerialAdapter:
			return Report_SystemIsInactive(req);

		case Interfaces::ICommandDispatcher::CommandResult::DeviceNotFound:
			return Report_ButtonDoesntExist(req, button_id);

		case Interfaces::ICommandDispatcher::CommandResult::UnknownEquipmentType:
		{
			LogWarning(Channel::Web, std::format("Cannot toggle device '{}': unknown equipment type", button_id));
			return MakeResponse(req, HTTP::Status::unprocessable_entity, ContentTypes::TEXT_PLAIN, "Device type cannot be mapped to a control command");
		}

		case Interfaces::ICommandDispatcher::CommandResult::InvalidValue:
		{
			LogWarning(Channel::Web, std::format("Cannot toggle device '{}': invalid value", button_id));
			return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "Invalid value for device command");
		}
		}

		// Fallback (should not be reached).
		return Report_SystemIsInactive(req);
	}

	HTTP::Response WebRoute_Equipment_Button::Report_ButtonDoesntExist(const HTTP::Request& req, const std::string& button_id)
	{
		LogInfo(Channel::Web, std::format("Received an invalid button id ('{}'); rejecting button request", button_id));

		auto resp = MakeResponse(req, HTTP::Status::not_found, ContentTypes::TEXT_PLAIN, std::format("'{}' is an invalid button id", button_id));
		resp.set(boost::beast::http::field::content_encoding, "none");

		return resp;
	}

	HTTP::Response WebRoute_Equipment_Button::Report_SystemIsInactive(const HTTP::Request& req)
	{
		LogInfo(Channel::Web, "Aqualink Automate has not yet initialised (PoolConfiguration == Unknown); rejecting button action request");

		auto resp = MakeResponse(req, HTTP::Status::service_unavailable, ContentTypes::TEXT_PLAIN, "Service is not initialised; cannot action button");
		resp.set(boost::beast::http::field::retry_after, ServerFields::RetryAfter());

		return resp;
	}

}
// namespace AqualinkAutomate::HTTP
