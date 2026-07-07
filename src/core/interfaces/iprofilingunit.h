#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace AqualinkAutomate::Interfaces
{

	class IProfilingUnit
	{
	public:
		explicit IProfilingUnit(std::string_view name);
		virtual ~IProfilingUnit() = default;

		virtual void Start() const = 0;
		virtual void Mark() const = 0;
		virtual void End() const = 0;

		virtual void Text(std::string_view text) const;
		virtual void Value(uint64_t value) const;

		std::string_view Name() const;

	private:
		const std::string_view m_Name;
	};

}
// namespace AqualinkAutomate::Interfaces
