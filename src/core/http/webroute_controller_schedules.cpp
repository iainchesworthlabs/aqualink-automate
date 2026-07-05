#include <optional>
#include <source_location>
#include <string>

#include <boost/url/parse.hpp>
#include <nlohmann/json.hpp>

#include "http/server/make_response.h"
#include "http/server/responses/response_405.h"
#include "http/server/responses/response_503.h"
#include "http/server/server_fields.h"
#include "http/server/server_types.h"
#include "http/webroute_command_helpers.h"
#include "http/webroute_controller_schedules.h"
#include "interfaces/icommanddispatcher.h"
#include "profiling/factories/profiling_unit_factory.h"
#include "scheduling/controller_schedule.h"
#include "scheduling/promotion_constraints.h"

namespace AqualinkAutomate::HTTP
{

	namespace
	{
		std::optional<std::string> LastPathSegment(const HTTP::Request& req)
		{
			auto url = boost::urls::parse_origin_form(req.target());
			if (!url.has_value()) { return std::nullopt; }
			auto segments = url->segments();
			if (segments.empty()) { return std::nullopt; }
			auto it = segments.end();
			--it;
			std::string last(*it);
			if (last.empty()) { return std::nullopt; }
			return last;
		}

		// A 400 body listing why a candidate cannot be represented on the controller.
		HTTP::Response NotRepresentableResponse(const HTTP::Request& req, const Scheduling::PromotionResult& feasibility)
		{
			nlohmann::json blockers = nlohmann::json::array();
			for (const auto blocker : feasibility.blockers)
			{
				blockers.push_back(std::string{ Scheduling::PromotionBlockerToString(blocker) });
			}
			nlohmann::json body{
				{ "error", "program is not representable on the controller" },
				{ "blockers", std::move(blockers) },
			};
			return MakeJsonResponse(req, HTTP::Status::bad_request, body.dump());
		}
	}

	//-------------------------------------------------------------------------
	// Collection: GET list + POST create
	//-------------------------------------------------------------------------
	WebRoute_ControllerSchedules::WebRoute_ControllerSchedules(std::shared_ptr<Scheduling::ControllerScheduleStore> store,
		std::shared_ptr<Interfaces::ICommandDispatcher> dispatcher) :
		m_Store(std::move(store)),
		m_Dispatcher(std::move(dispatcher))
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

		case HTTP::Verbs::post:
		{
			if (auto err = RequireCommandDispatcher(req, m_Dispatcher); err.has_value())
			{
				return std::move(*err);
			}

			nlohmann::json body;
			if (auto err = ParseJsonObjectBody(req, body); err.has_value())
			{
				return std::move(*err);
			}

			std::string parse_error;
			auto schedule = Scheduling::ControllerScheduleFromJson(body, parse_error);
			if (!schedule.has_value())
			{
				return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, parse_error);
			}

			// Reject up front with the specific blocker codes (rather than a generic 400 from the
			// device) so the UI can explain exactly why a program cannot be promoted/created.
			if (const auto feasibility = Scheduling::CheckControllerCandidate(*schedule); !feasibility.promotable)
			{
				return NotRepresentableResponse(req, feasibility);
			}

			// The write is asynchronous (queued as a page-navigation goal); Success == accepted.
			const auto result = m_Dispatcher->CreateControllerProgram(*schedule);
			nlohmann::json ack{
				{ "status", (result == Interfaces::ICommandDispatcher::CommandResult::Success) ? "queued" : "rejected" },
				{ "schedule", Scheduling::ToJson(*schedule) },
			};
			return MakeJsonResponse(req, StatusForCommandResult(result), ack.dump());
		}

		default:
			return HTTP::Responses::Response_405(req);
		}
	}

	//-------------------------------------------------------------------------
	// Item: DELETE /api/controller/schedules/{id}
	//-------------------------------------------------------------------------
	WebRoute_ControllerSchedule::WebRoute_ControllerSchedule(std::shared_ptr<Scheduling::ControllerScheduleStore> store,
		std::shared_ptr<Interfaces::ICommandDispatcher> dispatcher) :
		m_Store(std::move(store)),
		m_Dispatcher(std::move(dispatcher))
	{
	}

	HTTP::Response WebRoute_ControllerSchedule::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_ControllerSchedule::OnRequest", std::source_location::current());

		if (!m_Store)
		{
			return HTTP::Responses::Response_503(req);
		}

		const auto id = LastPathSegment(req);
		if (!id.has_value())
		{
			return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, "missing program id");
		}

		// Both DELETE and PUT act on an existing program, resolved by id against the read snapshot.
		if ((HTTP::Verbs::delete_ != req.method()) && (HTTP::Verbs::put != req.method()))
		{
			return HTTP::Responses::Response_405(req);
		}

		if (auto err = RequireCommandDispatcher(req, m_Dispatcher); err.has_value())
		{
			return std::move(*err);
		}

		// Resolve the id against the current read snapshot to get the full existing program.
		const Scheduling::ControllerSchedule* found = nullptr;
		for (const auto& schedule : m_Store->List())
		{
			if (schedule.id == *id) { found = &schedule; break; }
		}
		if (nullptr == found)
		{
			return MakeResponse(req, HTTP::Status::not_found, ContentTypes::TEXT_PLAIN, "unknown controller program");
		}

		if (HTTP::Verbs::delete_ == req.method())
		{
			const auto result = m_Dispatcher->DeleteControllerProgram(*found);
			nlohmann::json ack{ { "status", (result == Interfaces::ICommandDispatcher::CommandResult::Success) ? "queued" : "rejected" } };
			return MakeJsonResponse(req, StatusForCommandResult(result), ack.dump());
		}

		// PUT: parse the desired program from the body, gate it on controller feasibility, then edit
		// the resolved existing program in place. Copy the existing program (the store snapshot is
		// stable within this request, but the dispatch is by value).
		const Scheduling::ControllerSchedule existing = *found;

		nlohmann::json body;
		if (auto err = ParseJsonObjectBody(req, body); err.has_value())
		{
			return std::move(*err);
		}

		std::string parse_error;
		auto desired = Scheduling::ControllerScheduleFromJson(body, parse_error);
		if (!desired.has_value())
		{
			return MakeResponse(req, HTTP::Status::bad_request, ContentTypes::TEXT_PLAIN, parse_error);
		}

		if (const auto feasibility = Scheduling::CheckControllerCandidate(*desired); !feasibility.promotable)
		{
			return NotRepresentableResponse(req, feasibility);
		}

		const auto result = m_Dispatcher->EditControllerProgram(existing, *desired);
		nlohmann::json ack{
			{ "status", (result == Interfaces::ICommandDispatcher::CommandResult::Success) ? "queued" : "rejected" },
			{ "schedule", Scheduling::ToJson(*desired) },
		};
		return MakeJsonResponse(req, StatusForCommandResult(result), ack.dump());
	}

}
// namespace AqualinkAutomate::HTTP
