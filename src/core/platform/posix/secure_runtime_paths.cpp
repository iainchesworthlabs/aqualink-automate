#include "application/secure_runtime_paths.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace
{

	// Value of environment variable `name` if it is set and non-empty, else nullptr.
	const char* NonEmptyEnv(const char* name)
	{
		const char* v = std::getenv(name);
		return (nullptr != v && '\0' != v[0]) ? v : nullptr;
	}

}

namespace AqualinkAutomate::Application
{

	std::vector<fs::path> SecureRuntimeStateDirectories()
	{
		std::vector<fs::path> dirs;

		// 1. systemd StateDirectory= -> /var/lib/aqualink-automate, created and owned
		//    by the service account. Persistent across restarts, so a generated cert
		//    survives. STATE_DIRECTORY may be a colon-separated list; take the first.
		if (const char* sd = NonEmptyEnv("STATE_DIRECTORY"); nullptr != sd)
		{
			const std::string_view sv{ sd };
			dirs.emplace_back(std::string{ sv.substr(0, sv.find(':')) });
		}

		// 2. XDG runtime directory -- interactive user sessions. Guaranteed by the
		//    spec to be owned by, and accessible only to, the user (mode 0700).
		if (const char* xdg = NonEmptyEnv("XDG_RUNTIME_DIR"); nullptr != xdg)
		{
			dirs.emplace_back(fs::path{ xdg } / "aqualink-automate");
		}

		// 3. Per-user XDG state directory under HOME (persistent).
		if (const char* home = NonEmptyEnv("HOME"); nullptr != home)
		{
			dirs.emplace_back(fs::path{ home } / ".local" / "state" / "aqualink-automate");
		}

		return dirs;
	}

	bool PrepareSecureDirectory(const fs::path& dir)
	{
		if (dir.empty())
		{
			return false;
		}

		// Create the directory tree if absent. Parent components are created with the
		// process umask; the leaf's ownership/symlink/mode are verified and tightened
		// below, which is the actual security gate.
		std::error_code ec;
		fs::create_directories(dir, ec);

		// Inspect the LINK itself (lstat, not stat) so a symlink planted at `dir` is
		// rejected rather than followed to an attacker-chosen target.
		struct stat st{};
		if (0 != ::lstat(dir.c_str(), &st))
		{
			return false;
		}
		if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode))
		{
			return false;
		}

		// Must be owned by the current effective user. A directory pre-created by
		// another local user (a shared-path squat) is refused.
		if (st.st_uid != ::geteuid())
		{
			return false;
		}

		// Restrict to owner-only (0700), clearing any group/world bits a pre-existing
		// directory may have carried (e.g. systemd's default StateDirectoryMode=0755).
		if (0 != ::chmod(dir.c_str(), S_IRWXU))
		{
			return false;
		}

		return true;
	}

}
// namespace AqualinkAutomate::Application
