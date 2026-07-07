#pragma once

#include <cstdint>

#include <boost/statechart/event.hpp>

namespace AqualinkAutomate::Utility::ScreenDataPageUpdaterImpl
{

	class evHighlight : public boost::statechart::event<evHighlight>
	{
	public:
		evHighlight(uint8_t line_id);

		uint8_t LineId() const;

	private:
		const uint8_t m_LineId;
	};

}
// namespace AqualinkAutomate::Utility::ScreenDataPageUpdaterImpl
