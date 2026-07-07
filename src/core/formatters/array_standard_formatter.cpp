#include "formatters/array_standard_formatter.h"


namespace std
{

	std::ostream& operator<<(std::ostream& os, const std::array<uint8_t, 16>& obj)
	{
		os << std::format("{}", obj);
		return os;
	}

	std::ostream& operator<<(std::ostream& os, const std::span<uint8_t>& obj)
	{
		os << std::format("{}", obj);
		return os;
	}

	std::ostream& operator<<(std::ostream& os, const std::vector<uint8_t>& obj)
	{
		os << std::format("{}", obj);
		return os;
	}

}
// namespace std
