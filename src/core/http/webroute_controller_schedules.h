#pragma once

#include <memory>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/iwebroute.h"

namespace AqualinkAutomate::Scheduling
{
	class ControllerScheduleStore;
}

namespace AqualinkAutomate::HTTP
{
	inline constexpr char CONTROLLER_SCHEDULES_ROUTE_URL[] = "/api/controller/schedules";

	// Read-only view of the controller's internal schedules (its built-in
	// timers/programs). GET returns { "status": <source status>, "schedules": [...] }.
	// 405 for any non-GET verb. Writing controller schedules is gated on
	// reverse-engineering the OneTouch/IAQ edit flow and is intentionally absent.
	class WebRoute_ControllerSchedules : public Interfaces::IWebRoute<CONTROLLER_SCHEDULES_ROUTE_URL>
	{
	public:
		explicit WebRoute_ControllerSchedules(std::shared_ptr<Scheduling::ControllerScheduleStore> store);
		~WebRoute_ControllerSchedules() override = default;

	public:
		HTTP::Response OnRequest(const HTTP::Request& req) final;

	public:
		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb) const override
		{
			return { .Action = Auth::Vocabulary::SCHEDULES_VIEW };
		}

	private:
		std::shared_ptr<Scheduling::ControllerScheduleStore> m_Store;
	};

}
// namespace AqualinkAutomate::HTTP
