#pragma once

#include <memory>
#include <source_location>

#include "interfaces/iprofilingunit.h"
#include "profiling/profiling_units/unit_colours.h"

namespace AqualinkAutomate::Profiling
{

	class Frame : public Interfaces::IProfilingUnit
	{
	public:
		explicit Frame(std::string_view name, const std::source_location& src_loc = std::source_location::current(), UnitColours colour = UnitColours::NotSpecified);
		~Frame() override = default;

		void Start() const override;
		void Mark() const override;
		void End() const override;
	};

	using FramePtr = std::unique_ptr<Profiling::Frame>;

}
// namespace AqualinkAutomate::Profiling
