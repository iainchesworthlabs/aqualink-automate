#include <chrono>
#include <format>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <boost/log/sinks/event_log_backend.hpp>
#else
#include <boost/log/sinks/syslog_backend.hpp>
#endif

#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include "auth/audit_log.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Auth
{

	namespace
	{
		std::string NowIso8601Utc()
		{
			const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
			return std::format("{:%FT%TZ}", now);
		}

		nlohmann::json ToJson(const AuditEvent& event)
		{
			return {
				{ "ts", NowIso8601Utc() },
				{ "subject", event.SubjectId },
				{ "provider", event.Provider },
				{ "action", event.Action },
				{ "resource_kind", event.ResourceKind },
				{ "resource_id", event.ResourceId },
				{ "decision", event.Decision },
				{ "peer_ip", event.PeerIp },
				{ "detail", event.Detail }
			};
		}
	}
	// anonymous namespace

	AuditLog::AuditLog(Config config) :
		m_Config(std::move(config))
	{
	}

	void AuditLog::Record(const AuditEvent& event)
	{
		// Channel emission first: even if the JSONL write fails the event still
		// reaches the console log and any registered OS-native audit sink.
		LogNotify(Channel::Audit, std::format("{} subject={} provider={} action={} resource={}{}{} peer={}{}{}",
			event.Decision,
			event.SubjectId,
			event.Provider,
			event.Action,
			event.ResourceKind,
			event.ResourceId.empty() ? "" : ":",
			event.ResourceId,
			event.PeerIp,
			event.Detail.empty() ? "" : " - ",
			event.Detail));

		if (!m_Config.JsonlFile.empty())
		{
			AppendJsonl(event);
		}
	}

	void AuditLog::AppendJsonl(const AuditEvent& event)
	{
		namespace fs = std::filesystem;

		const auto line = ToJson(event).dump() + "\n";

		RotateIfNeeded(line.size());

		const bool is_new_file = !fs::exists(m_Config.JsonlFile);

		std::ofstream file(m_Config.JsonlFile, std::ios::app);

		if (!file.is_open())
		{
			LogWarning(Channel::Audit, std::format("Could not append to the audit file ({})", m_Config.JsonlFile.string()));
			return;
		}

		file << line;
		file.flush();

		if (is_new_file)
		{
			// The trail carries identities and addresses: owner-only, like every
			// other file in the auth state directory.
			std::error_code perm_ec;
			fs::permissions(m_Config.JsonlFile, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, perm_ec);
		}
	}

	void AuditLog::RotateIfNeeded(std::uintmax_t incoming_bytes)
	{
		namespace fs = std::filesystem;

		std::error_code ec;
		const auto current_size = fs::file_size(m_Config.JsonlFile, ec);

		if (ec || ((current_size + incoming_bytes) <= m_Config.MaxFileBytes))
		{
			return;
		}

		auto rotated = m_Config.JsonlFile;
		rotated += ".1";

		fs::remove(rotated, ec);
		fs::rename(m_Config.JsonlFile, rotated, ec);

		if (ec)
		{
			LogWarning(Channel::Audit, std::format("Could not rotate the audit file ({}): {}", m_Config.JsonlFile.string(), ec.message()));
		}
	}

	bool RegisterAuditOsSink()
	{
		namespace expr = boost::log::expressions;
		namespace sinks = boost::log::sinks;

		try
		{
#if defined(_WIN32)
			// Windows Event Log (Application log, source = the app name).
			auto backend = boost::make_shared<sinks::simple_event_log_backend>(boost::log::keywords::log_source = "Aqualink-Automate");

			auto sink = boost::make_shared<sinks::synchronous_sink<sinks::simple_event_log_backend>>(backend);
#else
			// syslog (journald picks this up on systemd distributions).
			auto backend = boost::make_shared<sinks::syslog_backend>(
				boost::log::keywords::facility = sinks::syslog::user,
				boost::log::keywords::use_impl = sinks::syslog::native);

			auto sink = boost::make_shared<sinks::synchronous_sink<sinks::syslog_backend>>(backend);
#endif

			sink->set_filter(channel == Channel::Audit);
			sink->set_formatter(expr::stream << expr::smessage);

			boost::log::core::get()->add_sink(sink);

			return true;
		}
		catch (const std::exception& ex)
		{
			LogWarning(Channel::Audit, std::format("Could not register the OS-native audit sink ({}); the JSONL audit file remains the durable trail", ex.what()));
			return false;
		}
	}

}
// namespace AqualinkAutomate::Auth
