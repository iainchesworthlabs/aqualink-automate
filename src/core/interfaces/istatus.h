#pragma once

#include <concepts>
#include <memory>
#include <string_view>
#include <type_traits>

#include <boost/signals2.hpp>
#include <nlohmann/json.hpp>

namespace AqualinkAutomate::Interfaces
{

	class IStatus
	{
	public:
		IStatus() = default;
		IStatus(const IStatus&) = default;
		IStatus(IStatus&& other) = default;
		virtual ~IStatus() = default;

		IStatus& operator=(const IStatus&) = default;
		IStatus& operator=(IStatus&& other) = default;

		virtual std::string_view SourceName() const = 0;
		virtual std::string_view SourceType() const = 0;
		virtual std::string_view StatusType() const = 0;
	};

	//---------------------------------------------------------------------
	// JSON CONVERSION
	//---------------------------------------------------------------------

	void to_json(nlohmann::json& json_object, const IStatus& status);
	void from_json(const nlohmann::json& json_object, IStatus& status);

}
// namespace AqualinkAutomate::Interfaces
