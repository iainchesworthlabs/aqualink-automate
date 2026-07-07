#include "utility/slot_connection_manager.h"

namespace AqualinkAutomate::Utility
{

	SlotConnectionManager::SlotConnectionManager()
	{
	}

	SlotConnectionManager::~SlotConnectionManager()
	{
		for (auto& elem : m_Connections)
		{
			if (std::holds_alternative<boost::signals2::connection>(elem))
			{
				std::get<boost::signals2::connection>(elem).disconnect();
			}
		}
	}

}
// namespace AqualinkAutomate::Utility
