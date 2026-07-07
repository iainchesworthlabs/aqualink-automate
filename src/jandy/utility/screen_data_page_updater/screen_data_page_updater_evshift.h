#pragma once

#include <cstdint>
#include <utility>

#include <boost/statechart/event.hpp>

#include "utility/screen_data_page.h"

namespace AqualinkAutomate::Utility::ScreenDataPageUpdaterImpl
{

	class evShift : public boost::statechart::event<evShift>
	{
	public:
		evShift(ScreenDataPage::ShiftDirections direction, uint8_t first_line, uint8_t last_line, uint8_t number_of_shifts);

		ScreenDataPage::ShiftDirections Direction() const;
		uint8_t FirstLineId() const;
		uint8_t LastLineId() const;
		uint8_t NumberOfShifts() const;

	private:
		const ScreenDataPage::ShiftDirections m_Direction;
		const uint8_t m_FirstLineId;
		const uint8_t m_LastLineId;
		const uint8_t m_NumberOfShifts;
	};

}
// namespace AqualinkAutomate::Utility::ScreenDataPageUpdaterImpl
