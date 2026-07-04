#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::Scheduling
{
	class ControllerScheduleStore;
}

namespace AqualinkAutomate::Interfaces
{
	class ICommandDispatcher;
}

namespace AqualinkAutomate::HTTP
{
	inline constexpr char CONTROLLER_SCHEDULES_ROUTE_URL[] = "/api/controller/schedules";
	inline constexpr char CONTROLLER_SCHEDULE_ITEM_ROUTE_URL[] = "/api/controller/schedules/{id}";

	// Collection route for the controller's internal schedules (its built-in timers/programs).
	// GET returns { "status", "active_group", "schedules": [...] } from the read store. POST creates
	// a program on the controller by driving a capable panel's Program menu (async): the body is a
	// controller-schedule ({ target, days_of_week, on_local, off_local, name?, group? }); on success
	// (a write queued) it answers 200 and the caller polls GET for the result. 400 on a bad body,
	// 503 when no writer/dispatcher is present, 405 for other verbs.
	class WebRoute_ControllerSchedules : public Interfaces::IWebRoute<CONTROLLER_SCHEDULES_ROUTE_URL>
	{
	public:
		WebRoute_ControllerSchedules(std::shared_ptr<Scheduling::ControllerScheduleStore> store,
			std::shared_ptr<Interfaces::ICommandDispatcher> dispatcher = {});
		~WebRoute_ControllerSchedules() override = default;

	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	public:
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb method) const override
		{
			if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
			{
				return { .Action = Auth::Vocabulary::SCHEDULES_VIEW };
			}
			return { .Action = Auth::Vocabulary::SCHEDULES_EDIT };
		}

	private:
		std::shared_ptr<Scheduling::ControllerScheduleStore> m_Store;
		std::shared_ptr<Interfaces::ICommandDispatcher> m_Dispatcher;
	};

	// Item route: DELETE /api/controller/schedules/{id} removes the program whose id matches a
	// currently-listed controller schedule (resolved from the read store, then dispatched to a
	// ControllerScheduleWriter). 404 unknown id, 503 no dispatcher, 405 for other verbs.
	class WebRoute_ControllerSchedule : public Interfaces::IWebRoute<CONTROLLER_SCHEDULE_ITEM_ROUTE_URL>
	{
	public:
		WebRoute_ControllerSchedule(std::shared_ptr<Scheduling::ControllerScheduleStore> store,
			std::shared_ptr<Interfaces::ICommandDispatcher> dispatcher = {});
		~WebRoute_ControllerSchedule() override = default;

	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	public:
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb method) const override
		{
			if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
			{
				return { .Action = Auth::Vocabulary::SCHEDULES_VIEW };
			}
			return { .Action = Auth::Vocabulary::SCHEDULES_EDIT };
		}

	private:
		std::shared_ptr<Scheduling::ControllerScheduleStore> m_Store;
		std::shared_ptr<Interfaces::ICommandDispatcher> m_Dispatcher;
	};

}
// namespace AqualinkAutomate::HTTP
