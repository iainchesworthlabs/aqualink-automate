#pragma once

#include <string>

#include "exceptions/exception_genericaqualinkexception.h"

namespace AqualinkAutomate::Exceptions
{

	class Developer_OptionKeyNotProvided : public GenericAqualinkException
	{
		static const std::string DEVELOPER_OPTIONS_KEY_NOT_PROVIDED_MESSAGE;

	public:
		Developer_OptionKeyNotProvided();
		explicit Developer_OptionKeyNotProvided(const std::string& message);
	};

}
// namespace AqualinkAutomate::Exceptions
