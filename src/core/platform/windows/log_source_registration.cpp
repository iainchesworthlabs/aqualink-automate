#include <cstring>
#include <string>

#include <windows.h>
#include <winreg.h>

#include "application/log_source_registration.h"

namespace AqualinkAutomate::Application
{

	namespace
	{
		std::string SourceKeyPath(const std::string& source_name)
		{
			return "SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\" + source_name;
		}
	}
	// namespace (anonymous)

	LogSourceRegistrationResult RegisterLogSource(const std::string& source_name)
	{
		const std::string key_path = SourceKeyPath(source_name);

		HKEY key{};
		if (ERROR_SUCCESS != ::RegCreateKeyExA(HKEY_LOCAL_MACHINE, key_path.c_str(), 0, nullptr,
			REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr))
		{
			// Almost always ERROR_ACCESS_DENIED when not running elevated.
			return LogSourceRegistrationResult::Failed;
		}

		bool ok = true;

		// EventMessageFile points at this executable. A message-table resource for
		// fully clean Event Viewer text is a deferred refinement; until then Event
		// Viewer still shows the message, wrapped in a generic description.
		char module_path[MAX_PATH]{};
		const DWORD length = ::GetModuleFileNameA(nullptr, module_path, MAX_PATH);
		if (0 < length && length < MAX_PATH)
		{
			ok = ok && (ERROR_SUCCESS == ::RegSetValueExA(key, "EventMessageFile", 0, REG_EXPAND_SZ,
				reinterpret_cast<const BYTE*>(module_path), static_cast<DWORD>(std::strlen(module_path) + 1)));
		}

		// EVENTLOG_ERROR_TYPE (1) | EVENTLOG_WARNING_TYPE (2) | EVENTLOG_INFORMATION_TYPE (4).
		DWORD types_supported = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;
		ok = ok && (ERROR_SUCCESS == ::RegSetValueExA(key, "TypesSupported", 0, REG_DWORD,
			reinterpret_cast<const BYTE*>(&types_supported), sizeof(types_supported)));

		::RegCloseKey(key);
		return ok ? LogSourceRegistrationResult::Succeeded : LogSourceRegistrationResult::Failed;
	}

	LogSourceRegistrationResult UnregisterLogSource(const std::string& source_name)
	{
		const std::string key_path = SourceKeyPath(source_name);

		const LSTATUS rc = ::RegDeleteKeyExA(HKEY_LOCAL_MACHINE, key_path.c_str(), KEY_WOW64_64KEY, 0);
		return ((ERROR_SUCCESS == rc) || (ERROR_FILE_NOT_FOUND == rc))
			? LogSourceRegistrationResult::Succeeded
			: LogSourceRegistrationResult::Failed;
	}

}
// namespace AqualinkAutomate::Application
