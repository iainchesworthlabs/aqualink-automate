#pragma once

#include <string>

#include "exceptions/exception_genericaqualinkexception.h"

namespace AqualinkAutomate::Exceptions
{

	class Developer_OptionKeyInvalidType : public GenericAqualinkException
	{
		static const std::string DEVELOPER_OPTIONS_KEY_INVALID_TYPE_MESSAGE;

	public:
		Developer_OptionKeyInvalidType();
		explicit Developer_OptionKeyInvalidType(const std::string& message);
	};

}
// namespace AqualinkAutomate::Exceptions
