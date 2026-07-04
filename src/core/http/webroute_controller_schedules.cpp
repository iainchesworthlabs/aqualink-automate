#include <source_location>
#include <string>

#include <nlohmann/json.hpp>

#include "http/server/make_response.h"
#include "http/server/responses/response_405.h"
#include "http/server/responses/response_503.h"
#include "http/server/server_fields.h"
#include "http/server/server_types.h"
#include "http/webroute_controller_schedules.h"
#include "profiling/factories/profiling_unit_factory.h"
#include "scheduling/controller_schedule.h"

namespace AqualinkAutomate::HTTP
{

	WebRoute_ControllerSchedules::WebRoute_ControllerSchedules(std::shared_ptr<Scheduling::ControllerScheduleStore> store) :
		m_Store(std::move(store))
	{
	}

	HTTP::Response WebRoute_ControllerSchedules::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_ControllerSchedules::OnRequest", std::source_location::current());

		if (!m_Store)
		{
			return HTTP::Responses::Response_503(req);
		}

		switch (req.method())
		{
		case HTTP::Verbs::get:
		{
			nlohmann::json arr = nlohmann::json::array();
			for (const auto& schedule : m_Store->List())
			{
				arr.push_back(Scheduling::ToJson(schedule));
			}

			nlohmann::json body{
				{ "status", std::string{ Scheduling::ControllerScheduleStatusToString(m_Store->Status()) } },
				{ "active_group", m_Store->ActiveGroup() },
				{ "schedules", std::move(arr) },
			};
			return MakeJsonResponse(req, HTTP::Status::ok, body.dump());
		}

		default:
			return HTTP::Responses::Response_405(req);
		}
	}

}
// namespace AqualinkAutomate::HTTP
