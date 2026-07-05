#include "factories/pentair_message_factory.h"
#include "messages/pentair_message_unknown.h"
#include "messages/chlorinator/pentair_chlorinator_message_setoutput.h"
#include "messages/chlorinator/pentair_chlorinator_message_status.h"
#include "messages/controller/pentair_controller_message_status.h"
#include "messages/pump/pentair_pump_message_power.h"
#include "messages/pump/pentair_pump_message_speed.h"
#include "messages/pump/pentair_pump_message_status.h"

namespace AqualinkAutomate::Pentair::Factory
{

	Types::PentairMessageTypePtr PentairMessageFactory::CreateMessageFromCommand(Messages::PentairMessageIds id) noexcept
	{
		using enum Messages::PentairMessageIds;

		switch (id)
		{
		// IntelliCenter / EasyTouch controller (B4).
		case Controller_Status:
			return std::make_shared<Messages::PentairControllerMessage_Status>();

		// VSP pump (IntelliFlo) commands (B2).
		case Pump_Status:
			return std::make_shared<Messages::PentairPumpMessage_Status>();

		case Pump_Speed:
			return std::make_shared<Messages::PentairPumpMessage_Speed>();

		case Pump_Power:
			return std::make_shared<Messages::PentairPumpMessage_Power>();

		// IntelliChlor salt-water generator (B3).
		case Chlorinator_Status:
			return std::make_shared<Messages::PentairChlorinatorMessage_Status>();

		case Chlorinator_SetOutput:
			return std::make_shared<Messages::PentairChlorinatorMessage_SetOutput>();

		case Unknown:
		default:
			return std::make_shared<Messages::PentairMessage_Unknown>();
		}
	}

}
// namespace AqualinkAutomate::Pentair::Factory
