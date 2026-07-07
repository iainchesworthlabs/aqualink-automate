#pragma once

#include <string>

#include "exceptions/exception_optionparsingfailed.h"

namespace AqualinkAutomate::Exceptions
{

	class Options_MissingDependency : public OptionParsingFailed
	{
		static const std::string OPTION_MISSING_DEPENDENCY_MESSAGE;

	public:
		Options_MissingDependency();
		explicit Options_MissingDependency(const std::string& message);
	};

}
// namespace AqualinkAutomate::Exceptions
