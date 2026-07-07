#pragma once

#include <string_view>

namespace AqualinkAutomate::HTTP
{

	class ContentTypes
	{
	public:
		static constexpr std::string_view APPLICATION_JSON{ "application/json" };
		static constexpr std::string_view TEXT_HTML{ "text/html" };
		static constexpr std::string_view TEXT_PLAIN{ "text/plain" };
	};

	class ServerFields
	{
	public:
		static const std::string_view RetryAfter();
		static const std::string_view Server();
	};

}
// namespace AqualinkAutomate::HTTP
