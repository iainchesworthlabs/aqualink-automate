#include "exceptions/exception_notimplemented.h"
#include "interfaces/istatus.h"

namespace AqualinkAutomate::Interfaces
{

	void to_json(nlohmann::json& json_object, const IStatus& status)
	{
		static constexpr std::string_view SOURCE_NAME{ "source_name" };
		static constexpr std::string_view SOURCE_TYPE{ "source_type" };
		static constexpr std::string_view STATUS_TYPE{ "status_type" };

		json_object = nlohmann::json
		{
			{ SOURCE_NAME, status.SourceName()},
			{ SOURCE_TYPE, status.SourceType()},
			{ STATUS_TYPE, status.StatusType() }
		};
	}

	void from_json(const nlohmann::json& /*json_object*/, IStatus& /*status*/)
	{
		throw Exceptions::NotImplemented();
	}

}
// namespace AqualinkAutomate::Interfaces
