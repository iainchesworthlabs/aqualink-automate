#pragma once

#include <source_location>
#include <string>

#include "exceptions/exception_genericaqualinkexception.h"

namespace AqualinkAutomate::Exceptions
{

	// Thrown when the preferences subsystem cannot read, migrate, or persist the
	// user-preferences store and must not silently drop everyone's settings.
	AQ_DECLARE_EXCEPTION_WITH_MESSAGE(Preferences_StoreError);

}
// namespace AqualinkAutomate::Exceptions
