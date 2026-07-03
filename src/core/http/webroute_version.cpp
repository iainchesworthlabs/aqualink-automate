#include <chrono>
#include <ctime>

#include <nlohmann/json.hpp>

#include "http/webroute_version.h"
#include "http/server/server_fields.h"
#include "platform/safe_ctime.h"
#include "profiling/factories/profiling_unit_factory.h"
#include "version/version.h"

namespace AqualinkAutomate::HTTP
{
	namespace
	{
		const auto SERVER_START_TIME = std::chrono::system_clock::now();

		std::string FormatIso8601(const std::chrono::system_clock::time_point& tp)
		{
			auto time_t_val = std::chrono::system_clock::to_time_t(tp);
			std::tm tm_buf{};
			(void)Platform::SafeGmTime(tm_buf, time_t_val);
			char buf[32];
			(void)std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
			return buf;
		}
	}

	HTTP::Response WebRoute_Version::OnRequest(const HTTP::Request& req)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("WebRoute_Version::OnRequest", std::source_location::current());

		nlohmann::json version_info;

		version_info["software_version"]["name"] = Version::VersionInfo::ProjectName();
		version_info["software_version"]["version"] = Version::VersionInfo::ProjectVersionFull();
		version_info["software_version"]["description"] = Version::VersionInfo::ProjectDescription();
		version_info["software_version"]["homepage"] = Version::VersionInfo::ProjectHomepageURL();

		if (Version::GitMetadata::Populated())
		{
			version_info["git_info"]["commit_sha1"] = Version::GitMetadata::CommitSHA1();
			version_info["git_info"]["commit_date"] = Version::GitMetadata::CommitDate();
			version_info["git_info"]["uncommitted_changes"] = Version::GitMetadata::AnyUncommittedChanges();
		}

		auto now = std::chrono::system_clock::now();
		auto uptime_secs = std::chrono::duration_cast<std::chrono::seconds>(now - SERVER_START_TIME).count();

		version_info["server_start_time"] = FormatIso8601(SERVER_START_TIME);
		version_info["uptime_seconds"] = uptime_secs;

		HTTP::Response resp{ HTTP::Status::ok, req.version() };

		resp.set(boost::beast::http::field::server, ServerFields::Server());
		resp.set(boost::beast::http::field::content_type, ContentTypes::APPLICATION_JSON);
		resp.keep_alive(req.keep_alive());
		resp.body() = version_info.dump();
		resp.prepare_payload();

		zone->Value(resp.body().size());

		return resp;
	}

}
// namespace AqualinkAutomate::HTTP
