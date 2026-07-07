#pragma once

#include <cstdint>
#include <string>

namespace AqualinkAutomate::Version
{

	class VersionInfo
	{
		static const std::string PROJECT_NAME;
		static const std::string PROJECT_VERSION;
		static const std::string PROJECT_DESCRIPTION;
		static const std::string PROJECT_HOMEPAGE_URL;
		static const std::string PROJECT_VERSION_FULL;
		static const std::string PROJECT_VERSION_PRERELEASE;
		static const uint32_t VERSION_MAJOR;
		static const uint32_t VERSION_MINOR;
		static const uint32_t VERSION_PATCH;

	public:
		static std::string ProjectName();
		static std::string ProjectVersion();
		static std::string ProjectVersionFull();
		static std::string ProjectVersionPrerelease();
		static std::string ProjectDescription();
		static std::string ProjectHomepageURL();

		static uint32_t Major();
		static uint32_t Minor();
		static uint32_t Patch();
	};

}
// namespace AqualinkAutomate::Version
