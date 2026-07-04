#include <optional>
#include <source_location>
#include <string>
#include <vector>

#include <boost/url/parse.hpp>
#include <nlohmann/json.hpp>

#include "http/server/make_response.h"
#include "http/server/responses/response_405.h"
#include "http/server/responses/response_503.h"
#include "http/server/server_fields.h"
#include "http/server/server_types.h"
#include "http/webroute_command_helpers.h"
#include "http/webroute_schedule_promote.h"
#include "interfaces/icommanddispatcher.h"
#include "profiling/factories/profiling_unit_factory.h"
#include "scheduling/controller_schedule.h"
#include "scheduling/promotion_constraints.h"
#include "scheduling/schedule.h"
#include "scheduling/scheduler_service.h"

namespace AqualinkAutomate::HTTP
{

	namespace
	{
		// The {uuid} in /api/schedules/{uuid}/promote is the segment before the trailing "promote".
		std::optional<std::string> UuidSegment(const HTTP::Request& req)
		{
			auto url = boost::urls::parse_origin_form(req.target());
			if (!url.has_value()) { return std::nullopt; }
			std::vector<std::string> segments(url->segments().begin(), url->segments().end());
			if (segments.size() < 2) { return std::nullopt; }
			const std::string uuid = segments[segments.size() - 2];
			if (uuid.empty()) { return std::nullopt; }
			return uuid;
		}
	}

	WebRoute_SchedulePromote::WebRoute_SchedulePromote(std::shared_ptr<Scheduling::SchedulerService> service,
		std::shared_ptr<Interfaces::ICommandDispatcher> dispatcher) :
		m_Service(std::move(service)),
		m_Dispatcher(std::move(dispatcher))
	{
	}

	HTTP::Response WebRoute_SchedulePromote::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_SchedulePromote::OnRequest", std::source_location::current());

		if (!m_Service)
		{
			return HTTP::Responses::Response_503(req);
		}
		if (req.method() != HTTP::Verbs::post)
		{
			return HTTP::Responses::Response_405(req);
		}
		if (auto err = RequireCommandDispatcher(req, m_Dispatcher); err.has_value())
		{
			return std::move(*err);
		}

		const auto uuid = UuidSegment(req);
		if (!uuid.has_value())
		{
			return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "missing schedule uuid");
		}

		const auto primary = m_Service->Get(*uuid);
		if (!primary.has_value())
		{
			return MakeResponse(req, HTTP::Status::not_found, ContentTypes::TEXT_PLAIN, "unknown schedule");
		}

		// Only a button on/off point can be half of a controller span.
		if (!Scheduling::IsControllerRepresentableAction(primary->action.type))
		{
			return MakeResponse(req, HTTP::Status::unprocessable_entity, ContentTypes::TEXT_PLAIN,
				"only button_on / button_off schedules can be promoted to the controller");
		}

		// Find the complementary edge: the opposite on/off action on the same target and days.
		const auto want = (primary->action.type == Scheduling::ActionType::ButtonOn)
			? Scheduling::ActionType::ButtonOff
			: Scheduling::ActionType::ButtonOn;

		const auto all = m_Service->List();
		const Scheduling::Schedule* complement = nullptr;
		for (const auto& candidate : all)
		{
			if (candidate.uuid == primary->uuid) { continue; }
			if (candidate.action.type == want
				&& candidate.action.target == primary->action.target
				&& candidate.days_of_week == primary->days_of_week)
			{
				complement = &candidate;
				break;
			}
		}
		if (nullptr == complement)
		{
			return MakeResponse(req, HTTP::Status::unprocessable_entity, ContentTypes::TEXT_PLAIN,
				"no complementary on/off schedule on the same target and days to form a span");
		}

		const bool primary_is_on = (primary->action.type == Scheduling::ActionType::ButtonOn);
		const Scheduling::Schedule& on_schedule = primary_is_on ? *primary : *complement;
		const Scheduling::Schedule& off_schedule = primary_is_on ? *complement : *primary;

		std::string error;
		const auto candidate = Scheduling::BuildPromotionCandidate(on_schedule, off_schedule, error);
		if (!candidate.has_value())
		{
			return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, error);
		}

		// Confirm the controller can represent the span; if not, explain why with blocker codes.
		if (const auto feasibility = Scheduling::CheckControllerCandidate(*candidate); !feasibility.promotable)
		{
			nlohmann::json blockers = nlohmann::json::array();
			for (const auto blocker : feasibility.blockers)
			{
				blockers.push_back(std::string{ Scheduling::PromotionBlockerToString(blocker) });
			}
			nlohmann::json body{
				{ "error", "the paired span is not representable on the controller" },
				{ "blockers", std::move(blockers) },
			};
			return MakeJsonResponse(req, HTTP::Status::bad_request, body.dump());
		}

		const auto result = m_Dispatcher->CreateControllerProgram(*candidate);
		nlohmann::json ack{
			{ "status", (result == Interfaces::ICommandDispatcher::CommandResult::Success) ? "queued" : "rejected" },
			{ "schedule", Scheduling::ToJson(*candidate) },
		};
		return MakeJsonResponse(req, StatusForCommandResult(result), ack.dump());
	}

}
// namespace AqualinkAutomate::HTTP
