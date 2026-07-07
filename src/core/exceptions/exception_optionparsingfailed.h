#pragma once

#include <string>

#include "exceptions/exception_genericaqualinkexception.h"

namespace AqualinkAutomate::Exceptions
{

	class OptionParsingFailed : public GenericAqualinkException
	{
		static const std::string OPTION_PARSING_FAILED_MESSAGE;

	public:
		OptionParsingFailed();
		explicit OptionParsingFailed(const std::string& message);
	};

}
// namespace AqualinkAutomate::Exceptions
