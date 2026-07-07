#pragma once

#include <string>

#include "exceptions/exception_genericaqualinkexception.h"

namespace AqualinkAutomate::Exceptions
{

	class Developer_OptionKeyHasNoValue : public GenericAqualinkException
	{
		static const std::string DEVELOPER_OPTIONS_KEY_HAS_NO_VALUE_MESSAGE;

	public:
		Developer_OptionKeyHasNoValue();
		explicit Developer_OptionKeyHasNoValue(const std::string& message);
	};

}
// namespace AqualinkAutomate::Exceptions
