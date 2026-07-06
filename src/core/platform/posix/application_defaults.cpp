#include "application/application_defaults.h"

#include <filesystem>
#include <new>
#include <string>
#include <system_error>

#include <boost/dll/runtime_symbol_info.hpp>

namespace
{

	// Resolve a bundled asset relative to the installed executable so the
	// application self-locates its web root and SSL material regardless of the
	// current working directory (e.g. when launched as a systemd service).
	//
	// Installed layout (FHS-aligned, relocatable — see cmake/CPackConfig.cmake):
	//   <prefix>/bin/aqualink-automate
	//   <prefix>/share/aqualink-automate/<relative>
	std::string AssetPath(const std::filesystem::path& relative)
	{
		try
		{
			const std::filesystem::path exe{ boost::dll::program_location().string() };
			return (exe.parent_path() / ".." / "share" / "aqualink-automate" / relative).lexically_normal().string();
		}
		catch (const std::system_error&)
		{
			// program_location can throw std::system_error if the executable path is
			// unavailable; fall back to a working-directory-relative path.
			return (std::filesystem::path("share") / "aqualink-automate" / relative).lexically_normal().string();
		}
		catch (const std::bad_alloc&)
		{
			// program_location documents std::bad_alloc on insufficient memory;
			// fall back to a working-directory-relative path.
			return (std::filesystem::path("share") / "aqualink-automate" / relative).lexically_normal().string();
		}
	}

}

namespace AqualinkAutomate::Application
{

	const std::string SERIAL_PORT{ "/dev/ttyS0" };
	const std::string DOC_ROOT{ AssetPath("web") };
	const uint16_t DEFAULT_HTTP_PORT{ 80 };
	const uint16_t DEFAULT_HTTPS_PORT{ 443 };
	const std::string DEFAULT_CERTIFICATE{ AssetPath("ssl/cert.pem") };
	const std::string DEFAULT_PRIVATE_KEY{ AssetPath("ssl/key.pem") };

}
// namespace AqualinkAutomate::Application
