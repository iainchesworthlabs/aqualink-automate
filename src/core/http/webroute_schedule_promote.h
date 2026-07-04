#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::Scheduling { class SchedulerService; }
namespace AqualinkAutomate::Interfaces { class ICommandDispatcher; }

namespace AqualinkAutomate::HTTP
{
	inline constexpr char SCHEDULE_PROMOTE_ROUTE_URL[] = "/api/schedules/{uuid}/promote";

	// POST /api/schedules/{uuid}/promote -- promote an app schedule to the controller. The named app
	// schedule (a button_on or button_off point) is paired with its complement (the matching
	// button_off/on on the same target + days) to form an on->off SPAN, which is checked against the
	// controller's constraints and, if representable, created on the controller (async, via the
	// ControllerScheduleWriter). 404 unknown uuid, 422 no complement to form a span, 400 not
	// representable (with blocker codes), 503 no scheduler/dispatcher. Requires schedules.edit.
	class WebRoute_SchedulePromote : public Interfaces::IWebRoute<SCHEDULE_PROMOTE_ROUTE_URL>
	{
	public:
		WebRoute_SchedulePromote(std::shared_ptr<Scheduling::SchedulerService> service,
			std::shared_ptr<Interfaces::ICommandDispatcher> dispatcher = {});
		~WebRoute_SchedulePromote() override = default;

	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	public:
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::SCHEDULES_EDIT };
		}

	private:
		std::shared_ptr<Scheduling::SchedulerService> m_Service;
		std::shared_ptr<Interfaces::ICommandDispatcher> m_Dispatcher;
	};

}
// namespace AqualinkAutomate::HTTP
