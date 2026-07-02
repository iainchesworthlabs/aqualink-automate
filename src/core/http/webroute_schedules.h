#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::Scheduling { class SchedulerService; }
namespace AqualinkAutomate::Kernel { class DataHub; }

namespace AqualinkAutomate::HTTP
{
	inline constexpr char SCHEDULES_ROUTE_URL[] = "/api/schedules";
	inline constexpr char SCHEDULE_ITEM_ROUTE_URL[] = "/api/schedules/{uuid}";

	// Collection route: GET lists all schedules, POST creates one (server assigns
	// the uuid; 201 + entity). 400 on validation error, 403 when the saving
	// subject is not entitled to the schedule's action (D14), 503 when the
	// scheduler is disabled (service null), 405 otherwise.
	//
	// data_hub (nullable) resolves button-action target labels to device ids for
	// the action-gating check; null (auth off) skips the resolution.
	class WebRoute_Schedules : public Interfaces::IWebRoute<SCHEDULES_ROUTE_URL>
	{
	public:
		WebRoute_Schedules(std::shared_ptr<Scheduling::SchedulerService> service, std::shared_ptr<Kernel::DataHub> data_hub = {});
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
		std::shared_ptr<Kernel::DataHub> m_DataHub;
	};

	// Item route: GET/PUT/DELETE /api/schedules/{uuid}. 404 unknown uuid, 400 on
	// validation error, 403 on an un-entitled action (D14), 503 when disabled,
	// 405 otherwise.
	class WebRoute_Schedule : public Interfaces::IWebRoute<SCHEDULE_ITEM_ROUTE_URL>
	{
	public:
		WebRoute_Schedule(std::shared_ptr<Scheduling::SchedulerService> service, std::shared_ptr<Kernel::DataHub> data_hub = {});
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
		std::shared_ptr<Kernel::DataHub> m_DataHub;
	};

}
// namespace AqualinkAutomate::HTTP
