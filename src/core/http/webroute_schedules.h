#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::Scheduling
{
	class SchedulerService;
}

namespace AqualinkAutomate::HTTP
{
	inline constexpr char SCHEDULES_ROUTE_URL[] = "/api/schedules";
	inline constexpr char SCHEDULE_ITEM_ROUTE_URL[] = "/api/schedules/{uuid}";

	// Collection route: GET lists all schedules, POST creates one (server assigns
	// the uuid; 201 + entity). 400 on validation error, 503 when the scheduler is
	// disabled (service null), 405 otherwise.
	class WebRoute_Schedules : public Interfaces::IWebRoute<SCHEDULES_ROUTE_URL>
	{
	public:
		explicit WebRoute_Schedules(std::shared_ptr<Scheduling::SchedulerService> service);
		~WebRoute_Schedules() override = default;

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
		std::shared_ptr<Scheduling::SchedulerService> m_Service;
	};

	// Item route: GET/PUT/DELETE /api/schedules/{uuid}. 404 unknown uuid, 400 on
	// validation error, 503 when disabled, 405 otherwise.
	class WebRoute_Schedule : public Interfaces::IWebRoute<SCHEDULE_ITEM_ROUTE_URL>
	{
	public:
		explicit WebRoute_Schedule(std::shared_ptr<Scheduling::SchedulerService> service);
		~WebRoute_Schedule() override = default;

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
		std::shared_ptr<Scheduling::SchedulerService> m_Service;
	};

}
// namespace AqualinkAutomate::HTTP
