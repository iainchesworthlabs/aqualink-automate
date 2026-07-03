#include <algorithm>

#include "auth/entitlement_vocabulary.h"

namespace AqualinkAutomate::Auth::Vocabulary
{

	bool IsKnownAction(std::string_view action)
	{
		return std::ranges::find(ALL_ACTIONS, action) != ALL_ACTIONS.end();
	}

}
// namespace AqualinkAutomate::Auth::Vocabulary
