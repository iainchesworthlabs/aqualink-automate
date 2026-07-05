#include <chrono>
#include <format>
#include <fstream>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

#include <boost/log/attributes/constant.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include "auth/audit_log.h"
#include "logging/logging.h"
#include "logging/logging_severity_levels.h"
#include "logging/sinks/sink_filters.h"
#include "logging/sinks/sink_native.h"

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

		// The audit trail is emitted through the Boost.Log core carrying the `IsAudit`
		// attribute (NOT a Logging::Channel). The audit native sink accepts only these
		// records; every operational sink is built with MakeOperationalFilter() and
		// rejects them (docs/logging-sinks-redesign.md §10).
		//
		// IsAudit MUST be a SOURCE attribute (added to the logger), not a per-record
		// stream `add_value`: Boost.Log evaluates sink filters at record-open time
		// against source attributes only. A streamed value reaches formatters but not
		// filters, which would let audit records leak to operational sinks.
		boost::log::sources::severity_logger_mt<Severity>& AuditLogger()
		{
			static boost::log::sources::severity_logger_mt<Severity> logger = []
			{
				boost::log::sources::severity_logger_mt<Severity> lg;
				lg.add_attribute("IsAudit", boost::log::attributes::constant<bool>(true));
				return lg;
			}();
			return logger;
		}
	}
	// anonymous namespace

	AuditLog::AuditLog(Config config) :
		m_Config(std::move(config))
	{
	}

	void AuditLog::Record(const AuditEvent& event)
	{
		// Emit to the audit trail first: even if the JSONL write fails the event
		// still reaches any installed OS-native audit sink. Denials carry Warning
		// (syslog `warning`); other outcomes carry Notify (syslog `notice`) — §10.3.
		const Severity severity = (event.Decision == "deny" || event.Decision == "failure")
			? Severity::Warning
			: Severity::Notify;

		const auto message = std::format("{} subject={} provider={} action={} resource={}{}{} peer={}{}{}",
			event.Decision,
			event.SubjectId,
			event.Provider,
			event.Action,
			event.ResourceKind,
			event.ResourceId.empty() ? "" : ":",
			event.ResourceId,
			event.PeerIp,
			event.Detail.empty() ? "" : " - ",
			event.Detail);

		BOOST_LOG_SEV(AuditLogger(), severity) << message;

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
			// A failure of the audit subsystem's own file I/O is an operational
			// diagnostic, not an audit event — surface it on an operational channel.
			LogWarning(Channel::Main, std::format("Could not append to the audit file ({})", m_Config.JsonlFile.string()));
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

		if (const auto current_size = fs::file_size(m_Config.JsonlFile, ec); ec || ((current_size + incoming_bytes) <= m_Config.MaxFileBytes))
		{
			return;
		}

		auto rotated = m_Config.JsonlFile;
		rotated += ".1";

		fs::remove(rotated, ec);
		fs::rename(m_Config.JsonlFile, rotated, ec);

		if (ec)
		{
			LogWarning(Channel::Main, std::format("Could not rotate the audit file ({}): {}", m_Config.JsonlFile.string(), ec.message()));
		}
	}

	boost::shared_ptr<boost::log::sinks::sink> RegisterAuditOsSink()
	{
		// The audit trail's OS-native destination: syslog with the LOG_AUTHPRIV
		// facility on POSIX (so distros route it to a restricted-mode auth log), or a
		// dedicated Windows Event Log source. Accepts only audit records. Returns null
		// (after a warning) when the platform sink cannot be installed — the JSONL
		// trail remains the local fallback (docs/logging-sinks-redesign.md §10). The
		// platform specifics live in the sink layer (sink_native.cpp / sink_oslog.cpp),
		// so this stays free of #ifdefs.
		auto sink = Sinks::MakeNativeSink(Sinks::NativeSinkConfig{
			.Filter = Sinks::MakeAuditFilter(),
			.Facility = Sinks::SyslogFacility::AuthPriv,
			.WindowsEventSource = "Aqualink-Automate" });

		if (sink)
		{
			boost::log::core::get()->add_sink(sink);
		}

		return sink;
	}

}
// namespace AqualinkAutomate::Auth
