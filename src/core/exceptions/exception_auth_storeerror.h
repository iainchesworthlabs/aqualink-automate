#pragma once

#include <source_location>
#include <string>

#include "exceptions/exception_genericaqualinkexception.h"

namespace AqualinkAutomate::Exceptions
{

	// Thrown when the authentication subsystem cannot complete a persistence or
	// cryptographic operation it must not silently proceed past (an unreadable or
	// schema-incompatible identity store, a failed key/token generation, etc.).
	AQ_DECLARE_EXCEPTION_WITH_MESSAGE(Auth_StoreError);

}
// namespace AqualinkAutomate::Exceptions
