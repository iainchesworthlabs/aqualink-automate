#include "errors/jandy_errors_scrapeable.h"

namespace AqualinkAutomate::ErrorCodes
{
	const Scrapeable_ErrorCategory& Scrapeable_ErrorCategory::Instance()
	{
		static Scrapeable_ErrorCategory category;
		return category;
	}

	std::string_view Scrapeable_ErrorCategory::Describe(Scrapeable_ErrorCodes e)
	{
		using enum Scrapeable_ErrorCodes;

		switch (e)
		{
		case WaitingForPage:
			return "Waiting for the expected page to be scraped before continuing";

		case WaitingForMessage:
			return "Waiting for one or more status messages before continuing";

		case NoStepPossible:
			return "No further navigation step is possible from the current page";

		case NoGraphBeingScraped:
			return "No menu graph is currently being scraped";

		case UnknownScrapeError:
			return "An unspecified error occurred during scraping";

		case PreCommandValidationFailed:
			return "Validation of the page state before issuing the command failed";

		case PostCommandValidationFailed:
			return "Validation of the page state after issuing the command failed";

		case RecoveryInProgress:
			return "Navigation recovery is in progress (pressing Back to reach a known page)";

		case RecoveryComplete:
			return "Navigation recovery completed and a known page was reached";

		case MaxRecoveryAttemptsExceeded:
			return "The maximum number of navigation recovery attempts was exceeded";

		default:
			return "Unknown scrapeable error";
		}
	}

}
// namespace AqualinkAutomate::ErrorCodes

boost::system::error_code make_error_code(AqualinkAutomate::ErrorCodes::Scrapeable_ErrorCodes e)
{
	return AqualinkAutomate::ErrorCodes::Scrapeable_ErrorCategory::MakeErrorCode(e);
}

boost::system::error_condition make_error_condition(AqualinkAutomate::ErrorCodes::Scrapeable_ErrorCodes e)
{
	return AqualinkAutomate::ErrorCodes::Scrapeable_ErrorCategory::MakeErrorCondition(e);
}
