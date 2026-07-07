#include "errors/jandy_errors_auxillary_factory.h"

namespace AqualinkAutomate::ErrorCodes
{
	const Factory_ErrorCategory& Factory_ErrorCategory::Instance()
	{
		static Factory_ErrorCategory category;
		return category;
	}

	std::string_view Factory_ErrorCategory::Describe(Factory_ErrorCodes e)
	{
		using enum Factory_ErrorCodes;

		switch (e)
		{
		case Error_UnknownFactoryError:
			return "An unspecified error occurred while creating the auxillary device";

		case Error_UnknownDeviceLabel:
			return "The device label is not recognised by the auxillary factory";

		case Error_CannotCastToJandyAuxillaryId:
			return "The device identifier could not be cast to a Jandy auxillary id";

		case Error_FailedToCreateAuxillaryPtr:
			return "The auxillary device pointer could not be created";

		case Error_ReceivedInvalidAuxillaryStatus:
			return "An invalid auxillary status was received";

		default:
			return "Unknown auxillary factory error";
		}
	}
}
// namespace AqualinkAutomate::ErrorCodes

boost::system::error_code make_error_code(AqualinkAutomate::ErrorCodes::Factory_ErrorCodes e)
{
	return AqualinkAutomate::ErrorCodes::Factory_ErrorCategory::MakeErrorCode(e);
}

boost::system::error_condition make_error_condition(AqualinkAutomate::ErrorCodes::Factory_ErrorCodes e)
{
	return AqualinkAutomate::ErrorCodes::Factory_ErrorCategory::MakeErrorCondition(e);
}
