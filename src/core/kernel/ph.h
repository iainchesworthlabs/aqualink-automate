#pragma once

#include <boost/cstdfloat.hpp>

namespace AqualinkAutomate::Kernel
{

	class pH
	{
		static constexpr boost::float32_t MINIMUM_PH_VALUE = 0.0f;
		static constexpr boost::float32_t MAXIMUM_PH_VALUE = 14.0f;

	public:
		pH(boost::float32_t value);  // NOSONAR(cpp:S1709) — implicit numeric conversion is part of this value-wrapper's contract (see operator= on float32_t)

		boost::float32_t operator()() const;

		pH& operator=(boost::float32_t value);

		bool operator==(const pH& other) const;

	private:
		static boost::float32_t ClampAndRound(boost::float32_t value);

	private:
		boost::float32_t m_pH;
	};

}
// namespace AqualinkAutomate::Kernel
