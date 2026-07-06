#pragma once

#include <source_location>
#include <string>

#include "exceptions/exception_genericaqualinkexception.h"

namespace AqualinkAutomate::Exceptions
{

	// Thrown when the history subsystem's SQLite database cannot be opened, migrated,
	// or written; the message carries the underlying SQLite diagnostic.
	AQ_DECLARE_EXCEPTION_WITH_MESSAGE(History_DatabaseError);

}
// namespace AqualinkAutomate::Exceptions
