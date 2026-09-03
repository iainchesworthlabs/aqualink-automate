#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include <boost/beast/http/verb.hpp>

#include "auth/entitlement_vocabulary.h"
#include "interfaces/icommanddispatcher.h"
#include "interfaces/iwebroute.h"
#include "kernel/data_hub.h"
#include "kernel/hub_locator.h"

namespace AqualinkAutomate::HTTP
{
	inline constexpr char EQUIPMENT_SETPOINTS_ROUTE_URL[] = "/api/equipment/setpoints";

	class WebRoute_Equipment_Setpoints : public Interfaces::IWebRoute<EQUIPMENT_SETPOINTS_ROUTE_URL>
	{
	public:
		explicit WebRoute_Equipment_Setpoints(Kernel::HubLocator& hub_locator);

		HTTP::Response OnRequest(const HTTP::Request& req) final;

		Interfaces::AccessRequirement RequiredAccess(boost::beast::http::verb method) const override
		{
			if ((boost::beast::http::verb::get == method) || (boost::beast::http::verb::head == method))
			{
				return { .Action = Auth::Vocabulary::EQUIPMENT_VIEW };
			}

			return { .Action = Auth::Vocabulary::EQUIPMENT_CONTROL_SETPOINTS };
		}

	private:
		HTTP::Response HandleGet(const HTTP::Request& req);
		HTTP::Response HandlePost(const HTTP::Request& req);

		// Converts a single Celsius setpoint field to a wire value and dispatches it.
		// Returns std::nullopt on success (or when the field is absent); otherwise the
		// bad-request response to send immediately. `overall_failure` is updated to the
		// FIRST non-Success CommandResult seen across the fields processed so far (pool,
		// then spa), which HandlePost maps to the response status via StatusForCommandResult
		// -- so a controller that is merely busy (transient, worth retrying) is distinguished
		// from one that genuinely cannot act, rather than every failure reading as a flat 500.
		std::optional<HTTP::Response> ConvertAndDispatchSetpoint(
			const HTTP::Request& req,
			const nlohmann::json& payload,
			const std::string& key,
			const std::function<Interfaces::ICommandDispatcher::CommandResult(uint8_t)>& dispatch_fn,
			nlohmann::json& result,
			Interfaces::ICommandDispatcher::CommandResult& overall_failure);

	private:
		std::shared_ptr<Kernel::DataHub> m_DataHub{ nullptr };
		std::shared_ptr<Interfaces::ICommandDispatcher> m_CommandDispatcher{ nullptr };
	};

}
// namespace AqualinkAutomate::HTTP
