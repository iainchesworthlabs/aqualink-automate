#pragma once

#include <cstdint>

#include <boost/statechart/event.hpp>

namespace AqualinkAutomate::Utility::ScreenDataPageUpdaterImpl
{

	class evHighlightChars : public boost::statechart::event<evHighlightChars>
	{
	public:
		evHighlightChars(uint8_t line_id, uint8_t start_index, uint8_t stop_index);

		uint8_t LineId() const;
		uint8_t StartIndex() const;
		uint8_t StopIndex() const;

	private:
		const uint8_t m_LineId;
		const uint8_t m_StartIndex;
		const uint8_t m_StopIndex;
	};

}
// namespace AqualinkAutomate::Utility::ScreenDataPageUpdaterImpl
